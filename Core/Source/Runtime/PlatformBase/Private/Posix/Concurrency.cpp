#include <Base/PlacementNew.h>
#include <Platform/CrashReporting.h>
#include <Platform/Thread.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include "../Common/ThreadInternal.h"
#include "PosixPlatform.h"
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

namespace rocks::platform {

void SleepFor(uint32 milliseconds) noexcept {
    if (milliseconds == 0) {
        return;
    }
    timespec remaining{static_cast<time_t>(milliseconds / 1000),
                       static_cast<long>(milliseconds % 1000) * 1000000};
    while (nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) {
            Fatal("nanosleep failed");
        }
    }
}

void YieldThread() noexcept {
    sched_yield();
}

uint32 LogicalProcessorCount() noexcept {
    return detail::PosixLogicalProcessorCount();
}

ThreadStackRange CurrentThreadStackRange() noexcept {
    ThreadStackRange range;
    detail::PosixCurrentThreadStack(range.low, range.high);
    if (range.low >= range.high) {
        Fatal("thread stack range query returned an empty range");
    }

    return range;
}

usize CopyCurrentThreadName(char* buffer, usize capacity) noexcept {
    if (buffer == nullptr || capacity == 0) {
        return 0;
    }

    buffer[0] = '\0';
    if (pthread_getname_np(pthread_self(), buffer, capacity) != 0) {
        buffer[0] = '\0';
        return 0;
    }

    usize length = 0;
    while (length < capacity && buffer[length] != '\0') {
        ++length;
    }

    if (length == capacity) {
        buffer[0] = '\0';
        return 0;
    }

    return length;
}

namespace {

uint32 exitSlotCount = 0;

} // namespace

ThreadExitSlot::ThreadExitSlot(Callback callback) noexcept {
    static_assert(sizeof(pthread_key_t) <= sizeof(native_), "pthread_key_t does not fit the slot");
    if (callback == nullptr) {
        return;
    }

    // Windows 구현과 같은 상한을 지킨다(플랫폼마다 다르게 동작하지 않게).
    if (AtomicFetchAdd<MemoryOrder::AcquireRelease>(&exitSlotCount, 1) >= MaxThreadExitSlots) {
        return;
    }

    // 소멸자는 값을 null로 지운 뒤에 불리고, main 반환·exit에서는 불리지 않는다.
    pthread_key_t key{};
    if (pthread_key_create(&key, callback) != 0) {
        return;
    }

    native_ = static_cast<uint64>(key);
    valid_ = true;
}

void* ThreadExitSlot::Get() const noexcept {
    if (!valid_) {
        return nullptr;
    }

    // 계약: OS 오류 값(errno)을 바꾸지 않는다. pthread_getspecific은 errno를 건드리지 않게 돼
    // 있지만 Windows 구현과 같은 모양으로 보존해 libc에 기대지 않는다.
    const int savedError = errno;
    void* value = pthread_getspecific(static_cast<pthread_key_t>(native_));
    errno = savedError;
    return value;
}

bool ThreadExitSlot::Set(void* value) noexcept {
    if (!valid_) {
        return false;
    }

    // 실패한 pthread_setspecific(저장소 할당 실패)이 errno를 바꾸는 libc가 있다. 호출자는 반환값만
    // 보므로 보존한다(Get과 같은 계약).
    const int savedError = errno;
    const bool stored = pthread_setspecific(static_cast<pthread_key_t>(native_), value) == 0;
    errno = savedError;
    return stored;
}

namespace detail {
namespace {

// sysconf가 page 크기를 알려 주지 못할 때 쓰는 값이다(POSIX 플랫폼의 최소 page 크기).
constexpr usize FallbackPageBytes = 4096;

void* ThreadEntry(void* context) noexcept {
    RunThread(*static_cast<ThreadState*>(context));
    ReleaseThreadCrashHandling();
    return nullptr;
}

} // namespace

void ApplyCurrentThreadOptions(const ThreadState& state) noexcept {
    // 치명 신호 처리기가 돌 대체 stack(sigaltstack)을 둔다.
    PrepareThreadForCrashHandling();
    if (state.name[0] != '\0') {
        PosixSetCurrentThreadName(state.name);
    }

    if (state.priority != ThreadPriority::Normal) {
        PosixSetCurrentThreadPriority(state.priority);
    }
}

} // namespace detail

bool TryGetCurrentThreadPriority(ThreadPriority& priority) noexcept {
    return detail::PosixGetCurrentThreadPriority(priority);
}

namespace detail {

ThreadStatus StartNativeThread(ThreadState& state) noexcept {
    static_assert(sizeof(pthread_t) <= sizeof(state.native));
    static_assert(alignof(pthread_t) <= alignof(ThreadState));
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) {
        return ThreadStatus::SystemError;
    }
    ThreadStatus status = ThreadStatus::Success;
    if (state.stackBytes != 0) {
        // pthread는 PTHREAD_STACK_MIN 미만·(일부 구현에서) page 배수가 아닌 크기를 거부하므로 둘 다
        // 맞춰 올린다. 상한(MaxThreadStackBytes)은 Start가 검사했으므로 올림이 넘치지 않는다.
        const long pageBytes = sysconf(_SC_PAGESIZE);
        const usize page = pageBytes > 0 ? static_cast<usize>(pageBytes) : FallbackPageBytes;
        const auto minimum = static_cast<usize>(PTHREAD_STACK_MIN);
        usize bytes = state.stackBytes < minimum ? minimum : state.stackBytes;
        bytes = (bytes + page - 1) / page * page;
        if (pthread_attr_setstacksize(&attributes, bytes) != 0) {
            status = ThreadStatus::InvalidArgument;
        }
    }

    if (status == ThreadStatus::Success) {
        auto* handle = ::new (state.native) pthread_t{};
        if (pthread_create(handle, &attributes, ThreadEntry, &state) != 0) {
            status = ThreadStatus::SystemError;
        }
    }

    if (pthread_attr_destroy(&attributes) != 0) {
        Fatal("pthread_attr_destroy failed");
    }

    return status;
}

ThreadStatus JoinNativeThread(ThreadState& state) noexcept {
    auto& handle = *reinterpret_cast<pthread_t*>(state.native);
    if (pthread_join(handle, nullptr) != 0) {
        return ThreadStatus::SystemError;
    }
    handle = {};
    return ThreadStatus::Success;
}

} // namespace detail
} // namespace rocks::platform
