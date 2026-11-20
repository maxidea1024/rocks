#include "CrashHelper.h"

#include <Base/Compiler.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/CrashReporting.h>
#include <Platform/Fatal.h>
#include <Platform/Win64/WideString.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>
#include <crtdbg.h>
#include <signal.h>
#include <stdlib.h>
#include <werapi.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

#if ROCKS_ADDRESS_SANITIZER
// ASan runtime의 보고 뒤 콜백 등록이다(sanitizer/common_interface_defs.h와 같은 선언).
extern "C" void __sanitizer_set_death_callback(void (*callback)(void));
#endif

// Windows 크래시 수집기다(Docs/Design/CrashHandlingAndLaunch.ko.md 8절). 보호 프레임
// (RunWithCrashGuard, platform::Thread, Fatal)과 unhandled exception filter·힙 손상 VEH가 같은
// 처리 함수(HandleCrash)로 들어온다. CRT 치명 경로(abort의 SIGABRT, invalid parameter, pure virtual
// call)는 Fatal과 같은 경로(FailFatally)로 들어오고 반환하지 않는다. 보호 함수의 __try 블록에는
// 인라인되지 않는 호출 하나만 둔다(clang의 동기 SEH는 블록 안 명령이 직접 낸 fault를 그 블록에서
// 잡지 못한다).
namespace rocks::platform {

namespace {

// 크래시 처리 스레드의 stack 예약 크기다(프로세스 안 덤프의 DbgHelp 여유).
constexpr usize ReporterStackBytes = 256 * 1024;
constexpr usize CodeMessageBytes = 64;
constexpr usize RuntimeCheckMessageBytes = 768;
constexpr usize RuntimeCheckPartBytes = 224;
// STATUS_HEAP_CORRUPTION(ntstatus.h)이다. Windows.h가 정의하지 않는다.
constexpr DWORD HeapCorruptionCode = 0xC0000374;
// 접근 위반 예외 인자의 종류 값이다(0 읽기, 1 쓰기, 8 실행).
constexpr ULONG_PTR AccessReadFlag = 0;
constexpr ULONG_PTR AccessWriteFlag = 1;
constexpr ULONG_PTR AccessExecuteFlag = 8;
constexpr unsigned int AbortBehaviorMask = _WRITE_ABORT_MSG | _CALL_REPORTFAULT;

constinit HANDLE crashRequested = nullptr;
constinit HANDLE reportFinished = nullptr;
constinit HANDLE reporterThread = nullptr;
constinit uint32 stopping = 0;
constinit LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;
// 프로세스 안 덤프를 쓰는가다(설치 때 정한다: 내장·sanitizer가 아니고 DbgHelp가 준비됨).
constinit bool inProcessDump = false;
// 덤프를 쓰려 하는가다(설치 때 정한다: 내장·sanitizer가 아님). DbgHelp를 열지 못해도 참이다(그때
// 덤프는 Failed로 남는다).
constinit bool dumpsRequested = false;
// 크래시 처리 스레드의 도우미 요청 결과다(그 스레드만 쓴다).
constinit detail::CrashHelperOutcome helperOutcome = {};
constinit bool helperCaptured = false;

// CRT hook과 힙 손상 VEH의 이전 값이다(설치·제거 때만 쓴다).
constinit bool crtHooksInstalled = false;
constinit _crt_signal_t previousAbortSignal = nullptr;
constinit _invalid_parameter_handler previousInvalidParameter = nullptr;
constinit _purecall_handler previousPurecall = nullptr;
constinit unsigned int previousAbortBehavior = 0;
constinit void* heapCorruptionHandler = nullptr;
// SuppressSystemErrorDialogs가 불렸다(제거 때 CRT abort 설정을 대화상자 있는 값으로 되돌리지
// 않는다).
constinit uint32 dialogsSuppressed = 0;

// 실패 문맥 사본이다(소유자만 쓴다). 덤프가 정적 EXCEPTION_POINTERS를 읽는다.
alignas(16) constinit CONTEXT contextCopy = {};
constinit EXCEPTION_RECORD recordCopy = {};
constinit EXCEPTION_POINTERS pointersCopy = {};

[[noreturn]] void TerminateWithCode(uint32 code) noexcept {
    (void)TerminateProcess(GetCurrentProcess(), code);
    detail::ParkCurrentThread();
}

// 크래시 처리 스레드가 쓸 수 있는 최대 시간이다. 도우미가 있으면 캡처 마감과, 늦은 캡처의 폴더
// 확정을 기다리는 메타데이터 몫을 더한다(캡처가 실패하면 남은 예산 안에서 프로세스 안 fallback까지
// 한다). 도우미가 캡처하면 실제 대기는 캡처와 콜백뿐이다.
uint32 ReporterBudgetMilliseconds() noexcept {
    const CrashReportingOptions& options = detail::CrashOptions();
    return (detail::CrashHelperStarted()
                ? options.captureTimeoutMilliseconds + options.metadataTimeoutMilliseconds
                : 0u) +
           detail::CrashHelperJobWaitMilliseconds() + options.callbackBudgetMilliseconds +
           (inProcessDump ? options.inProcessDumpTimeoutMilliseconds : 0u) +
           options.metadataTimeoutMilliseconds;
}

uint32 TotalWaitMilliseconds() noexcept {
    return ReporterBudgetMilliseconds() + detail::ReportMarginMilliseconds;
}

const char* SkippedDumpReason() noexcept {
    if (detail::CrashOptions().embedded) {
        return "embedded host owns crash dumps";
    }

    return ROCKS_ADDRESS_SANITIZER ? "sanitizer build" : nullptr;
}

// 크래시 처리 스레드의 파일 단계다. 도우미가 캡처했으면 콜백 결과만 넘기고(파일은 도우미가 쓴다),
// 아니면 프로세스 안에서 폴더·CrashInfo.json·덤프를 쓴다.
void WriteReportFiles() noexcept {
    if (helperCaptured) {
        detail::FinishCrashHelperReport();
        return;
    }

    const CrashReportingOptions& options = detail::CrashOptions();
    detail::CrashInProcessReport request;
    request.writeDump = dumpsRequested;
    request.skippedReason = SkippedDumpReason();
    request.dumpTimeoutMilliseconds = options.inProcessDumpTimeoutMilliseconds;
    request.metadataMilliseconds = options.metadataTimeoutMilliseconds;
    request.ownerDeadlineNanoseconds = detail::OwnerDeadline();
    detail::WriteCrashFiles(&pointersCopy, request, helperOutcome);
}

// 크래시 처리 스레드 자신이 실패했다. 종료 콜백 단계였으면 남은 콜백만 포기하고 파일 단계를 이어
// 간다. 파일 단계에서 실패했으면 그대로 포기한다. 어느 쪽이든 기다리는 소유자에게 알린 뒤 멈춘다.
[[noreturn]] void FailReporter() noexcept {
    const bool continueFiles = detail::TakeOverFilesAfterReporterFailure();
    detail::WriteDiagnosticLine(continueFiles ? "Rocks crash reporter thread failed; skipping the "
                                                "remaining termination callbacks"
                                              : "Rocks crash reporter thread failed while writing "
                                                "crash files");
    if (continueFiles) {
        WriteReportFiles();
    }

    if (reportFinished != nullptr) {
        SetEvent(reportFinished);
    }

    detail::ParkCurrentThread();
}

bool IsReporterThread(uint64 self) noexcept {
    return detail::CrashStateInstalled() && self == detail::ReporterThreadId();
}

// 겹쳐 실패한 스레드다. 스스로 끝내지 않고 소유자의 종료를 기다린다. 소유자가 마감과 여유를 넘겨도
// 끝내지 않았을 때만(소유자가 멈춘 경우) 소유자의 종료 코드로 끝낸다.
LONG WaitForOwner(uint64 self) noexcept {
    detail::RecordStoppedThread(self);
    const uint64 graceNanoseconds =
        uint64{detail::CrashOptions().concurrentCrashGraceMilliseconds} *
        detail::NanosecondsPerMillisecond;
    uint64 deadline = detail::OwnerDeadline();
    if (deadline == 0) {
        deadline = MonotonicNanoseconds() +
                   uint64{TotalWaitMilliseconds()} * detail::NanosecondsPerMillisecond;
    }

    const uint64 limit = deadline + graceNanoseconds;
    if (detail::CrashOptions().embedded) {
        // 내장 모드: 보고가 끝나면 각자 호스트로 넘긴다.
        const uint64 now = MonotonicNanoseconds();
        if (now < deadline) {
            (void)WaitForSingleObject(
                reportFinished,
                static_cast<DWORD>((deadline - now) / detail::NanosecondsPerMillisecond));
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

    for (uint64 now = MonotonicNanoseconds(); now < limit; now = MonotonicNanoseconds()) {
        Sleep(static_cast<DWORD>((limit - now) / detail::NanosecondsPerMillisecond) + 1);
    }

    TerminateWithCode(detail::OwnerExitCode());
}

detail::FaultAccess AccessOf(const EXCEPTION_RECORD& record, uint64& address) noexcept {
    address = 0;
    const DWORD code = record.ExceptionCode;
    if ((code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR) ||
        record.NumberParameters < 2) {
        return detail::FaultAccess::None;
    }

    address = record.ExceptionInformation[1];
    switch (record.ExceptionInformation[0]) {
    case AccessReadFlag:
        return detail::FaultAccess::Read;
    case AccessWriteFlag:
        return detail::FaultAccess::Write;
    case AccessExecuteFlag:
        return detail::FaultAccess::Execute;
    default:
        return detail::FaultAccess::None;
    }
}

// 보호 프레임·unhandled filter·힙 손상 VEH의 공통 처리다. 보고하고 끝내거나(기본), 넘길 때
// CONTINUE_SEARCH다. unhandledFilter는 OS 최종 filter에서 불렀는가다(Asan 정책이 다르다).
LONG HandleCrash(EXCEPTION_POINTERS* info, bool unhandledFilter = false) noexcept {
    if (!detail::CrashStateInstalled() || info == nullptr || info->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const uint64 self = GetCurrentThreadId();
    if (IsReporterThread(self)) {
        FailReporter();
    }

    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const bool fatal = code == RocksFatalExceptionCode;
    // ASan은 자체 보고(SEH 가로채기)를 가진다. 보호 프레임·VEH에서는 하드웨어 예외를 넘겨 ASan이
    // 먼저 보게 한다(ASan 보고 뒤 death callback이 공통 보고를 한다). 최종 filter까지 온 예외는
    // ASan이 치명으로 보지 않아 넘긴 것(사용자 예외 코드·잘못된 핸들 등)이므로 여기서
    // 보고한다(설계 13.2절).
    if (ROCKS_ADDRESS_SANITIZER && !fatal && !unhandledFilter) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const CrashReportingOptions& options = detail::CrashOptions();
    if (!options.reportWhenDebuggerAttached && IsDebuggerPresent()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    switch (detail::ClaimCrash(self)) {
    case detail::CrashClaim::Owner:
        break;
    case detail::CrashClaim::Recursive:
        // 내장 모드에서 보고를 마친 예외가 바깥 Rocks 보호 프레임(Fatal 안 → RunWithCrashGuard)을
        // 지나며 다시 오는 경우다. 재귀가 아니므로 조용히 호스트 쪽으로 넘긴다.
        if (options.embedded && detail::ReportCompleted()) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        detail::WriteDiagnosticLine("Rocks crash handling failed again on the crashing thread");
        if (options.embedded) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        TerminateWithCode(RocksRecursiveCrashExitCode);
    case detail::CrashClaim::Shutdown:
        detail::WriteDiagnosticLine("Rocks crash after crash reporting shutdown began");
        if (options.embedded) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        TerminateWithCode(code);
    case detail::CrashClaim::Concurrent:
        return WaitForOwner(self);
    }

    const uint32 waitMilliseconds = TotalWaitMilliseconds();
    const uint64 deadline =
        MonotonicNanoseconds() + uint64{waitMilliseconds} * detail::NanosecondsPerMillisecond;
    detail::PublishOwnerTermination(code, deadline);
    recordCopy = *info->ExceptionRecord;
    recordCopy.ExceptionRecord = nullptr;
    if (info->ContextRecord != nullptr) {
        contextCopy = *info->ContextRecord;
#if ROCKS_ARCH_X64
        // 확장 영역 없이 복사했으므로 XSTATE 표시를 지운다(남기면 읽는 쪽이 없는 영역을 읽는다).
        contextCopy.ContextFlags &= ~(CONTEXT_XSTATE & ~CONTEXT_AMD64);
#endif
    }

    pointersCopy.ExceptionRecord = &recordCopy;
    pointersCopy.ContextRecord = &contextCopy;
    uint64 accessAddress = 0;
    const detail::FaultAccess access = AccessOf(recordCopy, accessAddress);
    detail::RecordCrashException(detail::CrashCodeKind::Exception, code,
                                 reinterpret_cast<uptr>(recordCopy.ExceptionAddress), access,
                                 accessAddress);
    detail::RecordCrashThreadName(detail::CurrentPlatformThreadName());
    if (fatal && recordCopy.NumberParameters >= 1 && recordCopy.ExceptionInformation[0] != 0) {
        const auto* origin =
            reinterpret_cast<const detail::FatalOrigin*>(recordCopy.ExceptionInformation[0]);
        detail::RecordCrashOrigin(origin->reason, self, origin->message, origin->messageBytes,
                                  origin->file, origin->line, origin->returnAddress);
    } else {
        char message[CodeMessageBytes];
        const usize bytes = detail::FormatCodeMessage(message, sizeof(message),
                                                      "Unhandled exception 0x", code, true);
        detail::RecordCrashOrigin(TerminationReason::Exception, self, message, bytes, nullptr, 0,
                                  recordCopy.ExceptionAddress);
    }

    SetEvent(crashRequested);
    (void)WaitForSingleObject(reportFinished, waitMilliseconds);
    if (options.embedded) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    TerminateWithCode(code);
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* info) noexcept {
    const LONG result = HandleCrash(info, true);
    if (result == EXCEPTION_CONTINUE_SEARCH && previousFilter != nullptr) {
        return previousFilter(info);
    }

    return result;
}

// 힙 손상은 일반 filter에 오지 않고 VEH(첫 기회)와 WER에서만 보인다. 그 코드만 받고 나머지는
// 그대로 넘긴다.
LONG CALLBACK HeapCorruptionHandler(EXCEPTION_POINTERS* info) noexcept {
    if (info == nullptr || info->ExceptionRecord == nullptr ||
        info->ExceptionRecord->ExceptionCode != HeapCorruptionCode) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    return HandleCrash(info);
}

// 보호 블록(ReporterMain) 안에 인라인되지 않게 한다(D34).
ROCKS_NOINLINE void ReporterLoop() noexcept {
    PrepareThreadForCrashHandling();
    (void)WaitForSingleObject(crashRequested, INFINITE);
    if (AtomicLoad<MemoryOrder::Acquire>(&stopping) != 0) {
        return;
    }

    // 도우미가 있으면 캡처를 먼저 한다(증거를 크래시 시점에 가깝게). 캡처 뒤 덤프 작성은 도우미가
    // 콜백과 병행한다. 도우미가 없으면 종료 콜백(로그 flush)을 덤프보다 먼저 한다. 프로세스 안
    // 덤프가 다른 스레드가 쥔 heap·loader lock에 막히면 flush까지 잃기 때문이다(설계 19.5절).
    helperCaptured = detail::RequestCrashHelperCapture(&pointersCopy, helperOutcome) ==
                     detail::CrashHelperCapture::Captured;
    detail::RunTerminationCallbacks(MonotonicNanoseconds() +
                                    uint64{detail::CrashOptions().callbackBudgetMilliseconds} *
                                        detail::NanosecondsPerMillisecond);
    WriteReportFiles();
    detail::MarkReportCompleted();
    SetEvent(reportFinished);
}

// 크래시 처리 스레드다. 자기 실패는 HandleCrash의 처리 스레드 분기가 받는다.
DWORD WINAPI ReporterMain(void*) noexcept {
    __try {
        ReporterLoop();
    } __except (HandleCrash(GetExceptionInformation())) {
    }

    return 0;
}

// Fatal의 보고 경로다. 보호 블록에는 RaiseException 호출 하나만 둔다.
ROCKS_NOINLINE void RaiseFatalException(const detail::FatalOrigin* origin) noexcept {
    const ULONG_PTR argument = reinterpret_cast<ULONG_PTR>(origin);
    __try {
        RaiseException(RocksFatalExceptionCode, EXCEPTION_NONCONTINUABLE, 1, &argument);
    } __except (HandleCrash(GetExceptionInformation())) {
    }
}

[[noreturn]] void FailFatally(TerminationReason reason, const char* message, const char* file,
                              uint32 line, const void* returnAddress) noexcept {
    const char* text = message != nullptr ? message : "Rocks fatal error";
    usize length = 0;
    while (length < detail::MaxFatalDiagnosticBytes && text[length] != '\0') {
        ++length;
    }

    // CRT FILE lock과 heap formatting을 거치지 않는다.
    WriteDiagnostic(text, length);
    WriteDiagnostic("\n", 1);
    if (!detail::CrashStateInstalled()) {
        TerminateWithCode(RocksFatalExceptionCode);
    }

    const uint64 self = GetCurrentThreadId();
    if (IsReporterThread(self)) {
        FailReporter();
    }

    // 내장 모드는 호스트 프로세스를 끝내지 않는다. 재귀·종료 중 Fatal도 예외로 호스트에 넘긴다
    // (HandleCrash가 CONTINUE_SEARCH를 돌려준다).
    const bool embedded = detail::CrashOptions().embedded;
    const uint64 owner = detail::CrashOwner();
    if (owner == self && !embedded) {
        detail::WriteDiagnosticLine("Rocks fatal error while reporting a crash on this thread");
        TerminateWithCode(RocksRecursiveCrashExitCode);
    }

    if (owner == detail::ShutdownCrashOwner && !embedded) {
        TerminateWithCode(RocksFatalExceptionCode);
    }

    if (!detail::CrashOptions().reportWhenDebuggerAttached && IsDebuggerPresent()) {
        ROCKS_DEBUG_BREAK();
        TerminateWithCode(RocksFatalExceptionCode);
    }

    const detail::FatalOrigin origin{reason, text, length, file, line, returnAddress};
    RaiseFatalException(&origin);
    // 보고 경로가 돌아오면(내장 모드가 아니면 도달하지 않는다) 그대로 끝낸다.
    TerminateWithCode(RocksFatalExceptionCode);
}

// abort()가 낸 SIGABRT다. UCRT raise는 처리기를 부르기 전에 SIG_DFL로 되돌리므로 첫 줄에서 다시
// 건다(그 사이 다른 스레드의 abort는 보고 없이 끝날 수 있다, 설계 8.5절 한계). 반환하지 않는다
// (반환하면 abort가 _exit(3)으로 끝낸다).
ROCKS_NOINLINE void __cdecl AbortSignalHandler(int) noexcept {
    (void)signal(SIGABRT, &AbortSignalHandler);
    FailFatally(TerminationReason::Abort, "abort() was called", nullptr, 0, ROCKS_RETURN_ADDRESS());
}

usize AppendWideText(char* buffer, usize capacity, usize length, const wchar_t* text) noexcept {
    if (text == nullptr || length + 1 >= capacity) {
        return length;
    }

    char part[RuntimeCheckPartBytes];
    const windows::TextConversion converted =
        windows::ConvertWideToUtf8(text, windows::WideLength(text, RuntimeCheckPartBytes), part,
                                   sizeof(part), windows::TextFit::Truncate);
    if (!converted) {
        return length;
    }

    for (usize index = 0; index < converted.length && length + 1 < capacity; ++index) {
        buffer[length++] = part[index];
    }

    buffer[length] = '\0';
    return length;
}

usize AppendAsciiText(char* buffer, usize capacity, usize length, const char* text) noexcept {
    for (usize index = 0; text[index] != '\0' && length + 1 < capacity; ++index) {
        buffer[length++] = text[index];
    }

    buffer[length] = '\0';
    return length;
}

// CRT 함수가 잘못된 인자를 받았다. Release CRT는 모든 인자가 nullptr·0이고, Debug CRT는 식·함수·
// 파일·줄을 준다. 메시지는 이 스레드 stack에 만들고(보고가 끝날 때까지 이 프레임은 풀리지 않는다)
// 반환하지 않는다.
ROCKS_NOINLINE void __cdecl InvalidParameterHandler(const wchar_t* expression,
                                                    const wchar_t* function, const wchar_t* file,
                                                    unsigned int line, uintptr_t) noexcept {
    char message[RuntimeCheckMessageBytes];
    usize length = AppendAsciiText(message, sizeof(message), 0,
                                   "Invalid parameter passed to a C runtime "
                                   "function");
    if (expression != nullptr) {
        length = AppendAsciiText(message, sizeof(message), length, ": ");
        length = AppendWideText(message, sizeof(message), length, expression);
        length = AppendAsciiText(message, sizeof(message), length, " in ");
        length = AppendWideText(message, sizeof(message), length, function);
        length = AppendAsciiText(message, sizeof(message), length, " (");
        length = AppendWideText(message, sizeof(message), length, file);
        char number[CodeMessageBytes];
        (void)detail::FormatCodeMessage(number, sizeof(number), ":", line, false);
        length = AppendAsciiText(message, sizeof(message), length, number);
        (void)AppendAsciiText(message, sizeof(message), length, ")");
    }

    FailFatally(TerminationReason::RuntimeCheck, message, nullptr, 0, ROCKS_RETURN_ADDRESS());
}

ROCKS_NOINLINE void __cdecl PurecallHandler() noexcept {
    FailFatally(TerminationReason::RuntimeCheck, "Pure virtual function call", nullptr, 0,
                ROCKS_RETURN_ADDRESS());
}

#if ROCKS_ADDRESS_SANITIZER
// ASan이 오류 보고(use-after-free·접근 위반 등)를 stderr에 다 쓴 뒤 프로세스를 끝내기 직전에 부른다
// (설계 13.2절). 보고는 이미 남았으므로 공통 Fatal 경로로 종료 콜백(로그 flush)과 메타데이터(reason
// Sanitizer, 덤프 없음)만 남기고 끝낸다. 반환하지 않는다.
ROCKS_NOINLINE void SanitizerDeathCallback() noexcept {
    FailFatally(TerminationReason::Sanitizer, "AddressSanitizer reported an error", nullptr, 0,
                ROCKS_RETURN_ADDRESS());
}
#endif

// 환경 변수 ROCKS_CRASH_REPORT_WITH_DEBUGGER가 "1"이면 디버거가 붙어 있어도 보고한다(설계 13.1절,
// UE -crashreports 대응). 설치 때 한 번 읽는다.
bool ReportWithDebuggerRequested() noexcept {
    constexpr DWORD ValueUnits = 4;
    wchar_t value[ValueUnits] = {};
    const DWORD length =
        GetEnvironmentVariableW(L"ROCKS_CRASH_REPORT_WITH_DEBUGGER", value, ValueUnits);
    return length == 1 && value[0] == L'1';
}

void InstallCrtHooks(const CrashReportingOptions& options) noexcept {
    previousAbortBehavior = _set_abort_behavior(0, AbortBehaviorMask);
    previousAbortSignal = signal(SIGABRT, &AbortSignalHandler);
    previousInvalidParameter = _set_invalid_parameter_handler(&InvalidParameterHandler);
    previousPurecall = _set_purecall_handler(&PurecallHandler);
    crtHooksInstalled = true;
    if (options.captureHeapCorruption) {
        // 처음(1) 순서로 건다. 다른 VEH가 먼저 삼키지 않게 한다.
        heapCorruptionHandler = AddVectoredExceptionHandler(1, &HeapCorruptionHandler);
    }
}

void RemoveCrtHooks() noexcept {
    if (heapCorruptionHandler != nullptr) {
        (void)RemoveVectoredExceptionHandler(heapCorruptionHandler);
        heapCorruptionHandler = nullptr;
    }

    if (!crtHooksInstalled) {
        return;
    }

    (void)_set_purecall_handler(previousPurecall);
    (void)_set_invalid_parameter_handler(previousInvalidParameter);
    (void)signal(SIGABRT, previousAbortSignal);
    // 그 사이 대화상자 억제를 켰으면 메시지·WER 보고를 다시 켜지 않는다.
    const unsigned int restore = AtomicLoad<MemoryOrder::Acquire>(&dialogsSuppressed) != 0
                                     ? previousAbortBehavior & ~AbortBehaviorMask
                                     : previousAbortBehavior;
    (void)_set_abort_behavior(restore, AbortBehaviorMask);
    crtHooksInstalled = false;
}

void CloseReporterHandles() noexcept {
    const HANDLE handles[] = {reporterThread, crashRequested, reportFinished};
    for (HANDLE handle : handles) {
        if (handle != nullptr) {
            CloseHandle(handle);
        }
    }

    reporterThread = nullptr;
    crashRequested = nullptr;
    reportFinished = nullptr;
}

} // namespace

CrashCapability QueryCrashCapabilities() noexcept {
    const CrashCapability base = CrashCapability::OsFaultHandling | CrashCapability::ThreadGuard |
                                 CrashCapability::TerminationCallbacks | CrashCapability::Metadata;
    if (ROCKS_ADDRESS_SANITIZER) {
        // ASan 구성은 덤프·도우미를 쓰지 않는다(DbgHelp와 sanitizer runtime의 경합, 설계 13.2절).
        return base;
    }

    if (detail::CrashStateInstalled()) {
        // 설치 뒤에는 실제 준비 상태다. 도우미가 없으면 도우미 덤프·보고 없는 종료 기록이 없고,
        // 도우미도 DbgHelp도 없으면 덤프가 없다.
        CrashCapability installed = base;
        if (detail::CrashHelperStarted()) {
            installed = installed | CrashCapability::Minidump | CrashCapability::OutOfProcessDump |
                        CrashCapability::UnreportedExitRecord;
        } else if (dumpsRequested && detail::CanWriteInProcessDump()) {
            installed = installed | CrashCapability::Minidump;
        }

        return installed;
    }

    return ROCKS_ADDRESS_SANITIZER
               ? base
               : base | CrashCapability::Minidump | CrashCapability::OutOfProcessDump |
                     CrashCapability::UnreportedExitRecord;
}

CrashReportingStatus InstallCrashReporting(const CrashReportingOptions& options) noexcept {
    if (detail::CrashStateInstalled()) {
        return CrashReportingStatus::AlreadyInstalled;
    }

    if (!detail::ValidateCrashOptions(options) || !detail::PrepareCrashIdentity(options)) {
        return CrashReportingStatus::InvalidArgument;
    }

    const bool wantDumps = !options.embedded && !ROCKS_ADDRESS_SANITIZER;
    if (!detail::PrepareCrashFiles(options.outputDirectory, wantDumps)) {
        return CrashReportingStatus::InvalidArgument;
    }

    CrashReportingOptions effective = options;
    effective.reportWhenDebuggerAttached =
        options.reportWhenDebuggerAttached || ReportWithDebuggerRequested();
    detail::ResetCrashState(effective);
    dumpsRequested = wantDumps;
    inProcessDump = wantDumps && detail::CanWriteInProcessDump();
    AtomicStore<MemoryOrder::Relaxed>(&stopping, 0u);
    crashRequested = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    reportFinished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (crashRequested == nullptr || reportFinished == nullptr) {
        CloseReporterHandles();
        detail::ReleaseCrashFiles();
        return CrashReportingStatus::OutOfResources;
    }

    DWORD threadId = 0;
    reporterThread = CreateThread(nullptr, ReporterStackBytes, &ReporterMain, nullptr,
                                  STACK_SIZE_PARAM_IS_A_RESERVATION, &threadId);
    if (reporterThread == nullptr) {
        CloseReporterHandles();
        detail::ReleaseCrashFiles();
        return CrashReportingStatus::OutOfResources;
    }

    detail::SetReporterThreadId(threadId);
    // 도우미는 처리기를 걸기 전에 띄운다(시작 실패는 설치 실패가 아니다).
    helperCaptured = false;
    detail::PrepareCrashHelper(options);
    PrepareThreadForCrashHandling();
    if (!options.embedded) {
        previousFilter = SetUnhandledExceptionFilter(&UnhandledFilter);
        InstallCrtHooks(options);
#if ROCKS_ADDRESS_SANITIZER
        __sanitizer_set_death_callback(&SanitizerDeathCallback);
#endif
    }

    detail::SetCrashStateInstalled(true);
    return CrashReportingStatus::Installed;
}

void RemoveCrashReporting() noexcept {
    if (!detail::CrashStateInstalled()) {
        return;
    }

    if (!detail::BlockCrashOwnerForShutdown() && !detail::ReportCompleted()) {
        // 보고가 진행 중이다. 종료는 크래시한 스레드가 한다. 내장 모드에서 보고를 마친 뒤 호스트가
        // 실행을 이어 간 경우만 그대로 정리한다.
        detail::ParkCurrentThread();
    }

    // 도우미에 Shutdown을 먼저 알린다(정상 종료는 어떤 종료 코드든 기록하지 않는다).
    detail::StopCrashHelper();
    if (!detail::CrashOptions().embedded) {
#if ROCKS_ADDRESS_SANITIZER
        __sanitizer_set_death_callback(nullptr);
#endif
        RemoveCrtHooks();
        // 지금 filter가 우리 것일 때만 설치 전 filter로 되돌린다. 다른 코드가 바꿔 둔 filter(교환
        // 확인이 그대로 둔 것)는 빼앗지 않고 다시 건다(설계 8.6절). 그 filter가 우리 filter로 이어
        // 주면 해제 뒤에도 UnhandledFilter가 불리고(수집기가 없으므로 CONTINUE_SEARCH) 설치 전
        // filter(CRT 등)로 넘겨야 하므로, 이때는 previousFilter를 비우지 않는다.
        // 한계: 두 SetUnhandledExceptionFilter 호출 사이의 아주 짧은 창에서 다른 스레드의 처리되지
        // 않은 예외는 설치 전 filter가 받는다(그 filter를 바꾸던 다른 스레드가 있으면 그 변경도
        // 덮일 수 있다). 해제는 정상 종료 경로에서 한 번이므로 받아들인다.
        const LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(previousFilter);
        if (current != &UnhandledFilter) {
            (void)SetUnhandledExceptionFilter(current);
        } else {
            previousFilter = nullptr;
        }
    }

    detail::SetCrashStateInstalled(false);
    AtomicStore<MemoryOrder::Release>(&stopping, 1u);
    SetEvent(crashRequested);
    (void)WaitForSingleObject(reporterThread, INFINITE);
    CloseReporterHandles();
    detail::ReleaseCrashFiles();
    inProcessDump = false;
    dumpsRequested = false;
    detail::SetReporterThreadId(0);
    detail::ReleaseCrashOwnerAfterShutdown();
}

bool CheckUnhandledCrashFilter() noexcept {
    if (!detail::CrashStateInstalled() || detail::CrashOptions().embedded) {
        return true;
    }

    // 현재 filter를 읽는 API가 없어 교환으로 본다. 우리 것이 아니면 남의 선택을 빼앗지 않도록 바로
    // 되돌린다(설계 8.6절). 두 호출 사이의 외부 스레드 크래시는 우리 filter가 받는다.
    const LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(&UnhandledFilter);
    if (current == &UnhandledFilter) {
        return true;
    }

    (void)SetUnhandledExceptionFilter(current);
    return false;
}

bool SetCrashOutputDirectory(const char* directory) noexcept {
    if (!detail::CrashStateInstalled() || !detail::ResolveCrashOutputDirectory(directory)) {
        return false;
    }

    detail::UpdateCrashHelperFolders();
    return true;
}

usize CopyCrashReportFolder(char* buffer, usize capacity) noexcept {
    if (buffer == nullptr || capacity == 0 || !detail::CrashStateInstalled()) {
        return 0;
    }

    const char* folder = detail::CrashPrimaryFolderText();
    usize length = 0;
    while (folder[length] != '\0') {
        ++length;
    }

    if (length + 1 > capacity) {
        buffer[0] = '\0';
        return 0;
    }

    for (usize index = 0; index <= length; ++index) {
        buffer[index] = folder[index];
    }

    return length;
}

void SuppressSystemErrorDialogs() noexcept {
    AtomicStore<MemoryOrder::Release>(&dialogsSuppressed, 1u);
    // 자식 프로세스도 물려받는다(Process.h). 처리되지 않은 예외·파일 열기 실패 대화상자를 끈다.
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
    // abort의 "Debug Error" 대화상자와 WER fault 보고를 끈다.
    (void)_set_abort_behavior(0, AbortBehaviorMask);
    // Debug CRT의 assert·error·warning 보고를 대화상자 대신 stderr로 보낸다(Release CRT에서는
    // 아무 일도 하지 않는 매크로다).
    (void)_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    // 이 프로세스의 WER 대화상자를 끈다(프로세스 설정, 시스템 정책은 바꾸지 않는다).
    (void)WerSetFlags(WER_FAULT_REPORTING_NO_UI);
}

ROCKS_NOINLINE int RunWithCrashGuard(GuardedBody body, void* context) noexcept {
    // 상수 전파로 body가 보호 블록 안에 인라인되지 않게 volatile로 읽는다.
    GuardedBody volatile target = body;
    __try {
        return target(context);
    } __except (HandleCrash(GetExceptionInformation())) {
        return -1;
    }
}

void PrepareThreadForCrashHandling() noexcept {
    // 이미 더 큰 값이면 줄이지 않는다(실패는 무시한다, 처리기가 덜 안전해질 뿐이다).
    ULONG guarantee = static_cast<ULONG>(CrashStackReserveBytes);
    (void)SetThreadStackGuarantee(&guarantee);
}

void ReleaseThreadCrashHandling() noexcept {
    // Windows는 스레드별로 따로 잡은 것이 없다(stack 보장분은 스레드 stack 안이다).
}

ROCKS_NOINLINE void Fatal(const char* message) noexcept {
    FailFatally(TerminationReason::Fatal, message, nullptr, 0, ROCKS_RETURN_ADDRESS());
}

ROCKS_NOINLINE void FatalAt(const char* message, const char* file, uint32 line) noexcept {
    FailFatally(TerminationReason::Assert, message, file, line, ROCKS_RETURN_ADDRESS());
}

} // namespace rocks::platform
