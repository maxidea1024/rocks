#include <Platform/Thread.h>
#include <Platform/Fatal.h>
#include "../Common/ThreadInternal.h"
#include "PosixPlatform.h"
#include <pthread.h>
#include <errno.h>
#include <sched.h>
#include <time.h>
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

namespace detail {
namespace {

void* ThreadEntry(void* context) noexcept {
    RunThread(*static_cast<ThreadState*>(context));
    return nullptr;
}

} // namespace

ThreadStatus StartNativeThread(ThreadState& state) noexcept {
    static_assert(sizeof(pthread_t) <= sizeof(state.native));
    static_assert(alignof(pthread_t) <= alignof(ThreadState));
    auto* handle = ::new (state.native) pthread_t{};
    if (pthread_create(handle, nullptr, ThreadEntry, &state) != 0) {
        return ThreadStatus::SystemError;
    }
    return ThreadStatus::Success;
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
