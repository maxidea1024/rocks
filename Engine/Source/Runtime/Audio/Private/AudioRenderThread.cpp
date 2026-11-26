#include <Audio/AudioRenderThread.h>

#include <Platform/Fatal.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Memory.h>

namespace rocks::audio {

namespace {

using platform::MemoryOrder;

// pending 정렬 키(3.22.1절): (mixFrame, sequence) 오름차순.
bool Before(const AudioRtCommand& left, const AudioRtCommand& right) noexcept {
    return left.mixFrame < right.mixFrame ||
           (left.mixFrame == right.mixFrame && left.sequence < right.sequence);
}

} // namespace

AudioRenderThread::~AudioRenderThread() noexcept {
    Stop();
    Shutdown();
}

Result<void> AudioRenderThread::Initialize(Allocator& allocator, AudioMixer& mixer,
                                           AudioOutputRing& ring) noexcept {
    if (pending_ != nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto memory = allocator.Allocate(sizeof(AudioRtCommand) * AudioRtPendingCapacity,
                                     alignof(AudioRtCommand));
    if (!memory) {
        return Result<void>::Failure(memory.Error());
    }

    if (auto queue = commands_.Initialize(allocator, AudioRtCommandCapacity); !queue) {
        if (!allocator.Deallocate(memory.Value())) {
            platform::Fatal("AudioRenderThread pending deallocation was refused");
        }

        return queue;
    }

    pending_ = static_cast<AudioRtCommand*>(memory.Value());
    pendingCount_ = 0;
    // 다시 초기화하면(장치가 Null로 다시 열 때) 앞 시도의 계수·우선순위를 남기지 않는다.
    priorityKnown_.Store<MemoryOrder::Relaxed>(0);
    appliedPriority_.Store<MemoryOrder::Relaxed>(0);
    renderedBlocks_.Store<MemoryOrder::Relaxed>(0);
    pendingFullStalls_.Store<MemoryOrder::Relaxed>(0);
    stalledWaits_.Store<MemoryOrder::Relaxed>(0);
    allocator_ = &allocator;
    mixer_ = &mixer;
    ring_ = &ring;
    if (!wake_.Initialize()) {
        Shutdown();
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    return Result<void>::Success();
}

void AudioRenderThread::Shutdown() noexcept {
    if (pending_ == nullptr) {
        return;
    }

    commands_.Shutdown();
    if (!allocator_->Deallocate(pending_)) {
        platform::Fatal("AudioRenderThread pending deallocation was refused");
    }

    pending_ = nullptr;
    pendingCount_ = 0;
}

Result<void> AudioRenderThread::Submit(const AudioRtCommand& command) noexcept {
    if (pending_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return commands_.TryPush(command) ? Result<void>::Success()
                                      : Result<void>::Failure(ErrorCode::Busy);
}

Result<void> AudioRenderThread::Start() noexcept {
    if (pending_ == nullptr || running_.Load<MemoryOrder::Acquire>() != 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    stopRequested_.Store<MemoryOrder::Relaxed>(0);
    started_.Reset();
    platform::ThreadOptions options;
    options.name = "AudioRT";
    options.priority = platform::ThreadPriority::Highest;
    if (thread_.Start(&AudioRenderThread::ThreadMain, this, options) !=
        platform::ThreadStatus::Success) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    running_.Store<MemoryOrder::Release>(1);
    // 첫 바퀴가 링을 다 채운 뒤 돌아간다(backend Start 전에 링이 차 있어야 첫 콜백이 underrun이
    // 아니다).
    (void)started_.Wait();
    return Result<void>::Success();
}

void AudioRenderThread::Stop() noexcept {
    if (running_.Load<MemoryOrder::Acquire>() == 0) {
        return;
    }

    stopRequested_.Store<MemoryOrder::Release>(1);
    wake_.Signal();
    if (thread_.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("AudioRT join failed");
    }

    running_.Store<MemoryOrder::Release>(0);
}

void AudioRenderThread::ThreadMain(void* context) noexcept {
    auto& self = *static_cast<AudioRenderThread*>(context);
    platform::ThreadPriority priority = platform::ThreadPriority::Normal;
    if (platform::TryGetCurrentThreadPriority(priority)) {
        self.appliedPriority_.Store<MemoryOrder::Relaxed>(static_cast<uint32>(priority));
        self.priorityKnown_.Store<MemoryOrder::Release>(1);
    }

    platform::RunWithRealtimeAudioFloatingPoint(&AudioRenderThread::Loop, context);
}

// 링을 채우고, 차면 깨우기를 기다린다. 기다림은 깨우기 신호 하나뿐이다(3.2절).
void AudioRenderThread::Loop(void* context) noexcept {
    auto& self = *static_cast<AudioRenderThread*>(context);
    self.RenderAvailable();
    self.started_.Signal();
    while (self.stopRequested_.Load<MemoryOrder::Acquire>() == 0) {
        if (self.wake_.Wait(RenderWaitTimeoutMs) == platform::WaitResult::TimedOut &&
            !self.ring_->HasFreeSlot()) {
            self.stalledWaits_.FetchAdd<MemoryOrder::Relaxed>(1);
        }

        if (self.stopRequested_.Load<MemoryOrder::Acquire>() != 0) {
            break;
        }

        self.RenderAvailable();
    }
}

// (mixFrame, sequence) 순서를 지키며 넣는다. 같은 키의 뒤(upper bound)에 넣는다. 배열에 자리가 있을
// 때만 부른다.
void AudioRenderThread::InsertPending(const AudioRtCommand& command) noexcept {
    uint32 low = 0;
    uint32 high = pendingCount_;
    while (low < high) {
        const uint32 middle = low + (high - low) / 2;
        if (Before(command, pending_[middle])) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }

    if (low < pendingCount_) {
        platform::MemoryMove(pending_ + low + 1, pending_ + low,
                             sizeof(AudioRtCommand) * (pendingCount_ - low));
    }

    pending_[low] = command;
    ++pendingCount_;
}

// 명령 링을 pending으로 옮긴다. Immediate는 넣는 순간의 다음 블록 시작 frame으로 바꾼다(미래 명령
// 뒤에 갇히지 않게 — 3.22.1절). pending이 차면 남은 명령은 링에 둔다.
void AudioRenderThread::DrainCommands() noexcept {
    const uint64 blockStart = mixer_->MixFrame();
    while (pendingCount_ < AudioRtPendingCapacity) {
        AudioRtCommand command;
        if (!commands_.TryPop(command)) {
            return;
        }

        if (command.mixFrame == ImmediateMixFrame) {
            command.mixFrame = blockStart;
        }

        InsertPending(command);
    }

    if (!commands_.IsEmpty()) {
        pendingFullStalls_.FetchAdd<MemoryOrder::Relaxed>(1);
    }
}

void AudioRenderThread::RenderAvailable() noexcept {
    while (ring_->HasFreeSlot()) {
        DrainCommands();
        const RenderBlockResult result = mixer_->RenderBlock(
            Span<const AudioRtCommand>(pending_, pendingCount_), ring_->BeginWrite());
        if (result.consumedCommands > 0) {
            pendingCount_ -= result.consumedCommands;
            if (pendingCount_ > 0) {
                platform::MemoryMove(pending_, pending_ + result.consumedCommands,
                                     sizeof(AudioRtCommand) * pendingCount_);
            }
        }

        ring_->CommitWrite();
        renderedBlocks_.FetchAdd<MemoryOrder::Relaxed>(1);
    }
}

bool AudioRenderThread::AppliedPriority(platform::ThreadPriority& priority) const noexcept {
    if (priorityKnown_.Load<MemoryOrder::Acquire>() == 0) {
        return false;
    }

    priority = static_cast<platform::ThreadPriority>(appliedPriority_.Load<MemoryOrder::Relaxed>());
    return true;
}

AudioRenderCounters AudioRenderThread::CountersSnapshot() const noexcept {
    AudioRenderCounters counters;
    counters.renderedBlocks = renderedBlocks_.Load<MemoryOrder::Relaxed>();
    counters.pendingFullStalls = pendingFullStalls_.Load<MemoryOrder::Relaxed>();
    counters.stalledWaits = stalledWaits_.Load<MemoryOrder::Relaxed>();
    return counters;
}

} // namespace rocks::audio
