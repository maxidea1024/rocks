#include <Platform/Sync.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include "WaitDeadline.h"

namespace rocks::platform {

ManualResetEvent::ManualResetEvent(bool signaled) noexcept : signaled_(signaled) {
}

void ManualResetEvent::Signal() noexcept {
    {
        LockGuard lock(gate_);
        signaled_ = true;
    }
    changed_.NotifyAll();
}

bool ManualResetEvent::TrySignal(uint32 attempts) noexcept {
    for (uint32 attempt = 0; attempt < attempts; ++attempt) {
        if (gate_.TryLock()) {
            signaled_ = true;
            gate_.Unlock();
            changed_.NotifyAll();
            return true;
        }

        YieldThread();
    }

    return false;
}

void ManualResetEvent::Reset() noexcept {
    LockGuard lock(gate_);
    signaled_ = false;
}

WaitResult ManualResetEvent::Wait(uint32 timeoutMs) noexcept {
    detail::WaitDeadline deadline(timeoutMs);
    LockGuard lock(gate_);
    while (!signaled_) {
        const uint32 remaining = deadline.RemainingMilliseconds();
        if (remaining == 0) {
            return WaitResult::TimedOut;
        }
        if (changed_.Wait(gate_, remaining) == WaitResult::Failed) {
            return WaitResult::Failed;
        }
    }
    return WaitResult::Signaled;
}

Semaphore::Semaphore(uint32 initialCount, uint32 maximumCount) noexcept
    : count_(initialCount), maximum_(maximumCount) {
    if (maximumCount == 0 || initialCount > maximumCount) {
        Fatal("Invalid Semaphore count bounds");
    }
}

bool Semaphore::Release(uint32 count) noexcept {
    {
        LockGuard lock(gate_);
        if (count == 0 || count > maximum_ - count_) {
            return false;
        }
        count_ += count;
    }
    if (count == 1) {
        changed_.NotifyOne();
    } else {
        changed_.NotifyAll();
    }
    return true;
}

WaitResult Semaphore::Wait(uint32 timeoutMs) noexcept {
    detail::WaitDeadline deadline(timeoutMs);
    LockGuard lock(gate_);
    while (count_ == 0) {
        const uint32 remaining = deadline.RemainingMilliseconds();
        if (remaining == 0) {
            return WaitResult::TimedOut;
        }
        if (changed_.Wait(gate_, remaining) == WaitResult::Failed) {
            return WaitResult::Failed;
        }
    }
    --count_;
    return WaitResult::Signaled;
}

} // namespace rocks::platform
