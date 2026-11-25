#include <Platform/Sync/WaiterSignal.h>
#include <Platform/Fatal.h>

#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace rocks::platform {

namespace {

constexpr uint32 MillisecondsPerSecond = 1000;
constexpr long NanosecondsPerMillisecond = 1000000;

// 상태 word의 주소다. Atomic<uint32>는 표준 배치라 객체 주소가 값 주소다.
uint32* StateAddress(Atomic<uint32>& state) noexcept {
    static_assert(sizeof(Atomic<uint32>) == sizeof(uint32));
    return reinterpret_cast<uint32*>(&state);
}

void WakeOne(Atomic<uint32>& state) noexcept {
    if (syscall(SYS_futex, StateAddress(state), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0) < 0) {
        Fatal("futex wake failed");
    }
}

} // namespace

WaiterSignal::WaiterSignal() noexcept = default;

WaiterSignal::~WaiterSignal() noexcept = default;

void WaiterSignal::Notify() noexcept {
    state_.Store<MemoryOrder::Release>(1);
    WakeOne(state_);
}

// futex 깨우기는 주소를 키로만 쓰므로 Notify가 끝나기를 기다릴 것이 없다.
void WaiterSignal::Settle() noexcept {
}

// FUTEX_WAIT는 값이 0일 때만 잔다(아니면 EAGAIN). 상대 시한은 CLOCK_MONOTONIC 기준이다(futex(2)).
// 호출자가 남은 시간을 매번 다시 계산하므로 절대 시한(FUTEX_WAIT_BITSET)이 필요 없다. EINTR·EAGAIN·
// ETIMEDOUT은 호출자가 다시 본다.
void WaiterSignal::WaitOnce(uint32 timeoutMs) noexcept {
    timespec relative{};
    timespec* timeout = nullptr;
    if (timeoutMs != InfiniteTimeout) {
        relative.tv_sec = static_cast<time_t>(timeoutMs / MillisecondsPerSecond);
        relative.tv_nsec =
            static_cast<long>(timeoutMs % MillisecondsPerSecond) * NanosecondsPerMillisecond;
        timeout = &relative;
    }

    if (syscall(SYS_futex, StateAddress(state_), FUTEX_WAIT_PRIVATE, 0, timeout, nullptr, 0) < 0 &&
        errno != EAGAIN && errno != EINTR && errno != ETIMEDOUT) {
        Fatal("futex wait failed");
    }
}

} // namespace rocks::platform
