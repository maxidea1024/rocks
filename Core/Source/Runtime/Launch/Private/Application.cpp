#include "BuildIdentity.h"
#include "LaunchStages.h"
#include "TerminationRequest.h"

#include <Base/Compiler.h>
#include <Base/TypeTraits.h>
#include <Core/App/ProcessCommandLine.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Error.h>
#include <Core/Memory/BinnedAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Launch/Application.h>
#include <ModuleLoader/ModuleLoader.h>
#include <Platform/Atomics.h>
#include <Platform/CrashReporting.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/ProcessEntry.h>

#include <new>
#include <stdlib.h>

namespace rocks::launch {

namespace {

inline constexpr LogCategory LogLaunch{"Launch"};

// 기본 크래시 폴더 뿌리다(platform 기본값과 같다). 설정이 폴더를 지웠을 때 되돌릴 값이다.
constexpr char DefaultCrashDirectory[] = "Saved/Crashes";

// 명령줄 캡처 실패 줄의 칸이다(원인 이름과 상한 숫자를 담는다).
constexpr usize DiagnosticLineBytes = 256;

// 정리 뒤 lease가 남은 모듈을 이름까지 기록하는 상한이다(넘는 모듈은 수만 기록한다).
constexpr usize MaxLeaseReportNames = 16;

constinit uint32 terminationRequested = 0;

void OnTerminationRequest() noexcept {
    RequestTermination();
}

// 로그를 시작하기 전(또는 시작하지 못한) 단계의 진단은 stderr에 바로 쓴다.
void WriteText(const char* text) noexcept {
    usize length = 0;
    while (text[length] != '\0') {
        ++length;
    }

    platform::WriteDiagnostic(text, length);
}

void WriteLine(const char* text) noexcept {
    WriteText(text);
    platform::WriteDiagnostic("\n", 1);
}

bool SameText(const char* left, const char* right) noexcept {
    if (left == nullptr || right == nullptr) {
        return left == right;
    }

    return platform::CStringEquals(left, right);
}

bool SuppressesDialogs(ApplicationKind kind, const ApplicationSettings& settings) noexcept {
    return kind == ApplicationKind::TestRunner || settings.suppressSystemErrorDialogs;
}

// 크래시 수집 설정 칸(폴더 제외)을 source에서 target으로 옮긴다. 새 설정을 쓸 수 없을 때 이전
// 설정으로 되돌린다.
void CopyCrashInstallation(const ApplicationSettings& source,
                           ApplicationSettings& target) noexcept {
    target.crashReporting = source.crashReporting;
    target.dumpDetail = source.dumpDetail;
    target.helperDumpDeadlineMilliseconds = source.helperDumpDeadlineMilliseconds;
    target.crashCallbackBudgetMilliseconds = source.crashCallbackBudgetMilliseconds;
    target.crashCaptureTimeoutMilliseconds = source.crashCaptureTimeoutMilliseconds;
    target.inProcessDumpTimeoutMilliseconds = source.inProcessDumpTimeoutMilliseconds;
    target.concurrentCrashGraceMilliseconds = source.concurrentCrashGraceMilliseconds;
    target.reportWhenDebuggerAttached = source.reportWhenDebuggerAttached;
    target.captureHeapCorruption = source.captureHeapCorruption;
    target.crashHelperPath = source.crashHelperPath;
}

// 0이 아니면 그 값, 0이면 기본값이다.
uint32 OrDefault(uint32 value, uint32 fallback) noexcept {
    return value != 0 ? value : fallback;
}

} // namespace

// RunApplication의 상태와 단계다. ApplicationContext의 private 칸을 채운다.
class ApplicationRunner {
public:
    ApplicationRunner(const ApplicationDescription& description, int argc, wchar_t** wide,
                      char** narrow, bool embedded) noexcept
        : description_(description), argc_(argc), wide_(wide), narrow_(narrow), embedded_(embedded),
          settings_(description.settings) {
    }

    int Run() noexcept;

    void MarkStage(platform::RunPhase phase, const char* name) noexcept {
        platform::SetRunPhase(phase, name);
    }

private:
    static constexpr usize StageCount = 6;

    // 보호 프레임 안의 본문이다(인라인되지 않는 별도 함수, D34).
    static int GuardedStages(void* context) noexcept;
    static int RunBody(ApplicationRunner& runner) noexcept;

    static bool StartCommandLine(ApplicationRunner& runner) noexcept;
    static bool StartSettings(ApplicationRunner& runner) noexcept;
    static bool StartTerminationRequests(ApplicationRunner& runner) noexcept;
    static void StopTerminationRequests(ApplicationRunner& runner) noexcept;
    static bool StartDefaultAllocator(ApplicationRunner& runner) noexcept;
    static void StopDefaultAllocator(ApplicationRunner& runner) noexcept;
    static bool StartToolLog(ApplicationRunner& runner) noexcept;
    static void StopToolLog(ApplicationRunner& runner) noexcept;
    static bool StartModules(ApplicationRunner& runner) noexcept;
    static void StopModules(ApplicationRunner& runner) noexcept;

    // 범용 할당자다(Binned가 아니면 SystemAllocator).
    Allocator& DefaultAllocatorObject() noexcept {
        return binned_ != nullptr ? static_cast<Allocator&>(*binned_) : allocator_;
    }

    platform::CrashReportingOptions
    MakeCrashOptions(const ApplicationSettings& settings) const noexcept;
    bool SameCrashInstallation(const ApplicationSettings& left,
                               const ApplicationSettings& right) const noexcept;
    bool InstallCrashReporting(const ApplicationSettings& settings) noexcept;
    bool ReplaceCrashReporting(const ApplicationSettings& early) noexcept;
    void ApplyCrashSettings(const ApplicationSettings& early) noexcept;

    const ApplicationDescription& description_;
    int argc_;
    wchar_t** wide_;
    char** narrow_;
    // 진입 방식이다(RunEmbeddedApplication). configure가 바꿀 수 없다.
    bool embedded_;
    ApplicationSettings settings_;
    bool crashInstalled_ = false;
    bool terminationHandlerInstalled_ = false;
    char identity_[detail::BuildIdentityBytes] = {};
    SystemAllocator allocator_;
    // 범용 할당자(DefaultAllocatorKind::Binned)다. 설정을 적용한 뒤에 만든다(없으면 allocator_가
    // 범용 할당자다).
    alignas(BinnedAllocator) unsigned char binnedStorage_[sizeof(BinnedAllocator)] = {};
    BinnedAllocator* binned_ = nullptr;
    ApplicationContext context_;
    // 도구 로그(ToolLogging::Stderr)다. 시작한 경우에만 만든다.
    alignas(LogSystem) unsigned char logStorage_[sizeof(LogSystem)] = {};
    alignas(StderrLogSink) unsigned char sinkStorage_[sizeof(StderrLogSink)] = {};
    alignas(LogInstallScope) unsigned char scopeStorage_[sizeof(LogInstallScope)] = {};
    LogSystem* toolLog_ = nullptr;
    StderrLogSink* toolSink_ = nullptr;
    LogInstallScope* toolScope_ = nullptr;
    // 프로세스 모듈 로더다(범용 할당자로 만든다). 범용 할당자보다 먼저 파괴한다(단계 역순).
    UniquePtr<ModuleLoader> modules_;
};

platform::CrashReportingOptions
ApplicationRunner::MakeCrashOptions(const ApplicationSettings& settings) const noexcept {
    platform::CrashReportingOptions options;
    options.programName = description_.name;
    options.outputDirectory = settings.crashDirectory;
    options.identity = identity_;
    options.helperExecutable = settings.crashHelperPath;
    options.embedded = embedded_;
    options.reportWhenDebuggerAttached = settings.reportWhenDebuggerAttached;
    options.captureHeapCorruption = settings.captureHeapCorruption;
    options.dumpDetail = settings.dumpDetail;
    options.helperDumpDeadlineMilliseconds = settings.helperDumpDeadlineMilliseconds;
    options.callbackBudgetMilliseconds =
        OrDefault(settings.crashCallbackBudgetMilliseconds,
                  platform::CrashReportingOptions::DefaultCallbackBudgetMilliseconds);
    options.captureTimeoutMilliseconds =
        OrDefault(settings.crashCaptureTimeoutMilliseconds,
                  platform::CrashReportingOptions::DefaultCaptureTimeoutMilliseconds);
    options.inProcessDumpTimeoutMilliseconds =
        OrDefault(settings.inProcessDumpTimeoutMilliseconds,
                  platform::CrashReportingOptions::DefaultInProcessDumpTimeoutMilliseconds);
    options.concurrentCrashGraceMilliseconds =
        OrDefault(settings.concurrentCrashGraceMilliseconds,
                  platform::CrashReportingOptions::DefaultConcurrentCrashGraceMilliseconds);
    // 도우미는 설정을 다 읽은 뒤 띄운다(설정이 수집을 끄면 띄우지 않는다). 내장 진입은 도우미가
    // 없다.
    options.helper = settings.crashReporting == CrashReportingMode::Full && !embedded_
                         ? platform::CrashHelperPolicy::Deferred
                         : platform::CrashHelperPolicy::InProcess;
    options.argumentCount = argc_ > 0 ? static_cast<uint32>(argc_ - 1) : 0u;
    return options;
}

// 크래시 수집기를 다시 설치해야 하는 설정(폴더 제외)이 같은가다. 내장 진입에서 Full과 InProcess는
// 같은 내장 보고다.
bool ApplicationRunner::SameCrashInstallation(const ApplicationSettings& left,
                                              const ApplicationSettings& right) const noexcept {
    const bool sameMode = left.crashReporting == right.crashReporting ||
                          (embedded_ && left.crashReporting != CrashReportingMode::Off &&
                           right.crashReporting != CrashReportingMode::Off);
    return sameMode && left.dumpDetail == right.dumpDetail &&
           left.helperDumpDeadlineMilliseconds == right.helperDumpDeadlineMilliseconds &&
           left.crashCallbackBudgetMilliseconds == right.crashCallbackBudgetMilliseconds &&
           left.crashCaptureTimeoutMilliseconds == right.crashCaptureTimeoutMilliseconds &&
           left.inProcessDumpTimeoutMilliseconds == right.inProcessDumpTimeoutMilliseconds &&
           left.concurrentCrashGraceMilliseconds == right.concurrentCrashGraceMilliseconds &&
           left.reportWhenDebuggerAttached == right.reportWhenDebuggerAttached &&
           left.captureHeapCorruption == right.captureHeapCorruption &&
           SameText(left.crashHelperPath, right.crashHelperPath);
}

bool ApplicationRunner::InstallCrashReporting(const ApplicationSettings& settings) noexcept {
    if (settings.crashReporting == CrashReportingMode::Off) {
        return true;
    }

    crashInstalled_ = platform::InstallCrashReporting(MakeCrashOptions(settings)) ==
                      platform::CrashReportingStatus::Installed;
    return crashInstalled_;
}

// 수집 방식·예산이 바뀌었다. 새 설정을 떼기 전에 검사하고(잘못되면 지금 설치를 그대로 둔다), 떼고
// 다시 건다(그 사이 아주 짧게 보호가 없다). 다시 걸지 못하면 이전 설정과 이전 폴더로 되돌려 건다
// (새 폴더 때문에 실패했을 수 있으므로 폴더는 ApplyCrashSettings의 SetCrashOutputDirectory 분기가
// 따로 시도한다). 새 설정으로 설치했으면 true다(폴더도 그때 적용됐다).
bool ApplicationRunner::ReplaceCrashReporting(const ApplicationSettings& early) noexcept {
    if (settings_.crashReporting != CrashReportingMode::Off &&
        !platform::ValidateCrashReportingOptions(MakeCrashOptions(settings_))) {
        Log::Warning(LogLaunch,
                     "The crash reporting settings are invalid; keeping the previous ones");
        CopyCrashInstallation(early, settings_);
        return false;
    }

    if (crashInstalled_) {
        platform::RemoveCrashReporting();
        crashInstalled_ = false;
    }

    if (InstallCrashReporting(settings_)) {
        return true;
    }

    Log::Warning(LogLaunch, "The crash reporting settings could not be installed; restoring the "
                            "previous ones");
    const char* requestedDirectory = settings_.crashDirectory;
    CopyCrashInstallation(early, settings_);
    settings_.crashDirectory = early.crashDirectory;
    if (!InstallCrashReporting(settings_)) {
        Log::Warning(LogLaunch, "Crash reporting could not be restored; continuing without crash "
                                "reports");
    }

    settings_.crashDirectory = requestedDirectory;
    return false;
}

void ApplicationRunner::ApplyCrashSettings(const ApplicationSettings& early) noexcept {
    const bool reinstalled =
        !SameCrashInstallation(early, settings_) && ReplaceCrashReporting(early);
    if (crashInstalled_ && !reinstalled &&
        !SameText(early.crashDirectory, settings_.crashDirectory)) {
        const char* directory =
            settings_.crashDirectory != nullptr ? settings_.crashDirectory : DefaultCrashDirectory;
        if (!platform::SetCrashOutputDirectory(directory)) {
            Log::Warning(LogLaunch,
                         "The crash directory {Directory} is invalid; keeping the previous one",
                         StringView(directory));
            settings_.crashDirectory = early.crashDirectory;
        }
    }

    if (crashInstalled_ && settings_.crashReporting == CrashReportingMode::Full &&
        platform::StartCrashHelper() == platform::CrashHelperStart::Failed) {
        Log::Warning(LogLaunch,
                     "The crash dump helper could not start; crashes will be written in process");
    }
}

bool ApplicationRunner::StartCommandLine(ApplicationRunner& runner) noexcept {
    const Result<void> captured = runner.wide_ != nullptr
                                      ? ProcessCommandLine::Capture(runner.argc_, runner.wide_)
                                      : ProcessCommandLine::Capture(runner.argc_, runner.narrow_);
    if (!captured) {
        char line[DiagnosticLineBytes];
        FixedStringBuilder text{Span<char>(line)};
        (void)text.TryAppendFormat("Rocks launch: the command line arguments could not be "
                                   "captured ({})",
                                   StringView(ErrorCodeName(captured.Error())));
        if (captured.Error() == ErrorCode::CapacityExceeded) {
            (void)text.TryAppendFormat("; at most {} arguments and {} bytes",
                                       ProcessCommandLine::MaxArguments,
                                       ProcessCommandLine::MaxTotalBytes);
        }

        WriteLine(line);
        return false;
    }

    return true;
}

bool ApplicationRunner::StartSettings(ApplicationRunner& runner) noexcept {
    const ApplicationSettings early = runner.settings_;
    if (runner.description_.configure != nullptr) {
        runner.description_.configure(ProcessCommandLine::Get().Arguments(), runner.settings_);
    }

    if (SuppressesDialogs(runner.description_.kind, runner.settings_) &&
        !SuppressesDialogs(runner.description_.kind, early)) {
        platform::SuppressSystemErrorDialogs();
    }

    runner.ApplyCrashSettings(early);
    return true;
}

bool ApplicationRunner::StartTerminationRequests(ApplicationRunner& runner) noexcept {
    // 내장 진입은 호스트의 Ctrl-C·SIGINT·SIGTERM 처리를 그대로 둔다.
    if (!runner.settings_.handleTerminationRequests || runner.embedded_) {
        return true;
    }

    runner.terminationHandlerInstalled_ =
        platform::InstallTerminationRequestHandler(&OnTerminationRequest);
    if (!runner.terminationHandlerInstalled_) {
        Log::Warning(LogLaunch, "Termination requests (Ctrl-C) could not be handled; they end the "
                                "process immediately");
    }

    return runner.terminationHandlerInstalled_;
}

void ApplicationRunner::StopTerminationRequests(ApplicationRunner& runner) noexcept {
    if (runner.terminationHandlerInstalled_) {
        platform::RemoveTerminationRequestHandler();
        runner.terminationHandlerInstalled_ = false;
    }
}

bool ApplicationRunner::StartDefaultAllocator(ApplicationRunner& runner) noexcept {
    if (runner.settings_.defaultAllocator == DefaultAllocatorKind::Binned) {
        // 상위는 Launch의 SystemAllocator다(큰 블록과 예약하지 못했을 때의 모든 요청이 그리로
        // 간다).
        runner.binned_ = ::new (runner.binnedStorage_) BinnedAllocator(runner.allocator_);
    }

    return true;
}

void ApplicationRunner::StopDefaultAllocator(ApplicationRunner& runner) noexcept {
    if (runner.binned_ == nullptr) {
        return;
    }

    // 정적 소멸 없이 끝내는 경로(RequestExitWithoutStaticTeardown)는 남은 할당을 검사하지 않는다:
    // 멈춘 스레드가 할당을 쥔 채일 수 있다. SystemAllocator를 파괴하지 않고 _Exit하는 것과 같다.
    // 내장 진입은 그 요청을 무시하므로 파괴한다.
    if (runner.context_.exitWithoutStaticTeardown_ && !runner.embedded_) {
        return;
    }

    // 본문이 할당을 남겼으면 여기서 Fatal이다(BinnedAllocator의 계약).
    runner.binned_->~BinnedAllocator();
    runner.binned_ = nullptr;
}

bool ApplicationRunner::StartToolLog(ApplicationRunner& runner) noexcept {
    if (runner.settings_.toolLogging != ToolLogging::Stderr) {
        return true;
    }

    LogSystem* logs =
        ::new (runner.logStorage_) LogSystem(runner.allocator_, LogSystemConfig::ForTool());
    StderrLogSink* sink = ::new (runner.sinkStorage_) StderrLogSink();
    if (!logs->AddSink(*sink) || !logs->Start()) {
        // 로그 없이 계속한다(Warning 이상은 비상 경로로 stderr에 간다, D22).
        logs->~LogSystem();
        sink->~StderrLogSink();
        WriteLine("Rocks launch: the tool log could not start; continuing without it");
        return false;
    }

    runner.toolLog_ = logs;
    runner.toolSink_ = sink;
    runner.toolScope_ = ::new (runner.scopeStorage_) LogInstallScope(*logs);
    return true;
}

void ApplicationRunner::StopToolLog(ApplicationRunner& runner) noexcept {
    if (runner.toolLog_ == nullptr) {
        return;
    }

    runner.toolScope_->~LogInstallScope();
    runner.toolLog_->Stop();
    runner.toolLog_->~LogSystem();
    runner.toolSink_->~StderrLogSink();
    runner.toolScope_ = nullptr;
    runner.toolLog_ = nullptr;
    runner.toolSink_ = nullptr;
}

bool ApplicationRunner::StartModules(ApplicationRunner& runner) noexcept {
    // 이 실행 파일의 모듈 표로 로더를 만들고 부트 모듈(EarliestPossible)을 시작한다
    // (Docs/Design/ModuleLoader.ko.md 3.11절). 실패는 여기서 한 번 기록한다(로더는 기록하지
    // 않는다).
    auto created = ModuleLoader::TryCreate(runner.DefaultAllocatorObject(), GeneratedModuleTable());
    if (!created) {
        Log::Error(LogLaunch, "The module loader could not be created: {Error}",
                   StringView(ErrorCodeName(created.Error())));
        return false;
    }

    runner.modules_ = Move(created.Value());
    const ModuleLoadReport started = runner.modules_->StartPhase(LoadingPhase::EarliestPossible);
    if (!started.IsOk()) {
        Log::Error(LogLaunch, "Boot module {Module} failed to start ({Code}) at {Stage}: {Message}",
                   StringView(started.failed), StringView(ModuleLoadCodeName(started.code)),
                   StringView(started.stage), StringView(started.message));
        runner.modules_->ShutdownGroup(ModuleGroup::Process);
        runner.modules_.Reset();
        return false;
    }

    return true;
}

void ApplicationRunner::StopModules(ApplicationRunner& runner) noexcept {
    if (!runner.modules_) {
        return;
    }

    // 정적 소멸 없이 끝내는 경로는 로더를 내리지도 파괴하지도 않는다: 멈춘 출력 스레드를 포기한
    // 본문이 남긴 설정 문맥이 모듈 lease를 쥐고 있다(범용 할당자와 같은 규칙). 내장 진입은 그
    // 요청을 무시하므로 내린다.
    if (runner.context_.exitWithoutStaticTeardown_ && !runner.embedded_) {
        alignas(UniquePtr<ModuleLoader>) static unsigned char
            abandoned[sizeof(UniquePtr<ModuleLoader>)];
        ::new (abandoned) UniquePtr<ModuleLoader>(Move(runner.modules_));
        return;
    }

    runner.modules_->ShutdownGroup(ModuleGroup::Process);
    // 남은 lease는 모듈 코드를 가리키는 등록이 남은 것이다. 파괴의 Fatal(마지막 안전장치) 전에
    // 원인을 남긴다.
    StringView holders[MaxLeaseReportNames];
    const usize leftover = runner.modules_->ModulesWithLeases(Span<StringView>(holders));
    for (usize index = 0; index < leftover && index < MaxLeaseReportNames; ++index) {
        Log::Error(LogLaunch,
                   "Module {Module} still has code leases after shutdown; a registration that "
                   "points at its code was not released",
                   holders[index]);
    }

    if (leftover > MaxLeaseReportNames) {
        Log::Error(LogLaunch,
                   "{Count} more modules still have code leases after shutdown (names not listed)",
                   leftover - MaxLeaseReportNames);
    }

    runner.modules_.Reset();
}

int ApplicationRunner::RunBody(ApplicationRunner& runner) noexcept {
    ApplicationContext& context = runner.context_;
    context.allocator_ = &runner.allocator_;
    context.defaultAllocator_ = &runner.DefaultAllocatorObject();
    context.modules_ = runner.modules_.Get();

    context.toolLog_ = runner.toolLog_;
    context.settings_ = &runner.settings_;
    const int exitCode = runner.description_.run(context);
    context.toolLog_ = nullptr;
    context.modules_ = nullptr;
    return exitCode;
}

ROCKS_NOINLINE int ApplicationRunner::GuardedStages(void* context) noexcept {
    ApplicationRunner& runner = *static_cast<ApplicationRunner*>(context);
    const detail::LaunchStage<ApplicationRunner> stages[StageCount] = {
        {"Launch.CommandLine", &StartCommandLine, nullptr, detail::StageFailure::Abort,
         runner.description_.commandLineFailureExitCode},
        {"Launch.Settings", &StartSettings, nullptr, detail::StageFailure::Continue, 0},
        {"Launch.TerminationRequests", &StartTerminationRequests, &StopTerminationRequests,
         detail::StageFailure::Continue, 0},
        {"Launch.DefaultAllocator", &StartDefaultAllocator, &StopDefaultAllocator,
         detail::StageFailure::Continue, 0},
        {"Launch.ToolLog", &StartToolLog, &StopToolLog, detail::StageFailure::Continue, 0},
        {"Launch.Modules", &StartModules, &StopModules, detail::StageFailure::Abort,
         ModuleStartupFailureExitCode},
    };
    return detail::RunLaunchStages(stages, runner, &RunBody);
}

int ApplicationRunner::Run() noexcept {
    if (description_.name == nullptr || description_.run == nullptr) {
        platform::Fatal(
            "Rocks launch: the application description needs a name and a run function");
    }

    // 1) 이른 크래시 수집: 명령줄·설정보다 먼저 건다(설명의 기본 설정으로).
    if (SuppressesDialogs(description_.kind, settings_)) {
        platform::SuppressSystemErrorDialogs();
    }

    (void)detail::FormatBuildIdentity(identity_, sizeof(identity_));
    if (!InstallCrashReporting(settings_)) {
        // 수집 없이 계속한다(설계 8.1절 1단계).
        WriteLine("Rocks launch: crash reporting could not be installed; continuing without crash "
                  "reports");
    }

    // 2) 보호 프레임 안에서 단계와 본문을 돈다.
    const int exitCode = platform::RunWithCrashGuard(&GuardedStages, this);
    // 3) 보호 프레임 밖에서 수집기를 뗀다(도우미에 정상 종료를 먼저 알린다).
    if (crashInstalled_) {
        platform::RemoveCrashReporting();
        crashInstalled_ = false;
    }

    if (context_.exitWithoutStaticTeardown_ && !embedded_) {
        _Exit(exitCode);
    }

    return exitCode;
}

namespace {

// 독립 실행 진입은 도우미 모드 분기가 다른 무엇보다 먼저다(Launch 상태·할당기·로그를 만들지
// 않는다). 내장 진입은 호스트의 인자를 도우미 표지로 해석하지 않는다.
template <typename Character>
int RunEntry(const ApplicationDescription& description, int argc, Character** argv,
             bool embedded) noexcept {
    int helperExit = 0;
    if (!embedded && platform::RunCrashHelperIfRequested(argc, argv, helperExit)) {
        return helperExit;
    }

    // 상태는 크고(도구 로그 저장소) 프로세스에 하나이므로 정적 저장소에 둔다.
    alignas(ApplicationRunner) static unsigned char storage[sizeof(ApplicationRunner)];
    ApplicationRunner* runner = nullptr;
    if constexpr (IsSameV<Character, wchar_t>) {
        runner = ::new (storage) ApplicationRunner(description, argc, argv, nullptr, embedded);
    } else {
        runner = ::new (storage) ApplicationRunner(description, argc, nullptr, argv, embedded);
    }

    const int exitCode = runner->Run();
    runner->~ApplicationRunner();
    return exitCode;
}

} // namespace

int RunApplication(const ApplicationDescription& description, int argc, wchar_t** argv) noexcept {
    return RunEntry(description, argc, argv, false);
}

int RunApplication(const ApplicationDescription& description, int argc, char** argv) noexcept {
    return RunEntry(description, argc, argv, false);
}

int RunEmbeddedApplication(const ApplicationDescription& description, int argc,
                           wchar_t** argv) noexcept {
    return RunEntry(description, argc, argv, true);
}

int RunEmbeddedApplication(const ApplicationDescription& description, int argc,
                           char** argv) noexcept {
    return RunEntry(description, argc, argv, true);
}

int RunWindowedApplication(const ApplicationDescription& description) noexcept {
    int argc = 0;
    wchar_t** argv = nullptr;
    if (!platform::GetWindowedEntryArguments(argc, argv)) {
        WriteLine("Rocks launch: the windowed entry arguments are unavailable");
        return description.commandLineFailureExitCode;
    }

    return RunApplication(description, argc, argv);
}

void RequestTermination() noexcept {
    platform::AtomicStore<platform::MemoryOrder::Release>(&terminationRequested, 1u);
}

bool IsTerminationRequested() noexcept {
    return platform::AtomicLoad<platform::MemoryOrder::Acquire>(&terminationRequested) != 0;
}

namespace detail {

void ClearTerminationRequest() noexcept {
    platform::AtomicStore<platform::MemoryOrder::Release>(&terminationRequested, 0u);
}

} // namespace detail

Span<const StringView> ApplicationContext::Arguments() const noexcept {
    return ProcessCommandLine::Get().Arguments();
}

StringView ApplicationContext::ExecutablePath() const noexcept {
    return ProcessCommandLine::Get().ExecutablePath();
}

Allocator& ApplicationContext::DefaultAllocator() noexcept {
    return *defaultAllocator_;
}

Allocator& ApplicationContext::SystemAllocator() noexcept {
    return *allocator_;
}

ModuleLoader& ApplicationContext::Modules() noexcept {
    return *modules_;
}

LogSystem* ApplicationContext::ToolLog() noexcept {
    return toolLog_;
}

const ApplicationSettings& ApplicationContext::Settings() const noexcept {
    return *settings_;
}

void ApplicationContext::SetStage(platform::RunPhase phase, const char* stageName) noexcept {
    platform::SetRunPhase(phase, stageName);
}

void ApplicationContext::RequestExitWithoutStaticTeardown() noexcept {
    exitWithoutStaticTeardown_ = true;
}

} // namespace rocks::launch
