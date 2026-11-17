#include <RenderBase/RenderLane.h>

#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks::render {

namespace {

// lane 스레드 이름이 비어 있을 때 쓰는 이름이다.
constexpr char DefaultThreadName[] = "RenderLane";

// fence 완료 통지를 한 번에 꺼내 처리하는 수다(할당 없이 묶어 처리한다).
constexpr usize InboxBatch = 16;
// 장치에서 timestamp 결과를 한 번에 꺼내는 수다.
constexpr usize TimestampBatch = 32;

// 시작할 때 미리 잡는 통지 칸 수다(프레임 slot·Retire 통지가 보통 이 안에 든다).
constexpr usize InboxReserve = 64;

} // namespace

// Retire 하나의 fence 통지 문맥이다. 통지가 끝나면 스스로 해제한다.
struct RenderLane::RetireWait {
    RenderLane* lane = nullptr;
    RenderHandle handle;
};

// readback 하나의 fence 통지 문맥이다.
struct RenderLane::ReadbackWait {
    RenderLane* lane = nullptr;
    uint64 id = 0;
};

namespace {

// 갱신 작업 값이다.
struct ContentUpdate {
    RenderHandle handle;
    uint64 offset = 0;
    uint32 mipLevel = 0;
    bool texture = false;
    RenderBlob data;
};

} // namespace

} // namespace rocks::render

ROCKS_RENDER_TRANSFERABLE(rocks::render::ContentUpdate);

namespace rocks::render {

namespace {

void ApplyContentUpdate(RenderLaneContext& context, ContentUpdate& update) noexcept {
    if (update.texture) {
        auto texture = context.Handles().Texture(update.handle);
        if (texture) {
            auto updated = context.Device().UpdateTexture(texture.Value(), update.mipLevel,
                                                          update.data.View());
            if (!updated && updated.Error() != ErrorCode::InvalidArgument) {
                context.ReportFailure(updated.Error());
            }
        }
        return;
    }

    auto buffer = context.Handles().Buffer(update.handle);
    if (buffer) {
        auto updated =
            context.Device().UpdateBuffer(buffer.Value(), update.offset, update.data.View());
        if (!updated && updated.Error() != ErrorCode::InvalidArgument) {
            context.ReportFailure(updated.Error());
        }
    }
}

} // namespace

FrameTicket::FrameTicket(FrameTicket&& other) noexcept : slot_(other.slot_), frame_(other.frame_) {
    other.frame_ = 0;
}

FrameTicket& FrameTicket::operator=(FrameTicket&& other) noexcept {
    if (this != &other) {
        slot_ = other.slot_;
        frame_ = other.frame_;
        other.frame_ = 0;
    }

    return *this;
}

graphics::Device& RenderLaneContext::Device() const noexcept {
    return lane_.device_;
}

RenderHandleTable& RenderLaneContext::Handles() const noexcept {
    return lane_.handles_;
}

RenderLaneProcessor* RenderLaneContext::Processor() const noexcept {
    return lane_.config_.processor;
}

void RenderLaneContext::Notify(const RenderNotice& notice) const noexcept {
    lane_.notices_.Push(notice);
}

void RenderLaneContext::ReportFailure(ErrorCode error) const noexcept {
    lane_.RecordFailure(error == ErrorCode::None ? ErrorCode::DependencyFailed : error);
}

RenderLane::RenderLane(Allocator& allocator, graphics::Device& device) noexcept
    : allocator_(allocator), device_(device), lane_(allocator), handles_(allocator),
      notices_(allocator), readbacks_(allocator), inbox_(allocator) {
    for (uint32 index = 0; index < MaxFrameSlots; ++index) {
        slotFence_[index].lane = this;
        slotFence_[index].slot = index;
    }
}

RenderLane::~RenderLane() noexcept {
    if (running_) {
        platform::Fatal("RenderLane destroyed while running; call Shutdown or Stop first");
    }
}

bool RenderLane::IsGameThread() const noexcept {
    return platform::CurrentThreadId() == gameThread_;
}

Result<void> RenderLane::Start(const RenderLaneConfig& config) noexcept {
    if (running_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (config.frameSlots < MinFrameSlots || config.frameSlots > MaxFrameSlots ||
        (config.mode != RenderLaneMode::Thread && config.mode != RenderLaneMode::Inline)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (config.mode == RenderLaneMode::Thread && !device_.IsOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    UniquePtr<FrameArena> arenas[MaxFrameSlots];
    for (uint32 index = 0; index < config.frameSlots; ++index) {
        auto made = UniquePtr<FrameArena>::TryMake(allocator_, allocator_, config.frameArena);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        arenas[index] = Move(made.Value());
    }

    auto reserved = inbox_.TryReserve(InboxReserve);
    if (!reserved) {
        return reserved;
    }

    const char* name = config.thread.name;
    if (name == nullptr || name[0] == '\0') {
        name = DefaultThreadName;
    }

    const usize nameBytes = platform::CStringLength(name);
    if (nameBytes > platform::MaxThreadNameBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::MemoryCopy(threadName_, name, nameBytes + 1);
    DedicatedLaneConfig laneConfig;
    laneConfig.mode = config.mode == RenderLaneMode::Thread ? DedicatedLaneMode::Thread
                                                            : DedicatedLaneMode::Inline;
    laneConfig.lane = TaskLane::Render;
    laneConfig.thread = config.thread;
    laneConfig.thread.name = threadName_;
    laneConfig.launch = config.launch;
    laneConfig.launchContext = config.launchContext;
    gameThread_ = platform::CurrentThreadId();
    auto started = lane_.Start(laneConfig);
    if (!started) {
        return started;
    }

    {
        platform::LockGuard lock(gate_);
        config_ = config;
        config_.thread.name = threadName_;
        for (uint32 index = 0; index < MaxFrameSlots; ++index) {
            arenas_[index] = Move(arenas[index]);
            slots_[index] = {};
        }

        nextFrame_ = 1;
        markerPosted_ = 0;
        markerReached_ = 0;
        stats_ = {};
        failure_ = ErrorCode::None;
        startupError_ = ErrorCode::None;
        startupDone_ = false;
        ready_ = false;
        framesBlocked_ = false;
        ticketOut_ = false;
        outstandingNotifies_ = 0;
        running_ = true;
    }

    laneOwnsDevice_ = config.mode == RenderLaneMode::Inline;
    if (config.mode == RenderLaneMode::Thread) {
        // 게임 lane이 소유권을 놓고 lane이 시작 작업에서 잡는다(GX10). 그 사이 게시한 표면 작업은
        // lane이 소유권을 얻은 뒤 FIFO로 실행된다.
        auto released = device_.ReleaseOwnership();
        if (!released) {
            auto stopped = lane_.Shutdown();
            (void)stopped;
            platform::LockGuard lock(gate_);
            running_ = false;
            return released;
        }

        ownershipMoved_ = true;
    }

    auto posted = PostLaneTask([this]() noexcept { RunStartup(); });
    if (!posted) {
        // lane이 소유권을 잡지 못했다. Stop이 게임 lane으로 되찾는다.
        platform::LockGuard lock(gate_);
        startupDone_ = true;
        startupError_ = posted.Error();
        changed_.NotifyAll();
    }

    return Result<void>::Success();
}

void RenderLane::RunStartup() noexcept {
    ErrorCode error = ErrorCode::None;
    if (config_.mode == RenderLaneMode::Thread) {
        auto acquired = device_.AcquireOwnership();
        if (acquired) {
            laneOwnsDevice_ = true;
        } else {
            error = acquired.Error();
        }
    }

    if (error == ErrorCode::None && config_.processor != nullptr) {
        RenderLaneContext context(*this);
        auto started = config_.processor->OnLaneStart(context);
        if (!started) {
            error = started.Error();
        }
    }

    // 내장 자원 업로드가 GPU에서 끝난 뒤 준비 완료를 게시한다(5.5절 fence → BuiltinReady).
    if (error == ErrorCode::None) {
        const graphics::GpuFencePoint last = device_.LastSubmittedFence();
        if (!last.IsNull()) {
            auto waited = device_.WaitFence(last);
            if (!waited) {
                error = waited.Error();
            }
        }

        device_.ReclaimCompleted();
    }

    {
        platform::LockGuard lock(gate_);
        startupDone_ = true;
        startupError_ = error;
        ready_ = error == ErrorCode::None;
        changed_.NotifyAll();
    }

    if (error != ErrorCode::None) {
        RecordFailure(error);
    }
}

void RenderLane::RecordFailure(ErrorCode error) noexcept {
    bool first = false;
    {
        platform::LockGuard lock(gate_);
        if (failure_ == ErrorCode::None) {
            failure_ = error;
            first = true;
        }

        changed_.NotifyAll();
    }

    if (first) {
        RenderNotice notice;
        notice.kind = error == ErrorCode::Destroyed ? RenderNoticeKind::DeviceLost
                                                    : RenderNoticeKind::LaneFailed;
        notice.error = error;
        notices_.Push(notice);
    }
}

template <class Ready>
Result<void> RenderLane::WaitUntil(Ready&& ready) noexcept {
    if (!IsGameThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    const uint64 start = platform::MonotonicNanoseconds();
    for (;;) {
        // 이미 만족했으면 펌프하지 않고 돌아온다(대기한 경우에만 펌프한다).
        {
            platform::LockGuard lock(gate_);
            if (ready()) {
                return Result<void>::Success();
            }
        }

        if (config_.pump.pump != nullptr) {
            config_.pump.pump(config_.pump.context);
            platform::LockGuard lock(gate_);
            ++stats_.pumpCalls;
        }

        // Inline 모드는 기다리는 이 스레드가 lane이다. fence 완료 통지를 lane 작업으로 처리한다.
        if (config_.mode == RenderLaneMode::Inline && running_) {
            auto processed = PostLaneTask([this]() noexcept { ProcessInbox(); });
            (void)processed;
        }

        platform::LockGuard lock(gate_);
        if (ready()) {
            return Result<void>::Success();
        }

        uint32 slice =
            config_.pump.pump != nullptr ? FrameWaitPumpIntervalMs : platform::InfiniteTimeout;
        if (config_.hangTimeoutMs != 0) {
            const uint64 elapsedMs = (platform::MonotonicNanoseconds() - start) / 1000000u;
            if (elapsedMs >= config_.hangTimeoutMs) {
                ++stats_.hangReports;
                return Result<void>::Failure(ErrorCode::DependencyFailed);
            }

            const uint64 remaining = config_.hangTimeoutMs - elapsedMs;
            if (remaining < slice) {
                slice = static_cast<uint32>(remaining);
            }
        }

        changed_.Wait(gate_, slice);
    }
}

Result<FrameTicket> RenderLane::BeginFrame() noexcept {
    if (!IsGameThread()) {
        return Result<FrameTicket>::Failure(ErrorCode::WrongThread);
    }

    {
        platform::LockGuard lock(gate_);
        if (!running_ || framesBlocked_) {
            return Result<FrameTicket>::Failure(ErrorCode::ShuttingDown);
        }

        if (ticketOut_) {
            return Result<FrameTicket>::Failure(ErrorCode::Busy);
        }
    }

    auto started =
        WaitUntil([this]() noexcept { return startupDone_ || failure_ != ErrorCode::None; });
    if (!started) {
        return Result<FrameTicket>::Failure(started.Error());
    }

    uint32 slot = 0;
    {
        platform::LockGuard lock(gate_);
        if (startupError_ != ErrorCode::None || failure_ != ErrorCode::None) {
            return Result<FrameTicket>::Failure(startupError_ != ErrorCode::None ? startupError_
                                                                                 : failure_);
        }

        slot = static_cast<uint32>((nextFrame_ - 1) % config_.frameSlots);
        if (slots_[slot].phase != SlotPhase::Free) {
            ++stats_.slotWaits;
        }
    }

    // 빈 slot이 없으면 가장 오래된 slot(다음 차례)의 GPU 완료를 기다린다.
    auto freed = WaitUntil([this, slot]() noexcept {
        return slots_[slot].phase == SlotPhase::Free || failure_ != ErrorCode::None;
    });
    if (!freed) {
        return Result<FrameTicket>::Failure(freed.Error());
    }

    uint64 frame = 0;
    {
        platform::LockGuard lock(gate_);
        if (failure_ != ErrorCode::None) {
            return Result<FrameTicket>::Failure(failure_);
        }

        frame = nextFrame_++;
        slots_[slot].phase = SlotPhase::Recording;
        slots_[slot].frame = frame;
        slots_[slot].error = ErrorCode::None;
        ticketOut_ = true;
    }

    // lane은 그 slot의 프레임 처리를 끝냈다(GPU 완료는 lane 처리 완료 뒤다).
    auto reset = arenas_[slot]->Reset();
    if (!reset) {
        platform::LockGuard lock(gate_);
        slots_[slot].phase = SlotPhase::Free;
        ticketOut_ = false;
        return Result<FrameTicket>::Failure(reset.Error());
    }

    return Result<FrameTicket>::Success(FrameTicket(slot, frame));
}

FrameArena& RenderLane::Arena(const FrameTicket& ticket) noexcept {
    if (!ticket.IsValid() || ticket.slot_ >= config_.frameSlots || !arenas_[ticket.slot_]) {
        platform::Fatal("RenderLane::Arena called with an invalid frame ticket");
    }

    return *arenas_[ticket.slot_];
}

void RenderLane::AbandonFrame(FrameTicket&& ticket) noexcept {
    platform::LockGuard lock(gate_);
    if (ticket.IsValid() && ticket.slot_ < config_.frameSlots &&
        slots_[ticket.slot_].frame == ticket.frame_ &&
        slots_[ticket.slot_].phase == SlotPhase::Recording) {
        slots_[ticket.slot_].phase = SlotPhase::Free;
        ticketOut_ = false;
        changed_.NotifyAll();
    }

    ticket.frame_ = 0;
}

bool RenderLane::BeginSubmit(uint32 slot, uint64 frame) noexcept {
    platform::LockGuard lock(gate_);
    if (slot >= config_.frameSlots || slots_[slot].frame != frame ||
        slots_[slot].phase != SlotPhase::Recording) {
        return false;
    }

    // Inline 모드에서는 게시 안에서 처리까지 끝나므로 게시 전에 상태를 바꾼다.
    slots_[slot].phase = SlotPhase::Queued;
    ticketOut_ = false;
    ++stats_.framesSubmitted;
    ++outstandingNotifies_;
    return true;
}

void RenderLane::CancelSubmit(uint32 slot) noexcept {
    platform::LockGuard lock(gate_);
    slots_[slot].phase = SlotPhase::Recording;
    ticketOut_ = true;
    --stats_.framesSubmitted;
    --outstandingNotifies_;
    changed_.NotifyAll();
}

void RenderLane::FinishFrame(uint32 slot, uint64 frame, graphics::GpuFencePoint before) noexcept {
    const graphics::GpuFencePoint after = device_.LastSubmittedFence();
    const bool lost = device_.IsLost();
    {
        platform::LockGuard lock(gate_);
        slots_[slot].phase = SlotPhase::Processed;
        ++stats_.framesProcessed;
        changed_.NotifyAll();
    }

    if (lost) {
        RecordFailure(ErrorCode::Destroyed);
    }

    slotFence_[slot].frame = frame;
    // 이 프레임이 제출한 마지막 지점이 slot의 GPU 완료 표지다. 제출이 없으면 곧바로 완료다.
    if (after == before) {
        OnSlotFence(ErrorCode::None, &slotFence_[slot]);
        return;
    }

    auto notified = device_.NotifyFence(after, &RenderLane::OnSlotFence, &slotFence_[slot]);
    if (!notified) {
        OnSlotFence(notified.Error(), &slotFence_[slot]);
    }
}

void RenderLane::OnSlotFence(ErrorCode result, void* context) noexcept {
    auto* fence = static_cast<SlotFenceContext*>(context);
    RenderLane& lane = *fence->lane;
    InboxItem item;
    item.kind = InboxKind::FrameCompleted;
    item.error = result;
    item.slot = fence->slot;
    item.frame = fence->frame;
    lane.PushInbox(item);
    if (result != ErrorCode::None) {
        lane.RecordFailure(result);
    }

    // 마지막으로 lane을 만진다(Stop은 남은 통지가 0이 된 뒤에 끝난다).
    platform::LockGuard lock(lane.gate_);
    lane.slots_[fence->slot].phase = SlotPhase::Free;
    lane.slots_[fence->slot].error = result;
    ++lane.stats_.framesCompleted;
    --lane.outstandingNotifies_;
    lane.changed_.NotifyAll();
}

void RenderLane::PushInbox(const InboxItem& item) noexcept {
    bool wake = false;
    ErrorCode lostError = ErrorCode::None;
    {
        platform::LockGuard lock(inboxGate_);
        auto pushed = inbox_.TryPushBack(item);
        if (!pushed) {
            lostError = pushed.Error();
        } else if (!wakePending_) {
            wakePending_ = true;
            wake = true;
        }
    }

    if (lostError != ErrorCode::None) {
        // 통지를 잃으면 회수 대기가 끝나지 않는다. lane 실패로 드러낸다.
        RecordFailure(lostError);
        return;
    }

    if (wake && config_.mode == RenderLaneMode::Thread) {
        // 실패(종료 중)는 무시한다. Stop이 남은 통지를 게임 lane에서 처리한다.
        auto posted = PostLaneTask([this]() noexcept { ProcessInbox(); });
        (void)posted;
    }

    // Inline 모드에서 기다리는 게임 lane을 깨워 통지를 처리하게 한다.
    platform::LockGuard lock(gate_);
    changed_.NotifyAll();
}

void RenderLane::ProcessInbox() noexcept {
    // 장치를 가진 실행 흐름에서만 처리한다(소유권 반납 뒤의 lane 작업은 건너뛴다).
    if (!laneOwnsDevice_) {
        return;
    }

    bool reclaimed = false;
    for (;;) {
        InboxItem batch[InboxBatch];
        usize count = 0;
        {
            platform::LockGuard lock(inboxGate_);
            count = inbox_.Size() < InboxBatch ? inbox_.Size() : InboxBatch;
            for (usize index = 0; index < count; ++index) {
                batch[index] = inbox_[index];
            }

            if (count != 0) {
                auto removed = inbox_.TryRemoveAt(0, count);
                (void)removed;
            }

            if (inbox_.IsEmpty()) {
                wakePending_ = false;
            }
        }

        if (count == 0) {
            return;
        }

        // fence가 끝난 회수 대기 자원을 먼저 지운 뒤 통지한다(ResourceRetired는 해제 뒤다).
        if (!reclaimed) {
            device_.ReclaimCompleted();
            reclaimed = true;
        }

        for (usize index = 0; index < count; ++index) {
            const InboxItem& item = batch[index];
            RenderNotice notice;
            notice.error = item.error;
            if (item.kind == InboxKind::ReadbackCompleted) {
                FinishReadback(item.frame, item.error);
                continue;
            }

            if (item.kind == InboxKind::FrameCompleted) {
                PublishTimestamps();
                notice.kind = RenderNoticeKind::FrameCompleted;
                notice.frame = item.frame;
                notices_.Push(notice);
                continue;
            }

            auto removed = handles_.Remove(item.handle);
            (void)removed;
            notice.kind = RenderNoticeKind::ResourceRetired;
            notice.handle = item.handle;
            notices_.Push(notice);
            platform::LockGuard lock(gate_);
            --stats_.retiresPending;
            ++stats_.retiresCompleted;
            changed_.NotifyAll();
        }
    }
}

void RenderLane::PublishTimestamps() noexcept {
    graphics::TimestampSample samples[TimestampBatch];
    for (;;) {
        const usize count = device_.CollectTimestamps(Span<graphics::TimestampSample>(samples));
        for (usize index = 0; index < count; ++index) {
            RenderNotice notice;
            notice.kind = RenderNoticeKind::GpuTimestamp;
            notice.frame = samples[index].query;
            notice.value = samples[index].ticks;
            notice.error = samples[index].disjoint ? ErrorCode::Cancelled : ErrorCode::None;
            notices_.Push(notice);
        }

        if (count < TimestampBatch) {
            return;
        }
    }
}

Result<RenderHandle> RenderLane::ReserveHandle(RenderResourceKind kind) noexcept {
    return handles_.Reserve(kind);
}

Result<void> RenderLane::Retire(RenderHandle handle) noexcept {
    auto state = handles_.State(handle);
    if (!state) {
        return Result<void>::Failure(state.Error());
    }

    return PostLaneTask([this, handle]() noexcept {
        ProcessInbox();
        RunRetire(handle);
    });
}

void RenderLane::RunRetire(RenderHandle handle) noexcept {
    auto retired = handles_.BeginRetire(handle);
    if (!retired) {
        return; // 이미 Retire한 handle의 중복 요청이다.
    }

    const SlotHandle device = retired.Value().device;
    if (!device.IsNull()) {
        switch (retired.Value().kind) {
        case RenderResourceKind::Buffer:
            device_.Retire(graphics::BufferHandle{device});
            break;
        case RenderResourceKind::Texture:
            device_.Retire(graphics::TextureHandle{device});
            break;
        case RenderResourceKind::Pipeline:
            device_.Retire(graphics::PipelineHandle{device});
            break;
        }
    }

    {
        platform::LockGuard lock(gate_);
        ++stats_.retiresPending;
        ++outstandingNotifies_;
    }

    // lane 작업 사이에는 기록 중인 프레임이 없으므로 장치 내부 last-use는 지금까지의 마지막 제출
    // 이하다. 그 지점이 끝나면 장치가 해제할 수 있다.
    const graphics::GpuFencePoint point = device_.LastSubmittedFence();
    auto memory = allocator_.Allocate(sizeof(RetireWait), alignof(RetireWait));
    if (!memory) {
        // 통지 문맥을 만들 수 없다. 이 lane에서 GPU 완료를 기다린 뒤 끝낸다.
        auto waited = device_.WaitFence(point);
        InboxItem item;
        item.kind = InboxKind::RetireCompleted;
        item.error = waited ? ErrorCode::None : waited.Error();
        item.handle = handle;
        PushInbox(item);
        platform::LockGuard lock(gate_);
        --outstandingNotifies_;
        changed_.NotifyAll();
        return;
    }

    auto* wait = ConstructAt<RetireWait>(memory.Value());
    wait->lane = this;
    wait->handle = handle;
    auto notified = device_.NotifyFence(point, &RenderLane::OnRetireFence, wait);
    if (!notified) {
        OnRetireFence(notified.Error(), wait);
    }
}

void RenderLane::OnRetireFence(ErrorCode result, void* context) noexcept {
    auto* wait = static_cast<RetireWait*>(context);
    RenderLane& lane = *wait->lane;
    InboxItem item;
    item.kind = InboxKind::RetireCompleted;
    item.error = result;
    item.handle = wait->handle;
    DestroyAt(wait);
    (void)lane.allocator_.Deallocate(wait);
    lane.PushInbox(item);
    platform::LockGuard lock(lane.gate_);
    --lane.outstandingNotifies_;
    lane.changed_.NotifyAll();
}

Result<void> RenderLane::UpdateBuffer(RenderHandle buffer, uint64 offset,
                                      RenderBlob&& data) noexcept {
    auto state = handles_.State(buffer);
    if (!state) {
        return Result<void>::Failure(state.Error());
    }

    ContentUpdate update;
    update.handle = buffer;
    update.offset = offset;
    update.data = Move(data);
    return PostResource(Move(update), &ApplyContentUpdate);
}

Result<void> RenderLane::UpdateTexture(RenderHandle texture, uint32 mipLevel,
                                       RenderBlob&& pixels) noexcept {
    auto state = handles_.State(texture);
    if (!state) {
        return Result<void>::Failure(state.Error());
    }

    ContentUpdate update;
    update.handle = texture;
    update.mipLevel = mipLevel;
    update.texture = true;
    update.data = Move(pixels);
    return PostResource(Move(update), &ApplyContentUpdate);
}

Result<uint64> RenderLane::RequestReadback(RenderHandle texture) noexcept {
    auto state = handles_.State(texture);
    if (!state) {
        return Result<uint64>::Failure(state.Error());
    }

    uint64 id = 0;
    {
        platform::LockGuard lock(readbackGate_);
        ReadbackEntry entry;
        entry.id = nextReadback_++;
        entry.texture = texture;
        auto pushed = readbacks_.TryPushBack(Move(entry));
        if (!pushed) {
            return Result<uint64>::Failure(pushed.Error());
        }

        id = readbacks_[readbacks_.Size() - 1].id;
    }

    auto posted = PostLaneTask([this, id, texture]() noexcept {
        ProcessInbox();
        RunReadback(id, texture);
    });
    if (!posted) {
        platform::LockGuard lock(readbackGate_);
        auto removed = readbacks_.TryRemoveAt(readbacks_.Size() - 1);
        (void)removed;
        return Result<uint64>::Failure(posted.Error());
    }

    return Result<uint64>::Success(id);
}

void RenderLane::RunReadback(uint64 id, RenderHandle texture) noexcept {
    auto target = handles_.Texture(texture);
    if (!target) {
        FinishReadback(id, target.Error());
        return;
    }

    auto requested = device_.RequestReadback(target.Value());
    if (!requested) {
        FinishReadback(id, requested.Error());
        return;
    }

    {
        platform::LockGuard lock(readbackGate_);
        for (ReadbackEntry& entry : readbacks_) {
            if (entry.id == id) {
                entry.device = requested.Value().handle;
                entry.bytes = requested.Value().bytes;
            }
        }
    }

    auto memory = allocator_.Allocate(sizeof(ReadbackWait), alignof(ReadbackWait));
    if (!memory) {
        // 통지 문맥을 만들 수 없다. 이 lane에서 GPU 완료를 기다린 뒤 끝낸다.
        auto waited = device_.WaitFence(requested.Value().completion);
        FinishReadback(id, waited ? ErrorCode::None : waited.Error());
        return;
    }

    {
        platform::LockGuard lock(gate_);
        ++outstandingNotifies_;
    }

    auto* wait = ConstructAt<ReadbackWait>(memory.Value());
    wait->lane = this;
    wait->id = id;
    auto notified =
        device_.NotifyFence(requested.Value().completion, &RenderLane::OnReadbackFence, wait);
    if (!notified) {
        OnReadbackFence(notified.Error(), wait);
    }
}

void RenderLane::OnReadbackFence(ErrorCode result, void* context) noexcept {
    auto* wait = static_cast<ReadbackWait*>(context);
    RenderLane& lane = *wait->lane;
    InboxItem item;
    item.kind = InboxKind::ReadbackCompleted;
    item.error = result;
    item.frame = wait->id;
    DestroyAt(wait);
    (void)lane.allocator_.Deallocate(wait);
    lane.PushInbox(item);
    platform::LockGuard lock(lane.gate_);
    --lane.outstandingNotifies_;
    lane.changed_.NotifyAll();
}

// lane에서 readback 결과를 보관하고 통지한다(장치를 가진 실행 흐름).
void RenderLane::FinishReadback(uint64 id, ErrorCode error) noexcept {
    graphics::ReadbackHandle device;
    uint64 bytes = 0;
    RenderHandle texture;
    {
        platform::LockGuard lock(readbackGate_);
        for (ReadbackEntry& entry : readbacks_) {
            if (entry.id == id) {
                device = entry.device;
                bytes = entry.bytes;
                texture = entry.texture;
            }
        }
    }

    RenderBlob result;
    if (error == ErrorCode::None && !device.IsNull()) {
        auto buffer = allocator_.Allocate(static_cast<usize>(bytes), 16);
        if (!buffer) {
            error = buffer.Error();
        } else {
            auto copied =
                device_.TryReadback(device, Span<uint8>(static_cast<uint8*>(buffer.Value()),
                                                        static_cast<usize>(bytes)));
            if (!copied || !copied.Value()) {
                error = copied ? ErrorCode::DependencyFailed : copied.Error();
            } else {
                auto blob = RenderBlob::TryCopy(
                    allocator_, Span<const uint8>(static_cast<const uint8*>(buffer.Value()),
                                                  static_cast<usize>(bytes)));
                if (blob) {
                    result = Move(blob.Value());
                } else {
                    error = blob.Error();
                }
            }

            (void)allocator_.Deallocate(buffer.Value());
        }
    }

    if (!device.IsNull()) {
        device_.ReleaseReadback(device);
    }

    {
        platform::LockGuard lock(readbackGate_);
        for (ReadbackEntry& entry : readbacks_) {
            if (entry.id == id) {
                entry.result = Move(result);
                entry.ready = true;
                entry.device = {};
            }
        }
    }

    RenderNotice notice;
    notice.kind = RenderNoticeKind::ReadbackReady;
    notice.error = error;
    notice.frame = id;
    notice.handle = texture;
    notices_.Push(notice);
}

Result<RenderBlob> RenderLane::TakeReadback(uint64 readback) noexcept {
    platform::LockGuard lock(readbackGate_);
    for (usize index = 0; index < readbacks_.Size(); ++index) {
        if (readbacks_[index].id == readback && readbacks_[index].ready) {
            RenderBlob result = Move(readbacks_[index].result);
            auto removed = readbacks_.TryRemoveAt(index);
            (void)removed;
            return Result<RenderBlob>::Success(Move(result));
        }
    }

    return Result<RenderBlob>::Failure(ErrorCode::NotFound);
}

Result<LaneMarker> RenderLane::PostMarker() noexcept {
    uint64 sequence = 0;
    {
        platform::LockGuard lock(gate_);
        if (!running_) {
            return Result<LaneMarker>::Failure(ErrorCode::ShuttingDown);
        }

        sequence = ++markerPosted_;
    }

    auto posted = PostLaneTask([this, sequence]() noexcept {
        ProcessInbox();
        platform::LockGuard lock(gate_);
        if (sequence > markerReached_) {
            markerReached_ = sequence;
        }

        changed_.NotifyAll();
    });
    if (!posted) {
        return Result<LaneMarker>::Failure(posted.Error());
    }

    return Result<LaneMarker>::Success(LaneMarker{sequence});
}

bool RenderLane::IsReached(LaneMarker marker) const noexcept {
    platform::LockGuard lock(gate_);
    return markerReached_ >= marker.sequence;
}

Result<void> RenderLane::WaitMarker(LaneMarker marker) noexcept {
    return WaitUntil([this, marker]() noexcept { return markerReached_ >= marker.sequence; });
}

Result<void> RenderLane::Drain(RenderDrainReason reason) noexcept {
    if (!IsGameThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    const auto index = static_cast<uint32>(reason);
    if (index >= RenderDrainReasonCount) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    {
        platform::LockGuard lock(gate_);
        ++stats_.drains[index];
    }

    auto marker = PostMarker();
    if (!marker) {
        return Result<void>::Failure(marker.Error());
    }

    return WaitMarker(marker.Value());
}

bool RenderLane::IsExecutingLaneTask() const noexcept {
    return lane_.IsExecutingTask();
}

ErrorCode RenderLane::Failure() const noexcept {
    platform::LockGuard lock(gate_);
    return failure_;
}

bool RenderLane::IsReady() const noexcept {
    platform::LockGuard lock(gate_);
    return ready_;
}

RenderLaneStats RenderLane::Stats() const noexcept {
    const DedicatedLaneStats laneStats = lane_.Stats();
    platform::LockGuard lock(gate_);
    RenderLaneStats stats = stats_;
    stats.tasksPosted = laneStats.posted;
    stats.tasksCompleted = laneStats.completed;
    stats.running = running_;
    return stats;
}

void RenderLane::BlockFrames() noexcept {
    platform::LockGuard lock(gate_);
    framesBlocked_ = true;
    changed_.NotifyAll();
}

Result<void> RenderLane::WaitFramesIdle() noexcept {
    if (!running_) {
        return Result<void>::Success();
    }

    auto idle = WaitUntil([this]() noexcept {
        for (uint32 index = 0; index < config_.frameSlots; ++index) {
            if (slots_[index].phase == SlotPhase::Queued ||
                slots_[index].phase == SlotPhase::Processed) {
                return false;
            }
        }

        return true;
    });
    if (!idle) {
        return idle;
    }

    const ErrorCode failure = Failure();
    if (failure != ErrorCode::None) {
        return Result<void>::Failure(failure);
    }

    return Result<void>::Success();
}

Result<uint64> RenderLane::DrainRetires() noexcept {
    if (!running_) {
        return Result<uint64>::Success(0);
    }

    uint64 before = 0;
    {
        platform::LockGuard lock(gate_);
        before = stats_.retiresCompleted;
    }

    // 지금까지 게시한 Retire 작업을 lane이 모두 처리(장치 Retire·fence 통지 등록)하게 한 뒤 그 GPU
    // 완료와 회수를 기다린다.
    auto marker = PostMarker();
    if (!marker) {
        return Result<uint64>::Failure(marker.Error());
    }

    auto reached = WaitMarker(marker.Value());
    if (!reached) {
        return Result<uint64>::Failure(reached.Error());
    }

    auto drained = WaitUntil([this]() noexcept { return stats_.retiresPending == 0; });
    if (!drained) {
        return Result<uint64>::Failure(drained.Error());
    }

    platform::LockGuard lock(gate_);
    return Result<uint64>::Success(stats_.retiresCompleted - before);
}

Result<void> RenderLane::CloseResources() noexcept {
    if (!running_) {
        return Result<void>::Success();
    }

    // 4·5단계: 처리기가 내장 자원·스왑체인을 Retire하고, 장치 WaitIdle로 회수 완료를 기다린다.
    auto posted = PostLaneTask([this]() noexcept {
        ProcessInbox();
        if (!laneOwnsDevice_) {
            return;
        }

        if (config_.processor != nullptr) {
            RenderLaneContext context(*this);
            config_.processor->OnLaneStop(context);
        }

        auto idle = device_.WaitIdle();
        device_.ReclaimCompleted();
        ProcessInbox();
        const usize pending = device_.PendingRetireCount();
        platform::LockGuard lock(gate_);
        closeError_ = idle ? ErrorCode::None : idle.Error();
        closeDevicePending_ = pending;
        changed_.NotifyAll();
    });
    if (!posted) {
        return posted;
    }

    // OnLaneStop이 게시한 Retire까지 끝낸다.
    auto drained = DrainRetires();
    if (!drained) {
        return Result<void>::Failure(drained.Error());
    }

    platform::LockGuard lock(gate_);
    if (closeError_ != ErrorCode::None) {
        return Result<void>::Failure(closeError_);
    }

    // closure 확인: 남은 렌더 handle·Retire 대기·장치 회수 대기가 없어야 한다.
    stats_.liveHandles = handles_.Count();
    stats_.devicePendingRetires = closeDevicePending_;
    if (stats_.liveHandles != 0 || stats_.retiresPending != 0 || closeDevicePending_ != 0) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    return Result<void>::Success();
}

Result<void> RenderLane::Stop() noexcept {
    if (!running_) {
        return Result<void>::Success();
    }

    if (!IsGameThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    // 6단계: 남은 lane 작업과 장치 fence 통지를 모두 끝낸다.
    Result<void> outcome = Drain(RenderDrainReason::Shutdown);
    auto settled = WaitUntil([this]() noexcept { return outstandingNotifies_ == 0; });
    if (!settled && outcome) {
        outcome = Move(settled);
    }

    // 7단계: lane이 장치 소유권을 놓고, lane을 닫은 뒤 게임 lane이 다시 잡는다.
    if (ownershipMoved_) {
        auto released = PostLaneTask([this]() noexcept {
            ProcessInbox();
            if (laneOwnsDevice_) {
                auto dropped = device_.ReleaseOwnership();
                (void)dropped;
                laneOwnsDevice_ = false;
            }
        });
        (void)released;
    }

    auto shut = lane_.Shutdown();
    if (!shut && outcome) {
        outcome = Move(shut);
    }

    if (ownershipMoved_) {
        auto acquired = device_.AcquireOwnership();
        platform::LockGuard lock(gate_);
        stats_.ownershipReturned = acquired.IsOk();
        if (!acquired && outcome) {
            outcome = Move(acquired);
        }

        ownershipMoved_ = false;
    }

    // lane이 닫힌 뒤 도착한 통지가 있으면 장치를 다시 가진 이 스레드에서 처리한다.
    laneOwnsDevice_ = true;
    ProcessInbox();
    laneOwnsDevice_ = false;
    const DedicatedLaneStats laneStats = lane_.Stats();
    platform::LockGuard lock(gate_);
    running_ = false;
    if (laneStats.posted != laneStats.completed && outcome) {
        outcome = Result<void>::Failure(ErrorCode::Busy);
    }

    return outcome;
}

Result<void> RenderLane::Shutdown() noexcept {
    if (!running_) {
        return Result<void>::Success();
    }

    Result<void> outcome = Result<void>::Success();
    auto keep = [&outcome](Result<void>&& step) noexcept {
        if (!step && outcome) {
            outcome = Move(step);
        }
    };
    BlockFrames();
    keep(WaitFramesIdle());
    for (;;) {
        auto drained = DrainRetires();
        if (!drained) {
            keep(Result<void>::Failure(drained.Error()));
            break;
        }

        if (drained.Value() == 0) {
            break;
        }
    }

    keep(CloseResources());
    keep(Stop());
    return outcome;
}

} // namespace rocks::render
