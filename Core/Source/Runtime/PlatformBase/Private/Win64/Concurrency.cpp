#include <Platform/CrashReporting.h>
#include <Platform/Thread.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Platform/PlatformMath.h>
#include "../Common/ThreadInternal.h"
#include "WindowsString.h"
#include <Windows.h>
#include <process.h>
#include <new>

// ntdll: 프로세스가 종료 절차(LdrShutdownProcess)에 들어갔는지다. SDK 헤더에 선언이 없어 직접
// 선언한다.
extern "C" NTSYSAPI BOOLEAN NTAPI RtlDllShutdownInProgress(VOID);
#pragma comment(lib, "ntdll")

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

uint32 LogicalProcessorCount() noexcept {
    // 프로세서 그룹(64개 단위)을 모두 센 수와 이 프로세스의 affinity mask 비트 수 가운데 작은
    // 값이다. mask는 프로세스가 속한 그룹 하나만 보이므로, 여러 그룹 기계에서도 과다 구독하지 않는
    // 쪽(작은 값)을 고른다.
    DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) != 0 &&
        processMask != 0) {
        const DWORD allowed = static_cast<DWORD>(PlatformMath::PopCount64(processMask));
        count = allowed < count || count == 0 ? allowed : count;
    }

    return count == 0 ? 1u : static_cast<uint32>(count);
}

ThreadStackRange CurrentThreadStackRange() noexcept {
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    if (low >= high) {
        Fatal("GetCurrentThreadStackLimits returned an empty range");
    }

    ThreadStackRange range;
    range.low = static_cast<uptr>(low);
    range.high = static_cast<uptr>(high);
    return range;
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

namespace {

struct ExitSlotEntry {
    ThreadExitSlot::Callback callback = nullptr;
    DWORD index = FLS_OUT_OF_INDEXES;
};

ExitSlotEntry exitSlots[MaxThreadExitSlots];
uint32 exitSlotCount = 0;

// FLS callback은 값만 받으므로 칸마다 다른 함수를 건다(어느 칸인지 알아야 값을 지울 수 있다).
template <uint32 Slot>
void WINAPI ExitSlotTrampoline(void* value) noexcept {
    const ExitSlotEntry& entry = exitSlots[Slot];
    // 값을 먼저 지운다: 남아 있으면 Windows가 callback을 다시 부를 수 있다(UE 5.8
    // WindowsPlatformTLS.cpp OnThreadExit도 같다).
    FlsSetValue(entry.index, nullptr);
    // 프로세스 종료 중에는 부르지 않는다. 다른 스레드는 이미 강제 종료돼 잠금을 쥔 채일 수 있다.
    if (RtlDllShutdownInProgress() != FALSE) {
        return;
    }

    entry.callback(value);
}

constexpr PFLS_CALLBACK_FUNCTION ExitSlotTrampolines[MaxThreadExitSlots] = {
    &ExitSlotTrampoline<0>, &ExitSlotTrampoline<1>, &ExitSlotTrampoline<2>, &ExitSlotTrampoline<3>,
    &ExitSlotTrampoline<4>, &ExitSlotTrampoline<5>, &ExitSlotTrampoline<6>, &ExitSlotTrampoline<7>};

} // namespace

ThreadExitSlot::ThreadExitSlot(Callback callback) noexcept {
    if (callback == nullptr) {
        return;
    }

    const uint32 slot = AtomicFetchAdd<MemoryOrder::AcquireRelease>(&exitSlotCount, 1);
    if (slot >= MaxThreadExitSlots) {
        return;
    }

    exitSlots[slot].callback = callback;
    const DWORD index = FlsAlloc(ExitSlotTrampolines[slot]);
    if (index == FLS_OUT_OF_INDEXES) {
        return;
    }

    // 값은 이 생성자가 끝난 뒤에만 둘 수 있으므로, trampoline이 불리기 전에 번호가 적혀 있다.
    exitSlots[slot].index = index;
    native_ = index;
    valid_ = true;
}

void* ThreadExitSlot::Get() const noexcept {
    return valid_ ? FlsGetValue(static_cast<DWORD>(native_)) : nullptr;
}

bool ThreadExitSlot::Set(void* value) noexcept {
    return valid_ && FlsSetValue(static_cast<DWORD>(native_), value) != 0;
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
