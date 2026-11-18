#include "../Common/TerminationInternal.h"

#include <Platform/Atomics.h>
#include <Platform/CrashHandler.h>

#include <Windows.h>

namespace rocks::platform {

namespace {

// flush 전용 스레드의 stack 크기다(hook은 고정 버퍼만 쓴다).
constexpr usize FlushThreadStackBytes = 64 * 1024;
// 예외 코드 표기 "Unhandled exception 0x" + 16진 8자리 + NUL이다.
constexpr usize CrashMessageBytes = 64;
constexpr uint32 HexDigitsPerCode = 8;
constexpr uint32 BitsPerHexDigit = 4;
constexpr uint32 HexDigitMask = 0xF;

// 설치 상태다. 설치·제거는 시작·종료 단계에서만 하므로 lock 없이 원자 변수로 둔다.
constinit uint32 installed = 0;
constinit uint32 crashing = 0;
constinit uint32 stopping = 0;
constinit uint32 timeoutMilliseconds = 0;
constinit LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;
constinit HANDLE crashEvent = nullptr;
constinit HANDLE doneEvent = nullptr;
constinit HANDLE flushThread = nullptr;
constinit char crashMessage[CrashMessageBytes] = {};
constinit usize crashMessageBytes = 0;

// 예외 코드를 할당·CRT 없이 표기한다.
void FormatCrashMessage(DWORD code) noexcept {
    constexpr char Prefix[] = "Unhandled exception 0x";
    constexpr char Digits[] = "0123456789ABCDEF";
    usize length = 0;
    for (; Prefix[length] != '\0'; ++length) {
        crashMessage[length] = Prefix[length];
    }

    for (uint32 index = 0; index < HexDigitsPerCode; ++index) {
        const uint32 shift = (HexDigitsPerCode - 1 - index) * BitsPerHexDigit;
        crashMessage[length++] = Digits[(code >> shift) & HexDigitMask];
    }

    crashMessage[length] = '\0';
    crashMessageBytes = length;
}

// 크래시를 기다리다가 종료 hook을 부르는 스레드다. 깨진 스레드의 stack(넘쳤을 수 있다)을 쓰지
// 않으려고 따로 둔다.
DWORD WINAPI FlushThreadMain(void*) noexcept {
    PrepareThreadForCrashHandling();
    if (WaitForSingleObject(crashEvent, INFINITE) == WAIT_OBJECT_0 &&
        AtomicLoad<MemoryOrder::Acquire>(&stopping) == 0) {
        detail::RunTerminationHook(crashMessage, crashMessageBytes);
        SetEvent(doneEvent);
    }

    return 0;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) noexcept {
    // 처음 들어온 스레드만 flush를 맡긴다. 다른 스레드가 겹쳐 들어오면 바로 앞 처리기로 간다.
    if (AtomicExchange<MemoryOrder::AcquireRelease>(&crashing, 1u) == 0) {
        FormatCrashMessage(info != nullptr && info->ExceptionRecord != nullptr
                               ? info->ExceptionRecord->ExceptionCode
                               : 0);
        SetEvent(crashEvent);
        (void)WaitForSingleObject(doneEvent,
                                  AtomicLoad<MemoryOrder::Relaxed>(&timeoutMilliseconds));
    }

    return previousFilter != nullptr ? previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

void CloseHandles() noexcept {
    const HANDLE handles[] = {flushThread, crashEvent, doneEvent};
    for (HANDLE handle : handles) {
        if (handle != nullptr) {
            CloseHandle(handle);
        }
    }

    flushThread = nullptr;
    crashEvent = nullptr;
    doneEvent = nullptr;
}

} // namespace

bool InstallCrashHandler(const CrashHandlerOptions& options) noexcept {
    if (AtomicLoad<MemoryOrder::Acquire>(&installed) != 0) {
        return true;
    }

    AtomicStore<MemoryOrder::Relaxed>(&timeoutMilliseconds, options.flushTimeoutMilliseconds);
    AtomicStore<MemoryOrder::Relaxed>(&stopping, 0u);
    crashEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (crashEvent == nullptr || doneEvent == nullptr) {
        CloseHandles();
        return false;
    }

    flushThread = CreateThread(nullptr, FlushThreadStackBytes, &FlushThreadMain, nullptr,
                               STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (flushThread == nullptr) {
        CloseHandles();
        return false;
    }

    PrepareThreadForCrashHandling();
    previousFilter = SetUnhandledExceptionFilter(&CrashFilter);
    AtomicStore<MemoryOrder::Release>(&installed, 1u);
    return true;
}

void RemoveCrashHandler() noexcept {
    if (AtomicExchange<MemoryOrder::AcquireRelease>(&installed, 0u) == 0) {
        return;
    }

    (void)SetUnhandledExceptionFilter(previousFilter);
    previousFilter = nullptr;
    AtomicStore<MemoryOrder::Release>(&stopping, 1u);
    SetEvent(crashEvent);
    (void)WaitForSingleObject(flushThread, INFINITE);
    CloseHandles();
}

void PrepareThreadForCrashHandling() noexcept {
    // 이미 더 큰 값이면 줄이지 않는다(실패는 무시한다, 처리기가 덜 안전해질 뿐이다).
    ULONG guarantee = static_cast<ULONG>(CrashStackReserveBytes);
    (void)SetThreadStackGuarantee(&guarantee);
}

} // namespace rocks::platform
