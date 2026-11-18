#include <Platform/CrashHandler.h>
#include <Platform/Thread.h>
#include <Platform/Fatal.h>
#include "../Common/ThreadInternal.h"
#include "WindowsString.h"
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

usize CopyCurrentThreadName(char* buffer, usize capacity) noexcept {
    if (buffer == nullptr || capacity == 0) {
        return 0;
    }

    buffer[0] = '\0';
    PWSTR description = nullptr;
    if (FAILED(GetThreadDescription(GetCurrentThread(), &description))) {
        return 0;
    }

    const windows::TextConversion converted =
        windows::ConvertWideToUtf8(description, static_cast<usize>(lstrlenW(description)), buffer,
                                   capacity, windows::TextFit::Exact);
    LocalFree(description);
    // 실패하면 buffer는 이미 빈 문자열이다.
    return converted ? converted.length : 0;
}

namespace detail {
namespace {

unsigned __stdcall ThreadEntry(void* context) noexcept {
    RunThread(*static_cast<ThreadState*>(context));
    // _beginthreadex의 start routine 반환 경로로 CRT/TLS 종료 정리를 수행한다.
    return 0;
}

} // namespace

void ApplyCurrentThreadOptions(const ThreadState& state) noexcept {
    // 크래시 처리기(filter)가 돌 여유 stack을 확보한다(stack overflow 대응).
    PrepareThreadForCrashHandling();
    if (state.name[0] != '\0') {
        // Start가 UTF-8과 길이를 검사했으므로 변환은 실패하지 않는다. 실패해도 이름은 힌트라
        // 무시한다. UTF-8 n byte는 UTF-16 n 단위를 넘지 않으므로 칸이 모자라지 않는다.
        wchar_t wide[MaxThreadNameBytes + 1]{};
        usize bytes = 0;
        while (bytes < MaxThreadNameBytes && state.name[bytes] != '\0') {
            ++bytes;
        }

        if (windows::ConvertUtf8ToWide(state.name, bytes, wide, MaxThreadNameBytes + 1,
                                       windows::TextFit::Exact)) {
            static_cast<void>(SetThreadDescription(GetCurrentThread(), wide));
        }
    }

    int priority = THREAD_PRIORITY_NORMAL;
    switch (state.priority) {
    case ThreadPriority::Lowest:
        priority = THREAD_PRIORITY_LOWEST;
        break;
    case ThreadPriority::BelowNormal:
        priority = THREAD_PRIORITY_BELOW_NORMAL;
        break;
    case ThreadPriority::Normal:
        return;
    case ThreadPriority::AboveNormal:
        priority = THREAD_PRIORITY_ABOVE_NORMAL;
        break;
    case ThreadPriority::Highest:
        priority = THREAD_PRIORITY_HIGHEST;
        break;
    }

    static_cast<void>(SetThreadPriority(GetCurrentThread(), priority));
}

ThreadStatus StartNativeThread(ThreadState& state) noexcept {
    static_assert(sizeof(HANDLE) <= sizeof(state.native));
    static_assert(alignof(HANDLE) <= alignof(ThreadState));
    // stackBytes는 MaxThreadStackBytes(1 GiB) 이하라 unsigned에 들어간다. 예약 크기로 넘기며 OS가
    // 할당 단위로 올린다. 0이면 실행 파일 기본값이다.
    const auto stackBytes = static_cast<unsigned>(state.stackBytes);
    const unsigned flags = stackBytes != 0 ? STACK_SIZE_PARAM_IS_A_RESERVATION : 0u;
    const uintptr_t handle =
        _beginthreadex(nullptr, stackBytes, ThreadEntry, &state, flags, nullptr);
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
