#include <Platform/Sync.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include "WaitDeadline.h"

namespace rocks::platform {

namespace {

// 조건 변수가 큐에 들어간 뒤 짝 Mutex를 푸는 beforeWait다. 다른 함수가 잡은 잠금을 푸므로 정적 분석
// 밖이다(13.1절).
struct UnlockOnPark {
    Mutex& mutex;

    void operator()() const noexcept ROCKS_NO_THREAD_SAFETY_ANALYSIS {
        mutex.Unlock();
    }
};

} // namespace

// 시한 잠금이다(8.1절). 시한은 처음 부른 때의 단조 시계 기준이고 대기마다 남은 시간을 다시
// 계산한다.
bool Mutex::TryLockFor(uint32 timeoutMs) noexcept {
    if (Lockable::TryLock(state_)) {
        return true;
    }

    if (timeoutMs == 0) {
        return false;
    }

    if (timeoutMs == InfiniteTimeout) {
        Lockable::Lock(state_);
        return true;
    }

    const detail::WaitDeadline deadline(timeoutMs);
    return Lockable::TryLockUntil(
        state_, [&deadline]() noexcept { return deadline.RemainingMilliseconds(); });
}

// 대기 bit는 bucket 잠금 아래의 canWait 안에서 세운다(10.3절 규칙 1을 "밖에서 세우지 않음"으로
// 지킨다). 짝 Mutex는 큐에 들어간 뒤(beforeWait) 푼다. 그래서 푼 뒤 Mutex를 잡고 predicate를 바꾼
// 스레드는 그 뒤 bit를 반드시 본다(8.6절). 깨면 Mutex를 다시 잡는다.
WaitResult ConditionVariable::Wait(Mutex& mutex,
                                   uint32 timeoutMs) noexcept ROCKS_NO_THREAD_SAFETY_ANALYSIS {
    const ParkingLotWaitOutcome outcome = ParkingLot::Wait(
        &state_,
        [this]() noexcept {
            state_.FetchOr<MemoryOrder::Relaxed>(WaitingBit);
            return true;
        },
        UnlockOnPark{mutex}, timeoutMs);
    mutex.Lock();
    return outcome.woken ? WaitResult::Signaled : WaitResult::TimedOut;
}

// 콜백(bucket 잠금 아래)이 "이 주소에 남은 대기자 없음"을 받았을 때만 bit를 지운다(10.3절 규칙 2).
void ConditionVariable::NotifySlow(uint32 count) noexcept {
    ParkingLot::WakeMultiple(&state_, count, [this](ParkingLotWakeOutcome outcome) noexcept {
        if (!outcome.mayHaveMoreWaiters) {
            state_.FetchAnd<MemoryOrder::Relaxed>(static_cast<uint8>(~WaitingBit));
        }
    });
}

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
