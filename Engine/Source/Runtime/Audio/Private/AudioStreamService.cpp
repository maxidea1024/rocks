#include <Audio/AudioStreamService.h>

#include <Audio/AudioMixerTypes.h>
#include <Core/Assert.h>
#include <Core/Move.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>

// 스트림 디코드 서비스다(Docs/Design/AudioSystem 3.40.2절).
namespace rocks::audio {

using platform::AtomicLoad;
using platform::AtomicStore;
using platform::MemoryOrder;

namespace {

// 작업 번호 0은 "작업 없음"이다.
constexpr uint64 NoStreamJob = 0;

// 디코드 쪽이 큐를 비우는 사이 한 슬롯에 쌓일 수 있는 명령 수다(Docs/Design/AudioSystem 3.40.2절):
// 앞 작업의 늦은 Cancel(반납 확인 전에 넣은 것), 새 Start, 새 Cancel. 새 작업은 앞 작업이 반납된
// 뒤에만 시작하고 반납은 디코드 쪽이 그 Start를 꺼낸 뒤라, 그보다 앞 작업의 명령은 이미 꺼냈다.
constexpr usize StreamCommandsPerSlot = 3;

// 큐 용량(2의 거듭제곱)이다.
usize StreamQueueCapacity(uint32 maxStreams) noexcept {
    usize capacity = 2;
    while (capacity < static_cast<usize>(maxStreams) * StreamCommandsPerSlot) {
        capacity *= 2;
    }

    return capacity;
}

} // namespace

Result<UniquePtr<AudioStreamService>>
AudioStreamService::TryCreate(Allocator& allocator,
                              const AudioStreamServiceConfig& config) noexcept {
    using Made = Result<UniquePtr<AudioStreamService>>;
    if (config.maxStreams == 0 || config.maxStreams > MaxStreamSlots ||
        config.ringFrames < StreamRingMinFrames || config.ringFrames > StreamRingMaxFrames) {
        return Made::Failure(ErrorCode::InvalidArgument);
    }

    auto made = UniquePtr<AudioStreamService>::TryMake(allocator, allocator, config);
    if (!made) {
        return Made::Failure(made.Error());
    }

    AudioStreamService& service = *made.Value();
    if (!service.wake_.Initialize()) {
        return Made::Failure(ErrorCode::Unsupported);
    }

    const uint32 count = config.maxStreams;
    if (!service.rings_.TryReserve(count) || !service.control_.TryResize(count) ||
        !service.active_.TryResize(count) || !service.returned_.TryResize(count) ||
        !service.order_.TryReserve(count)) {
        return Made::Failure(ErrorCode::OutOfMemory);
    }

    for (uint32 slot = 0; slot < count; ++slot) {
        auto ring = AudioStreamRing::TryCreate(allocator, MaxClipChannels, config.ringFrames);
        if (!ring) {
            return Made::Failure(ring.Error());
        }

        (void)service.rings_.TryPushBack(Move(ring.Value()));
        service.returned_[slot] = NoStreamJob;
    }

    if (auto queue = service.commands_.Initialize(allocator, StreamQueueCapacity(count)); !queue) {
        return Made::Failure(queue.Error());
    }

    return made;
}

AudioStreamService::AudioStreamService(Allocator& allocator,
                                       const AudioStreamServiceConfig& config) noexcept
    : config_(config), rings_(allocator), control_(allocator), active_(allocator),
      returned_(allocator), order_(allocator) {
}

AudioStreamService::~AudioStreamService() noexcept {
    Shutdown();
}

Result<void> AudioStreamService::Start() noexcept {
    if (config_.mode == AudioStreamServiceMode::Inline || threadStarted_) {
        return Result<void>::Success();
    }

    AtomicStore<MemoryOrder::Release>(&running_, 1u);
    platform::ThreadOptions options;
    options.name = "AudioDecode";
    options.priority = platform::ThreadPriority::AboveNormal;
    if (thread_.Start(&AudioStreamService::ThreadMain, this, options) !=
        platform::ThreadStatus::Success) {
        AtomicStore<MemoryOrder::Release>(&running_, 0u);
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    threadStarted_ = true;
    return Result<void>::Success();
}

void AudioStreamService::Shutdown() noexcept {
    if (threadStarted_) {
        AtomicStore<MemoryOrder::Release>(&running_, 0u);
        wake_.Signal();
        if (thread_.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("AudioStreamService decode thread join failed");
        }

        threadStarted_ = false;
    }

    // 스레드가 없으므로 남은 큐와 활성 작업을 반납한 것으로 둔다(제어 쪽이 슬롯을 회수할 수 있게).
    Command command;
    while (commands_.TryPop(command)) {
        if (command.type == CommandType::Start) {
            Publish(command.slot, command.job);
        }
    }

    for (uint32 slot = 0; slot < active_.Size(); ++slot) {
        if (active_[slot].decoder != nullptr) {
            active_[slot].decoder->ReleaseChunk();
            Publish(slot, active_[slot].job);
            active_[slot] = ActiveSlot{};
        }
    }
}

Result<uint32> AudioStreamService::AcquireSlot(uint32 channels) noexcept {
    if (channels == 0 || channels > MaxClipChannels) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    for (uint32 slot = 0; slot < control_.Size(); ++slot) {
        ControlSlot& record = control_[slot];
        if (!record.acquired) {
            record = ControlSlot{};
            record.acquired = true;
            rings_[slot]->Reset(channels);
            return Result<uint32>::Success(slot);
        }
    }

    return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
}

AudioStreamRing& AudioStreamService::Ring(uint32 slot) noexcept {
    ROCKS_ASSERT(slot < rings_.Size());
    return *rings_[slot];
}

Result<void> AudioStreamService::Enqueue(const Command& command) noexcept {
    if (!commands_.TryPush(command)) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    wake_.Signal();
    return Result<void>::Success();
}

Result<void> AudioStreamService::StartJob(uint32 slot, AudioStreamDecoder* decoder) noexcept {
    if (slot >= control_.Size() || !control_[slot].acquired || control_[slot].started ||
        decoder == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ControlSlot& record = control_[slot];
    const uint64 job = ++nextJob_;
    Command command;
    command.type = CommandType::Start;
    command.slot = slot;
    command.job = job;
    command.decoder = decoder;
    if (auto queued = Enqueue(command); !queued) {
        return queued;
    }

    record.job = job;
    record.started = true;
    return Result<void>::Success();
}

void AudioStreamService::CancelJob(uint32 slot) noexcept {
    if (slot >= control_.Size() || !control_[slot].started || control_[slot].cancelled) {
        return;
    }

    ControlSlot& record = control_[slot];
    record.cancelled = true;
    // 이미 반납된 작업(끝까지 썼거나 실패)에는 보낼 것이 없다.
    if (AtomicLoad<MemoryOrder::Acquire>(&returned_[slot]) == record.job) {
        return;
    }

    Command command;
    command.type = CommandType::Cancel;
    command.slot = slot;
    command.job = record.job;
    // 큐는 슬롯마다 StreamCommandsPerSlot개까지 담으므로 넘치지 않는다.
    if (!Enqueue(command)) {
        platform::Fatal("AudioStreamService command queue overflowed");
    }
}

bool AudioStreamService::IsReturned(uint32 slot) const noexcept {
    if (slot >= control_.Size()) {
        return false;
    }

    const ControlSlot& record = control_[slot];
    if (!record.started) {
        return true;
    }

    return AtomicLoad<MemoryOrder::Acquire>(&returned_[slot]) == record.job;
}

Result<void> AudioStreamService::ReleaseSlot(uint32 slot) noexcept {
    if (slot >= control_.Size() || !control_[slot].acquired) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!IsReturned(slot)) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    control_[slot] = ControlSlot{};
    return Result<void>::Success();
}

uint32 AudioStreamService::AcquiredSlots() const noexcept {
    uint32 count = 0;
    for (const ControlSlot& record : control_) {
        count += record.acquired ? 1u : 0u;
    }

    return count;
}

void AudioStreamService::Publish(uint32 slot, uint64 job) noexcept {
    // 이 뒤로 디코드 쪽은 그 작업의 디코더·링을 건드리지 않는다.
    AtomicStore<MemoryOrder::Release>(&returned_[slot], job);
}

bool AudioStreamService::ServicePass() noexcept {
    Command command;
    while (commands_.TryPop(command)) {
        ActiveSlot& active = active_[command.slot];
        if (command.type == CommandType::Start) {
            active.decoder = command.decoder;
            active.job = command.job;
        } else if (active.job == command.job && active.decoder != nullptr) {
            // 청크는 반납 전에 이 스레드가 놓는다(AudioChunkSource 스레드 계약).
            active.decoder->ReleaseChunk();
            active = ActiveSlot{};
            Publish(command.slot, command.job);
        }
    }

    // 읽을 frame이 적은 슬롯부터 채운다(굶음에 가까운 스트림 먼저).
    order_.Clear();
    for (uint32 slot = 0; slot < active_.Size(); ++slot) {
        if (active_[slot].decoder != nullptr) {
            (void)order_.TryPushBack(slot);
        }
    }

    for (usize i = 1; i < order_.Size(); ++i) {
        const uint32 key = order_[i];
        const uint32 keyReadable = rings_[key]->ReadableFrames();
        usize j = i;
        while (j > 0 && rings_[order_[j - 1]]->ReadableFrames() > keyReadable) {
            order_[j] = order_[j - 1];
            --j;
        }

        order_[j] = key;
    }

    bool remaining = false;
    for (const uint32 slot : order_) {
        ActiveSlot& active = active_[slot];
        active.decoder->Pump(*rings_[slot]);
        if (active.decoder->IsFinished()) {
            const uint64 job = active.job;
            active = ActiveSlot{};
            Publish(slot, job);
        } else {
            remaining = true;
        }
    }

    return remaining;
}

void AudioStreamService::PumpAll() noexcept {
    ROCKS_ASSERT(config_.mode == AudioStreamServiceMode::Inline);
    (void)ServicePass();
}

Result<void> AudioStreamService::Flush() noexcept {
    if (config_.mode == AudioStreamServiceMode::Inline) {
        PumpAll();
        return Result<void>::Success();
    }

    if (!threadStarted_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    flushDone_.Reset();
    const uint64 requested = AtomicLoad<MemoryOrder::Relaxed>(&flushRequested_) + 1;
    AtomicStore<MemoryOrder::Release>(&flushRequested_, requested);
    wake_.Signal();
    if (flushDone_.Wait() != platform::WaitResult::Signaled) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    return Result<void>::Success();
}

void AudioStreamService::ThreadMain(void* context) noexcept {
    static_cast<AudioStreamService*>(context)->Loop();
}

void AudioStreamService::Loop() noexcept {
    bool active = false;
    while (AtomicLoad<MemoryOrder::Acquire>(&running_) != 0) {
        (void)wake_.Wait(active ? StreamPumpTimeoutMs : platform::InfiniteTimeout);
        if (AtomicLoad<MemoryOrder::Acquire>(&running_) == 0) {
            break;
        }

        // 요청을 먼저 읽는다: 이 뒤의 처리 한 번이 요청 전에 넣은 큐·데이터를 모두 본다.
        const uint64 requested = AtomicLoad<MemoryOrder::Acquire>(&flushRequested_);
        active = ServicePass();
        if (requested != flushServed_) {
            flushServed_ = requested;
            flushDone_.Signal();
        }
    }
}

} // namespace rocks::audio
