#include <Platform/Thread.h>
#include <Platform/Fatal.h>
#include "../Common/ThreadInternal.h"
#include <Windows.h>
#include <process.h>
#include <new>

namespace rocks::platform {

ConditionVariable::ConditionVariable() noexcept {
    static_assert(sizeof(CONDITION_VARIABLE) <= sizeof(storage_));
    static_assert(alignof(CONDITION_VARIABLE) <= alignof(ConditionVariable));
    InitializeConditionVariable(reinterpret_cast<CONDITION_VARIABLE*>(storage_));
}

ConditionVariable::~ConditionVariable() noexcept = default;

WaitResult ConditionVariable::Wait(Mutex& mutex, uint32 timeoutMs) noexcept {
    if (SleepConditionVariableSRW(reinterpret_cast<CONDITION_VARIABLE*>(storage_),
                                  reinterpret_cast<SRWLOCK*>(mutex.storage_), timeoutMs, 0)) {
        return WaitResult::Signaled;
    }
    return GetLastError() == ERROR_TIMEOUT ? WaitResult::TimedOut : WaitResult::Failed;
}

void ConditionVariable::NotifyOne() noexcept {
    WakeConditionVariable(reinterpret_cast<CONDITION_VARIABLE*>(storage_));
}

void ConditionVariable::NotifyAll() noexcept {
    WakeAllConditionVariable(reinterpret_cast<CONDITION_VARIABLE*>(storage_));
}

void SleepFor(uint32 milliseconds) noexcept {
    // wait API의 무한 sentinel과 달리 SleepFor의 최댓값도 유한 시간으로 취급한다.
    if (milliseconds == InfiniteTimeout) {
        Sleep(InfiniteTimeout - 1);
        Sleep(1);
    } else if (milliseconds != 0) {
        Sleep(milliseconds);
    }
}

void YieldThread() noexcept {
    SwitchToThread();
}

namespace detail {
namespace {

unsigned __stdcall ThreadEntry(void* context) noexcept {
    RunThread(*static_cast<ThreadState*>(context));
    // _beginthreadex의 start routine 반환 경로로 CRT/TLS 종료 정리를 수행한다.
    return 0;
}

} // namespace

ThreadStatus StartNativeThread(ThreadState& state) noexcept {
    static_assert(sizeof(HANDLE) <= sizeof(state.native));
    static_assert(alignof(HANDLE) <= alignof(ThreadState));
    const uintptr_t handle = _beginthreadex(nullptr, 0, ThreadEntry, &state, 0, nullptr);
    if (handle == 0) {
        return ThreadStatus::SystemError;
    }
    ::new (state.native) HANDLE(reinterpret_cast<HANDLE>(handle));
    return ThreadStatus::Success;
}

ThreadStatus JoinNativeThread(ThreadState& state) noexcept {
    auto& handle = *reinterpret_cast<HANDLE*>(state.native);
    if (WaitForSingleObject(handle, INFINITE) != WAIT_OBJECT_0 || !CloseHandle(handle)) {
        return ThreadStatus::SystemError;
    }
    handle = nullptr;
    return ThreadStatus::Success;
}

} // namespace detail
} // namespace rocks::platform
