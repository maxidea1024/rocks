#include <TestSupport/HostFailure.h>
#include <Core/Assert.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/SystemAllocator.h>

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

} // namespace rocks::test
