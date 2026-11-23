#include <TestSupport/HostFailure.h>
#include <TestSupport/Test.h>
#include <Base/Compiler.h>
#include <Core/Assert.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Launch/Application.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/CrashReporting.h>
#include <Platform/Fatal.h>
#include <Platform/File.h>
#include <Platform/Memory.h>
#include <Platform/ProcessEntry.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>

#include <stdlib.h>
#include <string.h>

// 크래시 수집기(Docs/Design/CrashHandlingAndLaunch.ko.md LC1a·LC1b·LC2)의 자식 probe다. 시험(C#
// Native.Crash.*)이 `--negative-control crash-<mode> <crash directory>`로 띄우고 종료 코드·stderr
// 줄·크래시 폴더(CrashInfo.json·Minidump.dmp)를 단언한다. 의도적 장애 주입은 이 파일에만 있다.
// 수집기 설치는 시험 실행기의 Launch 설정이 한다(ConfigureCrashProbe가 mode별 수집 방식·시험용
// 예산·폴더를 고르고, 본문 RunCrashProbe는 설치를 확인한 뒤 실패를 낸다). 정상이면 반환하지 않는다.
namespace rocks::test {

namespace {

inline constexpr LogCategory LogCrashReportingProbe{"Tests.CrashReporting"};

// 준비 실패 종료 코드다. 크래시 종료 코드(예외 코드·Rocks 코드)와 겹치지 않는다.
constexpr int UnknownMode = 30;
constexpr int InstallFailed = 31;
constexpr int SetupFailed = 32;
constexpr int DidNotCrash = 33;
constexpr int MissingDirectory = 34;

// 크래시가 끝내지 않을 때 probe가 기다리는 상한이다(시험이 무한히 기다리지 않게).
constexpr uint32 CrashWaitMilliseconds = 60000;
// 동시 실패 probe의 콜백 예산이다(두 번째 실패가 칸에 남을 때까지 기다릴 여유).
constexpr uint32 ConcurrentCallbackBudgetMilliseconds = 10000;
// 겹친 실패 스레드의 여유와 프로세스 안 덤프 마감이다(설계 17절의 시험용 짧은 예산).
constexpr uint32 ProbeConcurrentGraceMilliseconds = 1000;
constexpr uint32 ProbeDumpTimeoutMilliseconds = 20000;
// inprocess-dump-lock-holder의 프로세스 안 덤프 마감이다. 덤프가 heap lock에서 막히므로 크래시한
// 스레드가 이 마감과 남은 예산 뒤 프로세스를 끝낸다(짧게 잡아 시험이 빨리 끝난다).
constexpr uint32 ProbeLockedDumpTimeoutMilliseconds = 2000;
// 도우미 캡처 마감이다(시험용 짧은 예산). 고장 난 도우미 시험이 이만큼 기다린다.
constexpr uint32 ProbeCaptureTimeoutMilliseconds = 5000;
// Full 덤프 마감 시험: 추가 덤프 마감 1 ms와 그보다 훨씬 오래 걸릴 큰 메모리(쓴 페이지)다.
constexpr uint32 ProbeTinyDumpDeadlineMilliseconds = 1;
constexpr usize ProbeLargeMemoryBytes = 256ull * 1024 * 1024;
constexpr usize ProbePageBytes = 4096;
constexpr usize ProbeLargeMemoryAlignment = 4096;
// 크래시 전에 끝난 도우미 시험이 도우미 종료를 기다리는 상한이다.
constexpr uint32 HelperExitWaitMilliseconds = 30000;
constexpr int ProbeExitCode = 7;
constexpr usize LineBytes = 256;
// 메타데이터 associatedFiles.logFile에 남는 시험 값이다.
constexpr char ProbeLogFile[] = "Saved/Logs/CrashProbe.log";
constexpr char WorkerThreadName[] = "CrashProbeWorker";

int* volatile nullTarget = nullptr;

void WriteLine(StringView text) noexcept {
    platform::WriteDiagnostic(text.Data(), text.ByteCount());
    platform::WriteDiagnostic("\n", 1);
}

ROCKS_NOINLINE void FaultAccess() noexcept {
    *nullTarget = 1;
}

// 끝없는 재귀로 stack을 넘친다. 깊이 검사는 무한 재귀 경고를 피하려는 것이고, 닿지 않는다.
ROCKS_NOINLINE uint64 OverflowStack(uint64 depth) noexcept {
    constexpr usize OverflowFrameBytes = 1024;
    volatile uint8 frame[OverflowFrameBytes];
    frame[0] = static_cast<uint8>(depth);
    if (depth == NumericLimits<uint64>::Max()) {
        return frame[0];
    }

    return OverflowStack(depth + 1) + frame[0];
}

// cdb stack 확인의 기대 프레임 이름이다(C# 시험이 같은 이름을 찾는다).
ROCKS_NOINLINE void AbortFromProbe() noexcept {
    abort();
}

ROCKS_NOINLINE void FatalFromProbe() noexcept {
    platform::Fatal("crash probe: platform fatal");
}

void AnnounceThread(const char* role) noexcept {
    char line[LineBytes];
    FixedStringBuilder text{Span<char>(line)};
    (void)text.TryAppendFormat("crash probe {} thread={}", role, platform::CurrentThreadId());
    WriteLine(text.View());
}

// 콜백 동작이다.
enum class CallbackAction : uint8 {
    Report,
    Fault,
    Fatal,
    Abort,
    AwaitStopped
};

struct ProbeState {
    CallbackAction action = CallbackAction::Report;
    platform::ManualResetEvent callbackEntered;
};

ProbeState probeState;

platform::TerminationCallbackResult ProbeCallback(const platform::TerminationContext& context,
                                                  void* user) noexcept {
    auto& state = *static_cast<ProbeState*>(user);
    if (state.action == CallbackAction::Fault) {
        FaultAccess();
    } else if (state.action == CallbackAction::Fatal) {
        platform::Fatal("crash probe: fatal inside a termination callback");
    } else if (state.action == CallbackAction::Abort) {
        abort();
    } else if (state.action == CallbackAction::AwaitStopped) {
        // 시험의 gate다: 콜백에 들어왔음을 알리고, 두 번째 실패가 칸에 남을 때까지 기다린다.
        state.callbackEntered.Signal();
        while (platform::AtomicLoad<platform::MemoryOrder::Acquire>(context.stoppedThreadCount) ==
                   0 &&
               platform::MonotonicNanoseconds() < context.deadlineNanoseconds) {
            platform::YieldThread();
        }
    }

    char line[LineBytes];
    FixedStringBuilder text{Span<char>(line)};
    const uint32 stopped =
        platform::AtomicLoad<platform::MemoryOrder::Acquire>(context.stoppedThreadCount);
    (void)text.TryAppendFormat(
        "crash probe callback reason={} failing={} stopped={} first={} message={}",
        static_cast<uint32>(context.reason), context.failingThreadId, stopped,
        stopped != 0 ? context.stoppedThreadIds[0] : uint64{0},
        StringView(context.message, context.messageBytes));
    WriteLine(text.View());
    return platform::TerminationCallbackResult::Completed;
}

// Launch가 설치한 수집기에 probe 표시(도우미 pid 줄·단계·연관 파일·종료 콜백)를 단다.
bool PrepareInstalled() noexcept {
    if (!platform::IsCrashReportingInstalled()) {
        return false;
    }

    // 시험이 도우미 프로세스의 종료를 기다린다(덤프는 부모가 끝난 뒤에도 쓰인다).
    char line[LineBytes];
    FixedStringBuilder text{Span<char>(line)};
    (void)text.TryAppendFormat("crash probe helper pid={}", platform::CrashHelperProcessId());
    WriteLine(text.View());

    platform::SetRunPhase(platform::RunPhase::Running, "CrashProbe.Run");
    (void)platform::SetCrashAnnotation(platform::CrashAnnotation::LogFile, ProbeLogFile,
                                       sizeof(ProbeLogFile) - 1);
    platform::TerminationCallbackHandle handle;
    return platform::RegisterTerminationCallback(&ProbeCallback, &probeState, handle);
}

void ThreadFault(void*) noexcept {
    AnnounceThread("worker");
    FaultAccess();
}

void ThreadOverflow(void*) noexcept {
    AnnounceThread("worker");
    (void)OverflowStack(0);
}

void ThreadAbort(void*) noexcept {
    AnnounceThread("worker");
    AbortFromProbe();
}

void ThreadFatal(void*) noexcept {
    AnnounceThread("second");
    platform::Fatal("crash probe: second thread fatal");
}

// 크래시가 프로세스를 끝내기를 기다린다. 끝나지 않으면 DidNotCrash다.
int AwaitCrash() noexcept {
    platform::ManualResetEvent never;
    (void)never.Wait(CrashWaitMilliseconds);
    return DidNotCrash;
}

int RunInPlatformThread(platform::Thread::Entry entry) noexcept {
    static platform::Thread thread;
    platform::ThreadOptions options;
    options.name = WorkerThreadName;
    if (thread.Start(entry, nullptr, options) != platform::ThreadStatus::Success) {
        return SetupFailed;
    }

    return AwaitCrash();
}

int RunInUnmanagedThread(void (*entry)(void*) noexcept) noexcept {
    if (!StartUnmanagedThread(entry, nullptr)) {
        return SetupFailed;
    }

    return AwaitCrash();
}

// 스레드 A가 실패하고, 콜백이 gate에 들어온 뒤 스레드 B가 실패한다. 보고는 A 하나이고 B는 칸에
// 남는다.
int RunConcurrent() noexcept {
    probeState.action = CallbackAction::AwaitStopped;
    static platform::Thread first;
    static platform::Thread second;
    if (first.Start(&ThreadFault, nullptr) != platform::ThreadStatus::Success) {
        return SetupFailed;
    }

    if (probeState.callbackEntered.Wait(CrashWaitMilliseconds) != platform::WaitResult::Signaled) {
        return SetupFailed;
    }

    if (second.Start(&ThreadFatal, nullptr) != platform::ThreadStatus::Success) {
        return SetupFailed;
    }

    return AwaitCrash();
}

// Write가 출력 스레드 안에서 접근 위반을 내는 sink다(출력 스레드 자신의 크래시).
class FaultingSink final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent&) noexcept override {
        AnnounceThread("worker");
        FaultAccess();
        return Result<void>::Success();
    }

    Result<void> Flush() noexcept override {
        return Result<void>::Success();
    }
};

// 출력 스레드가 크래시하면 로그 종료 콜백은 큐를 기다리지 않고 비상 문구만 남긴다.
int RunWriterFault() noexcept {
    static SystemAllocator allocator;
    static FaultingSink sink;
    static LogSystem logs(allocator, LogSystemConfig::ForEngineHost());
    if (!logs.AddSink(sink) || !logs.Start()) {
        return SetupFailed;
    }

    static LogInstallScope installed(logs);
    Log::Info(LogCrashReportingProbe, "crash probe record that crashes the writer");
    return AwaitCrash();
}

// Async LogSystem(stderr sink)을 설치한 채 Log::Fatal한다.
int RunLogFatal() noexcept {
    static SystemAllocator allocator;
    static StderrLogSink sink;
    static LogSystem logs(allocator, LogSystemConfig::ForEngineHost());
    if (!logs.AddSink(sink) || !logs.Start()) {
        return SetupFailed;
    }

    static LogInstallScope installed(logs);
    Log::Info(LogCrashReportingProbe, "crash probe record before fatal");
    Log::Fatal(LogCrashReportingProbe, "crash probe log fatal {Value}", 7);
}

bool CrashModeIs(const char* mode, const char* name) noexcept {
    return strcmp(mode, name) == 0;
}

// 큰 메모리를 잡아 모든 페이지를 쓴 뒤 크래시한다(Full 덤프가 마감 안에 끝나지 않게).
int RunLargeMemoryFault() noexcept {
    auto* memory = static_cast<uint8*>(
        platform::AlignedAllocate(ProbeLargeMemoryBytes, ProbeLargeMemoryAlignment));
    if (memory == nullptr) {
        return SetupFailed;
    }

    for (usize offset = 0; offset < ProbeLargeMemoryBytes; offset += ProbePageBytes) {
        memory[offset] = static_cast<uint8>(offset / ProbePageBytes);
    }

    FaultAccess();
    return DidNotCrash;
}

// 크래시 폴더 안에 CrashInfo.json·Minidump.dmp와 같은 이름의 폴더를 미리 만든다. 폴더는 만들어져
// 있지만 파일을 만들 수 없는 경우다(쓰는 쪽이 임시 폴더로 옮겨야 한다). dumpOnly면 Minidump.dmp만
// 막는다(예비 CrashInfo.json은 쓸 수 있지만 덤프 파일을 만들 수 없다).
bool BlockCrashFiles(bool dumpOnly) noexcept {
    char folder[platform::CrashReportingOptions::MaxOutputDirectoryBytes + 256];
    const usize length = platform::CopyCrashReportFolder(folder, sizeof(folder));
    if (length == 0) {
        return false;
    }

    constexpr const char* BlockedNames[] = {"/Minidump.dmp", "/CrashInfo.json"};
    for (const char* name : Span<const char* const>(BlockedNames, dumpOnly ? 1 : 2)) {
        char path[sizeof(folder) + 32];
        FixedStringBuilder text{Span<char>(path)};
        if (!text.TryAppendFormat("{}{}", StringView(folder, length), name) || text.Truncated() ||
            !platform::MakeDirectoryTree(platform::PhysicalFileSystem(), path, text.ByteCount())) {
            return false;
        }
    }

    return true;
}

// 시험 폴더 안의 없는 실행 파일 경로다(도우미 시작 실패 시험).
char missingHelper[platform::CrashReportingOptions::MaxHelperExecutableBytes + 1];

// 설정 단계의 준비 실패다(본문이 SetupFailed로 끝낸다).
bool configureFailed = false;

bool IsJobMode(const char* mode) noexcept {
    return CrashModeIs(mode, "job-kill-on-close") ||
           CrashModeIs(mode, "job-kill-on-close-breakaway") ||
           CrashModeIs(mode, "job-kill-on-close-breakaway-blocked");
}

} // namespace

void ConfigureCrashProbe(const char* mode, const char* directory,
                         launch::ApplicationSettings& settings) noexcept {
    // 수집기 미설치 Fatal과 폴더 없는 호출은 수집하지 않는다(본문이 결과를 낸다).
    if (CrashModeIs(mode, "off-fatal") || directory == nullptr) {
        settings.crashReporting = launch::CrashReportingMode::Off;
        return;
    }

    settings.crashDirectory = directory;
    settings.crashCallbackBudgetMilliseconds =
        CrashModeIs(mode, "concurrent") ? ConcurrentCallbackBudgetMilliseconds : 0u;
    settings.concurrentCrashGraceMilliseconds = ProbeConcurrentGraceMilliseconds;
    settings.inProcessDumpTimeoutMilliseconds = ProbeDumpTimeoutMilliseconds;
    settings.crashCaptureTimeoutMilliseconds = ProbeCaptureTimeoutMilliseconds;
    settings.dumpDetail = CrashModeIs(mode, "detailed-dump") ? platform::CrashDumpDetail::Detailed
                                                             : platform::CrashDumpDetail::Standard;
    settings.captureHeapCorruption =
        CrashModeIs(mode, "heap-corruption") || CrashModeIs(mode, "process-heap-corruption");
    settings.crashReporting = launch::CrashReportingMode::Full;
    if (CrashModeIs(mode, "inprocess-dump-lock-holder")) {
        settings.crashReporting = launch::CrashReportingMode::InProcess;
        settings.inProcessDumpTimeoutMilliseconds = ProbeLockedDumpTimeoutMilliseconds;
    } else if (CrashModeIs(mode, "inprocess-access-violation") ||
               CrashModeIs(mode, "files-blocked-inprocess") ||
               CrashModeIs(mode, "dump-blocked-inprocess") ||
               CrashModeIs(mode, "inprocess-platform-fatal")) {
        settings.crashReporting = launch::CrashReportingMode::InProcess;
    } else if (CrashModeIs(mode, "helper-start-failure")) {
        FixedStringBuilder path{Span<char>(missingHelper)};
        configureFailed =
            !path.TryAppendFormat("{}/missing-crash-helper.exe", directory) || path.Truncated();
        settings.crashHelperPath = configureFailed ? nullptr : missingHelper;
    } else if (CrashModeIs(mode, "full-dump-deadline")) {
        settings.dumpDetail = platform::CrashDumpDetail::Full;
        settings.helperDumpDeadlineMilliseconds = ProbeTinyDumpDeadlineMilliseconds;
    }

    // job 시험: KILL_ON_JOB_CLOSE job에 들어간 뒤 도우미를 띄운다(도우미는 설정 단계 끝에 뜬다). 이
    // 프로세스가 끝나면 job이 닫혀 job 안 프로세스가 끝난다. breakaway를 막은 job은 도우미가 job
    // 안에 남고, 허락한 job은 도우미만 빠져나가고 VA clone은 job에 남을 수 있다(부모는 어느 쪽이든
    // 기본 덤프가 끝날 때까지 기다려야 한다).
    // -blocked 변형은 breakaway를 막은 바깥 job에 먼저 들어간다(시험 실행기가 그런 job 안에 있는 CI
    // 러너 조건, 설계 17절). 안쪽 job이 허락해도 도우미는 job 안에 남는다.
    const bool blocked = CrashModeIs(mode, "job-kill-on-close-breakaway-blocked");
    const bool allowBreakaway = blocked || CrashModeIs(mode, "job-kill-on-close-breakaway");
    if (IsJobMode(mode) &&
        ((blocked && !EnterBreakawayBlockingJob()) || !EnterKillOnCloseJob(allowBreakaway))) {
        configureFailed = true;
    }
}

int RunCrashProbe(const char* mode, const char* directory) noexcept {
    if (CrashModeIs(mode, "off-fatal")) {
        // 수집기 미설치: 비상 출력 뒤 바로 끝난다(콜백·파일 없음).
        AnnounceThread("main");
        platform::Fatal("crash probe: fatal without crash reporting");
    }

    if (directory == nullptr) {
        return MissingDirectory;
    }

    if (configureFailed) {
        return SetupFailed;
    }

    if (!PrepareInstalled()) {
        return InstallFailed;
    }

    AnnounceThread("main");
    if (CrashModeIs(mode, "main-access-violation") || CrashModeIs(mode, "detailed-dump") ||
        CrashModeIs(mode, "inprocess-access-violation") ||
        CrashModeIs(mode, "helper-start-failure") || CrashModeIs(mode, "helper-capture-timeout") ||
        CrashModeIs(mode, "helper-exit-during-capture") ||
        CrashModeIs(mode, "helper-gate-parent-first") || IsJobMode(mode)) {
        // 도우미 시험은 C#이 고장 난 도우미를 환경 변수로 고른다.
        FaultAccess();
    } else if (CrashModeIs(mode, "inprocess-dump-lock-holder")) {
        // 프로세스 안 덤프가 스레드 목록 단계에 들어오면 다른 스레드가 프로세스 heap lock을 쥐고
        // 멈춘다(설계 11.5절). 덤프는 그 lock에서 막히지만, 덤프가 살아 있는 스레드를 멈추지
        // 않으므로 이 스레드(크래시한 스레드)가 마감에 프로세스를 끝낸다.
        if (!HoldProcessHeapDuringDump()) {
            return SetupFailed;
        }

        FaultAccess();
    } else if (CrashModeIs(mode, "job-after-helper")) {
        // 도우미를 띄운 뒤 부모만 breakaway 허용 KILL_ON_JOB_CLOSE job에 들어간다(harness가 띄운 뒤
        // job에 넣는 경우). 도우미는 job 밖이고 VA clone은 job 안이다. 부모는 크래시 때 job 여부를
        // 다시 읽어 기본 덤프까지 기다려야 한다.
        if (!EnterKillOnCloseJob(true)) {
            return SetupFailed;
        }

        FaultAccess();
    } else if (CrashModeIs(mode, "helper-exited-early")) {
        // 도우미가 먼저 끝난 것을 확인한 뒤 실패한다(순서는 프로세스 종료 대기로 정한다).
        if (!WaitForProcessExit(platform::CrashHelperProcessId(), HelperExitWaitMilliseconds)) {
            return SetupFailed;
        }

        FaultAccess();
    } else if (CrashModeIs(mode, "full-dump-deadline")) {
        return RunLargeMemoryFault();
    } else if (CrashModeIs(mode, "files-blocked") || CrashModeIs(mode, "files-blocked-inprocess") ||
               CrashModeIs(mode, "dump-blocked") || CrashModeIs(mode, "dump-blocked-inprocess")) {
        if (!BlockCrashFiles(CrashModeIs(mode, "dump-blocked") ||
                             CrashModeIs(mode, "dump-blocked-inprocess"))) {
            return SetupFailed;
        }

        FaultAccess();
    } else if (CrashModeIs(mode, "process-heap-corruption")) {
        return CorruptProcessHeap() ? DidNotCrash : UnknownMode;
    } else if (CrashModeIs(mode, "fastfail")) {
        return FailFast() ? DidNotCrash : UnknownMode;
    } else if (CrashModeIs(mode, "control-c-exit")) {
        return ExitWithControlCStatus() ? DidNotCrash : UnknownMode;
    } else if (CrashModeIs(mode, "exit-7")) {
        // 정상 종료(정리 없이 바로 — 공유 라이브러리의 정적 소멸도 돌지 않는다)다. 도우미는 기록하지
        // 않는다.
        rocks::platform::ExitProcessWithoutTeardown(ProbeExitCode);
    } else if (CrashModeIs(mode, "normal-exit")) {
        // 정상 종료: Launch가 수집기를 떼며 도우미에 Shutdown을 알리고 끝낸다(도우미가 남지
        // 않는다).
        return 0;
    } else if (CrashModeIs(mode, "wait-for-kill")) {
        // 시험이 도우미 pid 줄을 본 뒤 이 프로세스를 kill한다.
        return AwaitCrash();
    } else if (CrashModeIs(mode, "main-stack-overflow")) {
        (void)OverflowStack(0);
    } else if (CrashModeIs(mode, "thread-access-violation")) {
        return RunInPlatformThread(&ThreadFault);
    } else if (CrashModeIs(mode, "thread-stack-overflow")) {
        return RunInPlatformThread(&ThreadOverflow);
    } else if (CrashModeIs(mode, "unmanaged-thread-access-violation")) {
        return RunInUnmanagedThread(&ThreadFault);
    } else if (CrashModeIs(mode, "platform-fatal") ||
               CrashModeIs(mode, "inprocess-platform-fatal")) {
        FatalFromProbe();
    } else if (CrashModeIs(mode, "assert")) {
        ROCKS_ASSERT(nullTarget != nullptr);
    } else if (CrashModeIs(mode, "log-fatal")) {
        return RunLogFatal();
    } else if (CrashModeIs(mode, "writer-fault")) {
        return RunWriterFault();
    } else if (CrashModeIs(mode, "concurrent")) {
        return RunConcurrent();
    } else if (CrashModeIs(mode, "callback-fault")) {
        probeState.action = CallbackAction::Fault;
        FaultAccess();
    } else if (CrashModeIs(mode, "callback-fatal")) {
        probeState.action = CallbackAction::Fatal;
        FaultAccess();
    } else if (CrashModeIs(mode, "abort-main")) {
        AbortFromProbe();
    } else if (CrashModeIs(mode, "abort-unmanaged-thread")) {
        return RunInUnmanagedThread(&ThreadAbort);
    } else if (CrashModeIs(mode, "callback-abort")) {
        // 처리 스레드가 콜백 안에서 abort해도 SIGABRT 처리기는 반환하지 않는다(종료 코드 3이
        // 아니다). 남은 콜백만 포기하고 파일 단계를 이어 간다.
        probeState.action = CallbackAction::Abort;
        AbortFromProbe();
    } else if (CrashModeIs(mode, "invalid-parameter")) {
        return PassInvalidParameter() ? DidNotCrash : UnknownMode;
    } else if (CrashModeIs(mode, "user-exception")) {
        return RaiseUserException() ? DidNotCrash : UnknownMode;
    } else if (CrashModeIs(mode, "purecall")) {
        return CallPureVirtual() ? DidNotCrash : UnknownMode;
    } else if (CrashModeIs(mode, "asan-uaf")) {
        // Asan 구성: ASan이 use-after-free를 보고한 뒤 death callback이 공통 보고(reason
        // Sanitizer)를 한다. 다른 구성은 읽기가 그대로 성공한다(DidNotCrash).
        (void)RunAsanNegativeControl();
        return DidNotCrash;
    } else if (CrashModeIs(mode, "heap-corruption")) {
        return CorruptHeap() ? DidNotCrash : UnknownMode;
    } else {
        return UnknownMode;
    }

    return DidNotCrash;
}

} // namespace rocks::test
