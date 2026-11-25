#pragma once

#include <Base/Types.h>
#include <Platform/Atomics.h>

namespace rocks::platform::detail {

// ParkingLot 안에서만 쓰는 포인터 크기 잠금이다(UE FWordMutex·WebKit WordLock 대응,
// Docs/Design/Synchronization 10.2절). ParkingLot bucket과 영구 할당기를 지킨다. ParkingLot 위의
// Mutex를 쓸 수 없는 자리라(그 Mutex가 ParkingLot을 부른다) 대기 노드를 대기 스레드 스택에 두고
// 노드마다 WaiterSignal로 잔다. 상수 초기화되고 파괴자가 없다.
//
// 상태 word: bit 0 잠김, bit 1 큐 잠김(대기열을 고치는 중), 나머지 bit는 대기열 머리 노드 주소다
// (노드는 4 byte 이상 정렬). 대기자가 없을 때만 CpuPause로 잠깐 돈다. 공정하지 않다.
class ROCKS_PLATFORMBASE_API QueueMutex {
public:
    constexpr QueueMutex() noexcept = default;
    QueueMutex(const QueueMutex&) = delete;
    QueueMutex& operator=(const QueueMutex&) = delete;

    void Lock() noexcept {
        uptr expected = 0;
        if (!word_.CompareExchangeWeak<MemoryOrder::Acquire, MemoryOrder::Relaxed>(expected,
                                                                                   LockedBit)) {
            LockSlow();
        }
    }

    bool TryLock() noexcept {
        uptr current = word_.Load<MemoryOrder::Relaxed>();
        while ((current & LockedBit) == 0) {
            if (word_.CompareExchangeWeak<MemoryOrder::Acquire, MemoryOrder::Relaxed>(
                    current, current | LockedBit)) {
                return true;
            }
        }

        return false;
    }

    void Unlock() noexcept {
        uptr expected = LockedBit;
        if (!word_.CompareExchangeStrong<MemoryOrder::Release, MemoryOrder::Relaxed>(expected, 0)) {
            UnlockSlow();
        }
    }

    static constexpr uptr LockedBit = 1;
    static constexpr uptr QueueLockedBit = 2;
    static constexpr uptr QueueHeadMask = ~(LockedBit | QueueLockedBit);

private:
    void LockSlow() noexcept;
    void UnlockSlow() noexcept;

    Atomic<uptr> word_;
};

// QueueMutex를 범위 동안 쥔다.
class QueueMutexGuard {
public:
    explicit QueueMutexGuard(QueueMutex& mutex) noexcept : mutex_(mutex) {
        mutex_.Lock();
    }

    ~QueueMutexGuard() noexcept {
        mutex_.Unlock();
    }

    QueueMutexGuard(const QueueMutexGuard&) = delete;
    QueueMutexGuard& operator=(const QueueMutexGuard&) = delete;

private:
    QueueMutex& mutex_;
};

} // namespace rocks::platform::detail
