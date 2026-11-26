#pragma once

#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Platform/Sync/WaitTypes.h>

#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

// Android futex 대기·깨우기다(WaiterSignal·RealtimeWakeSignal의 OS 수단,
// Docs/Design/Synchronization 7.3·9.6절). 상태 word는 Atomic<uint32>이고 표준 배치라 객체 주소가 값
// 주소다.
namespace rocks::platform::detail {

inline constexpr uint32 FutexMillisecondsPerSecond = 1000;
inline constexpr long FutexNanosecondsPerMillisecond = 1000000;

inline uint32* FutexWord(Atomic<uint32>& state) noexcept {
    static_assert(sizeof(Atomic<uint32>) == sizeof(uint32));
    return reinterpret_cast<uint32*>(&state);
}

// 값이 expected인 동안 잔다(아니면 바로 돌아온다). 상대 시한은 CLOCK_MONOTONIC 기준이고 호출자가
// 남은 시간을 매번 다시 계산하므로 절대 시한(FUTEX_WAIT_BITSET)이 필요 없다.
// EINTR·EAGAIN·ETIMEDOUT은 호출자가 상태를 다시 본다.
inline void FutexWait(Atomic<uint32>& state, uint32 expected, uint32 timeoutMs) noexcept {
    timespec relative{};
    timespec* timeout = nullptr;
    if (timeoutMs != InfiniteTimeout) {
        relative.tv_sec = static_cast<time_t>(timeoutMs / FutexMillisecondsPerSecond);
        relative.tv_nsec = static_cast<long>(timeoutMs % FutexMillisecondsPerSecond) *
                           FutexNanosecondsPerMillisecond;
        timeout = &relative;
    }

    if (syscall(SYS_futex, FutexWord(state), FUTEX_WAIT_PRIVATE, expected, timeout, nullptr, 0) <
            0 &&
        errno != EAGAIN && errno != EINTR && errno != ETIMEDOUT) {
        Fatal("futex wait failed");
    }
}

inline void FutexWakeOne(Atomic<uint32>& state) noexcept {
    if (syscall(SYS_futex, FutexWord(state), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0) < 0) {
        Fatal("futex wake failed");
    }
}

} // namespace rocks::platform::detail
