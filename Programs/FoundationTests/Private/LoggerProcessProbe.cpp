#include <TestSupport/HostFailure.h>
#include <Core/Assert.h>
#include <Core/Diagnostics/FileLogSink.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringView.h>
#include <Platform/CrashHandler.h>
#include <Platform/Fatal.h>
#include <Platform/Sync.h>

namespace rocks::test {
bool ConfigureLoggerFailureProbe() noexcept;

namespace {

inline constexpr LogCategory LogProbe{"Tests.LoggerProbe"};
inline constexpr LogCategory LogFatalProbe{"Tests.Fatal"};

// Fatal을 렌더하는 중에 다시 Fatal을 부르는 Formatter다(재귀 차단 확인).
struct RecursiveFatal {};

} // namespace

} // namespace rocks::test

namespace rocks {

template <>
struct Formatter<test::RecursiveFatal> {
    static Result<void> Format(StringBuilderBase&, const test::RecursiveFatal&,
                               StringView) noexcept {
        Log::Fatal(test::LogFatalProbe, "nested fatal");
    }
};

} // namespace rocks

namespace rocks::test {

// 오직 별도 child에서 실행한다. stderr를 쓸 수 없는 상태로 만들어 sink 실패가 통계에 남는지
// 검사한다.
int RunLoggerFailureProbe() noexcept {
    if (!ConfigureLoggerFailureProbe()) {
        return 2;
    }

    SystemAllocator allocator;
    LogSystem logs(allocator, LogSystemConfig::ForTool());
    StderrLogSink sink;
    if (!logs.AddSink(sink) || !logs.Start()) {
        return 3;
    }

    {
        LogInstallScope installed(logs);
        Log::Error(LogProbe, "broken pipe");
    }

    // 기록 write와 묶음 flush가 모두 실패한다(둘 다 sink 실패로 센다).
    return logs.Stats().sinkFailures != 0 ? 0 : 5;
}

// Log::Fatal·AssertFailed 경로를 별도 child에서 실행한다(설계 11절). 대화상자 없이 끝나도록 시험
// 프로세스 준비를 다시 한다. 정상이면 반환하지 않는다(abort).
// - mode 0: Sync LogSystem(StderrLogSink) 설치 → 비상 출력 + sink 출력 → 종료
// - mode 1: Async LogSystem 설치 → 비상 출력 + 큐 전달·flush 대기 → 종료
// - mode 2: Async 설치 상태의 ROCKS_ASSERT 실패
// - mode 3: Fatal을 렌더하는 Formatter 안에서 다시 Fatal(재귀 차단)
int RunLogFatalProbe(uint32 mode) noexcept {
    PrepareFatalTestProcess();
    SystemAllocator allocator;
    LogSystem logs(allocator,
                   mode == 0 ? LogSystemConfig::ForTool() : LogSystemConfig::ForEngineHost());
    StderrLogSink sink;
    if (!logs.AddSink(sink) || !logs.Start()) {
        return 3;
    }

    LogInstallScope installed(logs);
    if (mode == 2) {
        const int value = 1;
        ROCKS_ASSERT(value == 2);
    } else if (mode == 3) {
        Log::Fatal(LogFatalProbe, "outer {Value}", RecursiveFatal{});
    } else {
        Log::Fatal(LogFatalProbe, "fatal probe {Value}", 42);
    }

    return 4;
}

namespace {

inline constexpr LogCategory LogCrashProbe{"Tests.Crash"};

// 멈춘 sink probe(mode 3)의 값이다. flush 제한 시간을 짧게 두어 시험이 빨리 끝난다.
constexpr uint32 StuckSinkMode = 3;
constexpr uint32 StuckFlushTimeoutMilliseconds = 200;
constexpr uint32 StuckEnterTimeoutMilliseconds = 30000;
constexpr uint32 StuckReleaseTimeoutMilliseconds = 60000;

// Write에 들어와 돌아오지 않는 sink다(풀어 줄 일이 없어 프로세스가 끝날 때까지 기다린다).
class StuckSink final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent&) noexcept override {
        entered.Signal();
        (void)release.Wait(StuckReleaseTimeoutMilliseconds);
        return Result<void>::Success();
    }

    Result<void> Flush() noexcept override {
        return Result<void>::Success();
    }

    platform::ManualResetEvent entered;
    platform::ManualResetEvent release;
};

// 끝없는 재귀로 stack을 넘친다. 깊이 검사는 무한 재귀 경고를 피하려는 것이고, 닿지 않는다.
[[gnu::noinline]] uint64 OverflowStack(uint64 depth) noexcept {
    constexpr usize OverflowFrameBytes = 1024;
    volatile uint8 frame[OverflowFrameBytes];
    frame[0] = static_cast<uint8>(depth);
    if (depth == NumericLimits<uint64>::Max()) {
        return frame[0];
    }

    return OverflowStack(depth + 1) + frame[0];
}

} // namespace

// 비정상 종료 직전의 Async 기록이 파일에 남는지 별도 child에서 확인한다(설계 14.1.8, L24·L25).
// directory에 Probe-*.log를 쓴다. 정상이면 반환하지 않는다.
// - mode 0: platform::Fatal을 바로 부른다(종료 hook)
// - mode 1: 크래시 처리기를 설치하고 접근 위반
// - mode 2: 크래시 처리기를 설치하고 stack overflow
// - mode 3: 멈춘 sink(Write가 돌아오지 않음) 안에서 platform::Fatal. flush는 제한 시간 뒤 포기하고
//   프로세스가 끝난다(파일 기록은 없고 stderr에 Fatal 문구가 남는다)
int RunLogCrashProbe(uint32 mode, const char* directory) noexcept {
    // 준비 실패 종료 코드다. 크래시 종료 코드(abort 3, 예외 코드)와 겹치지 않게 따로 둔다.
    constexpr int MissingDirectory = 20;
    constexpr int HandlerFailed = 21;
    constexpr int SetupFailed = 22;
    constexpr int DidNotCrash = 23;
    PrepareFatalTestProcess();
    if (directory == nullptr) {
        return MissingDirectory;
    }

    if (mode != 0 && !platform::InstallCrashHandler()) {
        return HandlerFailed;
    }

    SystemAllocator allocator;
    if (mode == StuckSinkMode) {
        static StuckSink stuck;
        LogSystemConfig stuckConfig = LogSystemConfig::ForEngineHost();
        stuckConfig.fatalFlushTimeoutMilliseconds = StuckFlushTimeoutMilliseconds;
        static LogSystem stuckLogs(allocator, stuckConfig);
        if (!stuckLogs.AddSink(stuck) || !stuckLogs.Start()) {
            return SetupFailed;
        }

        static LogInstallScope stuckInstalled(stuckLogs);
        Log::Info(LogCrashProbe, "record held by a stuck sink");
        if (stuck.entered.Wait(StuckEnterTimeoutMilliseconds) != platform::WaitResult::Signaled) {
            return SetupFailed;
        }

        platform::Fatal("log crash probe: fatal inside a stuck sink");
    }

    FileLogSinkConfig config;
    config.directory = StringView(directory);
    config.hostName = "Probe";
    FileLogSink file(allocator, config);
    LogSystem logs(allocator, LogSystemConfig::ForEngineHost());
    if (!file.Open() || !logs.AddSink(file) || !logs.Start()) {
        return SetupFailed;
    }

    LogInstallScope installed(logs);
    Log::Info(LogCrashProbe, "last record before crash mode {Mode}", mode);
    if (mode == 0) {
        platform::Fatal("log crash probe: platform fatal");
    } else if (mode == 1) {
        int* volatile target = nullptr;
        *target = 1;
    } else {
        (void)OverflowStack(0);
    }

    return DidNotCrash;
}

} // namespace rocks::test
