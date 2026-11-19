#include <Platform/CrashHandler.h>
#include <Platform/Thread.h>
#include <Platform/Fatal.h>
#include "../Common/ThreadInternal.h"
#include "PosixPlatform.h"
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <new>

namespace rocks::platform {

ConditionVariable::ConditionVariable() noexcept {
    static_assert(sizeof(pthread_cond_t) <= sizeof(storage_));
    static_assert(alignof(pthread_cond_t) <= alignof(ConditionVariable));
    detail::PosixInitializeCondition(reinterpret_cast<pthread_cond_t*>(storage_));
}

ConditionVariable::~ConditionVariable() noexcept {
    if (pthread_cond_destroy(reinterpret_cast<pthread_cond_t*>(storage_)) != 0) {
        Fatal("Destroying a busy condition variable");
    }
}

WaitResult ConditionVariable::Wait(Mutex& mutex, uint32 timeoutMs) noexcept {
    auto* condition = reinterpret_cast<pthread_cond_t*>(storage_);
    auto* nativeMutex = reinterpret_cast<pthread_mutex_t*>(mutex.storage_);
    int result;
    if (timeoutMs == InfiniteTimeout) {
        result = pthread_cond_wait(condition, nativeMutex);
    } else {
        // 시스템 시각(CLOCK_REALTIME) 변경의 영향을 받지 않는 OS별 시한 대기를 쓴다.
        result = detail::PosixTimedWait(condition, nativeMutex, timeoutMs);
    }
    if (result == 0) {
        return WaitResult::Signaled;
    }
    return result == ETIMEDOUT ? WaitResult::TimedOut : WaitResult::Failed;
}

void ConditionVariable::NotifyOne() noexcept {
    if (pthread_cond_signal(reinterpret_cast<pthread_cond_t*>(storage_)) != 0) {
        Fatal("pthread_cond_signal failed");
    }
}

void ConditionVariable::NotifyAll() noexcept {
    if (pthread_cond_broadcast(reinterpret_cast<pthread_cond_t*>(storage_)) != 0) {
        Fatal("pthread_cond_broadcast failed");
    }
}

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
