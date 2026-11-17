#include <Graphics/FenceTimeline.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>

namespace rocks::graphics {

namespace {

// 유한 대기의 남은 시간(millisecond)이다. deadline이 지났으면 0이다.
uint32 RemainingMilliseconds(uint64 deadlineNanoseconds) noexcept {
    const uint64 now = platform::MonotonicNanoseconds();
    if (now >= deadlineNanoseconds) {
        return 0;
    }

    const uint64 remaining = (deadlineNanoseconds - now + 999999u) / 1000000u;
    return remaining >= platform::InfiniteTimeout ? platform::InfiniteTimeout - 1
                                                  : static_cast<uint32>(remaining);
}

} // namespace

FenceTimeline::FenceTimeline(Allocator& allocator, uint64 deviceEpoch, QueueKind queue) noexcept
    : deviceEpoch_(deviceEpoch), queue_(queue), waiters_(allocator) {
}

FenceTimeline::~FenceTimeline() noexcept {
    StopWatcher();
    Close();
}

Result<void> FenceTimeline::StartWatcher(FenceWaitSource& source, const char* threadName) noexcept {
    {
        platform::LockGuard lock(gate_);
        if (watcherRunning_) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        source_ = &source;
        watcherRunning_ = true;
        watcherStopping_ = false;
    }

    platform::ThreadOptions options;
    options.name = threadName;
    if (watcher_.Start(&FenceTimeline::RunWatcher, this, options) !=
        platform::ThreadStatus::Success) {
        platform::LockGuard lock(gate_);
        watcherRunning_ = false;
        source_ = nullptr;
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<void>::Success();
}

void FenceTimeline::StopWatcher() noexcept {
    {
        platform::LockGuard lock(gate_);
        if (!watcherRunning_) {
            return;
        }

        watcherStopping_ = true;
        watcherWake_.NotifyAll();
    }

    if (watcher_.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("FenceTimeline watcher thread could not be joined");
    }

    platform::LockGuard lock(gate_);
    watcherRunning_ = false;
    watcherStopping_ = false;
    source_ = nullptr;
}

void FenceTimeline::RunWatcher(void* raw) noexcept {
    auto* self = static_cast<FenceTimeline*>(raw);
    FenceWaitSource* source = nullptr;
    {
        platform::LockGuard lock(self->gate_);
        source = self->source_;
    }

    source->OnWatcherStart();
    for (;;) {
        PendingSubmission entry;
        {
            platform::LockGuard lock(self->gate_);
            while (self->pendingCount_ == 0 && !self->watcherStopping_) {
                self->watcherWake_.Wait(self->gate_);
            }

            if (self->pendingCount_ == 0) {
                break;
            }

            // 맨 앞 항목은 관찰이 끝날 때까지 목록에 둔다. Submitted가 가장 최근 항목만 바꾸고
            // 맨 앞(두 개 이상일 때)은 건드리지 않는다.
            entry = self->pending_[self->pendingHead_];
        }

        const bool completed = source->WaitSubmission(entry.value, entry.token);
        source->ReleaseToken(entry.token);
        {
            platform::LockGuard lock(self->gate_);
            self->pending_[self->pendingHead_] = {};
            self->pendingHead_ = (self->pendingHead_ + 1) % MaxPendingSubmissions;
            --self->pendingCount_;
        }

        if (completed) {
            self->Publish(entry.value);
        } else {
            self->MarkLost();
        }
    }

    source->OnWatcherStop();
}

void FenceTimeline::Submitted(uint64 value, void* token) noexcept {
    void* replaced = nullptr;
    FenceWaitSource* source = nullptr;
    {
        platform::LockGuard lock(gate_);
        if (value <= submitted_) {
            platform::Fatal("FenceTimeline submissions must increase");
        }

        submitted_ = value;
        if (!watcherRunning_) {
            replaced = token;
            source = source_;
        } else if (pendingCount_ == MaxPendingSubmissions) {
            // 가장 최근 항목을 이 제출로 바꾼다. 맨 앞 항목(관찰 중일 수 있다)과 다르다.
            const usize newest = (pendingHead_ + pendingCount_ - 1) % MaxPendingSubmissions;
            replaced = pending_[newest].token;
            source = source_;
            pending_[newest] = {value, token};
        } else {
            const usize tail = (pendingHead_ + pendingCount_) % MaxPendingSubmissions;
            pending_[tail] = {value, token};
            ++pendingCount_;
            watcherWake_.NotifyAll();
        }
    }

    if (replaced != nullptr && source != nullptr) {
        source->ReleaseToken(replaced);
    }
}

void FenceTimeline::Publish(uint64 completed) noexcept {
    {
        platform::LockGuard lock(gate_);
        if (completed <= completed_) {
            return;
        }

        completed_ = completed;
        changed_.NotifyAll();
    }

    FireDecided();
}

void FenceTimeline::MarkLost() noexcept {
    {
        platform::LockGuard lock(gate_);
        lost_ = true;
        changed_.NotifyAll();
    }

    FireDecided();
}

void FenceTimeline::Close() noexcept {
    {
        platform::LockGuard lock(gate_);
        closed_ = true;
        changed_.NotifyAll();
    }

    FireDecided();
}

bool FenceTimeline::TakeDecidedLocked(Waiter& out, ErrorCode& result) noexcept {
    for (usize index = 0; index < waiters_.Size(); ++index) {
        const Waiter& waiter = waiters_[index];
        ErrorCode decided = ErrorCode::None;
        if (waiter.value <= completed_) {
            decided = ErrorCode::None;
        } else if (lost_) {
            decided = ErrorCode::Destroyed;
        } else if (closed_) {
            decided = ErrorCode::ShuttingDown;
        } else {
            continue;
        }

        out = waiter;
        result = decided;
        auto removed = waiters_.TryRemoveAtSwap(index);
        (void)removed;
        return true;
    }

    return false;
}

void FenceTimeline::FireDecided() noexcept {
    for (;;) {
        Waiter waiter;
        ErrorCode result = ErrorCode::None;
        {
            platform::LockGuard lock(gate_);
            if (!TakeDecidedLocked(waiter, result)) {
                return;
            }
        }

        waiter.callback(result, waiter.context);
    }
}

GpuFencePoint FenceTimeline::Point(uint64 value) const noexcept {
    GpuFencePoint point;
    if (value != 0) {
        point.deviceEpoch = deviceEpoch_;
        point.queue = queue_;
        point.value = value;
    }

    return point;
}

GpuFencePoint FenceTimeline::LastSubmitted() const noexcept {
    platform::LockGuard lock(gate_);
    return Point(submitted_);
}

uint64 FenceTimeline::Completed() const noexcept {
    platform::LockGuard lock(gate_);
    return completed_;
}

bool FenceTimeline::IsLost() const noexcept {
    platform::LockGuard lock(gate_);
    return lost_;
}

Result<void> FenceTimeline::CheckPoint(GpuFencePoint point) const noexcept {
    if (point.IsNull()) {
        return Result<void>::Success();
    }

    if (point.deviceEpoch != deviceEpoch_ || point.queue != queue_) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    return Result<void>::Success();
}

Result<bool> FenceTimeline::IsComplete(GpuFencePoint point) const noexcept {
    auto checked = CheckPoint(point);
    if (!checked) {
        return Result<bool>::Failure(checked.Error());
    }

    platform::LockGuard lock(gate_);
    if (point.value > submitted_) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    if (point.value <= completed_) {
        return Result<bool>::Success(true);
    }

    if (lost_) {
        return Result<bool>::Failure(ErrorCode::Destroyed);
    }

    return Result<bool>::Success(false);
}

Result<platform::WaitResult> FenceTimeline::Wait(GpuFencePoint point, uint32 timeoutMs) noexcept {
    auto checked = CheckPoint(point);
    if (!checked) {
        return Result<platform::WaitResult>::Failure(checked.Error());
    }

    const bool finite = timeoutMs != platform::InfiniteTimeout;
    const uint64 deadline =
        finite ? platform::MonotonicNanoseconds() + static_cast<uint64>(timeoutMs) * 1000000u : 0;
    platform::LockGuard lock(gate_);
    if (point.value > submitted_) {
        return Result<platform::WaitResult>::Failure(ErrorCode::InvalidArgument);
    }

    for (;;) {
        if (point.value <= completed_) {
            return Result<platform::WaitResult>::Success(platform::WaitResult::Signaled);
        }

        if (lost_) {
            return Result<platform::WaitResult>::Failure(ErrorCode::Destroyed);
        }

        uint32 slice = platform::InfiniteTimeout;
        if (finite) {
            slice = RemainingMilliseconds(deadline);
            if (slice == 0) {
                return Result<platform::WaitResult>::Success(platform::WaitResult::TimedOut);
            }
        }

        changed_.Wait(gate_, slice);
    }
}

Result<void> FenceTimeline::Notify(GpuFencePoint point, FenceCallback callback,
                                   void* context) noexcept {
    if (callback == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto checked = CheckPoint(point);
    if (!checked) {
        return checked;
    }

    ErrorCode decided = ErrorCode::None;
    {
        platform::LockGuard lock(gate_);
        if (point.value > submitted_) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (point.value <= completed_) {
            decided = ErrorCode::None;
        } else if (lost_) {
            decided = ErrorCode::Destroyed;
        } else if (closed_) {
            return Result<void>::Failure(ErrorCode::ShuttingDown);
        } else {
            auto pushed = waiters_.TryPushBack({point.value, callback, context});
            return pushed;
        }
    }

    callback(decided, context);
    return Result<void>::Success();
}

usize FenceTimeline::PendingNotifications() const noexcept {
    platform::LockGuard lock(gate_);
    return waiters_.Size();
}

} // namespace rocks::graphics
