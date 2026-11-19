#include <Config/Tests/ConfigTestSupport.h>
#include <TraceAnalysis/TraceModel.h>
#include <TraceCapture/TraceCounter.h>
#include <TraceCapture/TraceCpuScope.h>
#include <TraceCapture/TraceMark.h>
#include <TraceCapture/TraceTask.h>
#include <TraceCapture/Tests/TraceCaptureTestSupport.h>
#include <TraceControl/TraceController.h>
#include <TraceControl/TraceSettings.generated.h>

// trace 제어 시험이다(Docs/Design/TraceSystem.ko.md 5·8절 — TR5): 설정 데이터의 채널 묶음과 배열
// 연산, 명령줄 `--trace=`, 콘솔 명령 trace.*. 설정 값은 Sync에서만 바뀌고, 콘솔 명령은 Sync에서
// 실행된다(시간에 기대지 않는다).
using namespace rocks;
using namespace rocks::trace_control;
using rocks::test::config::ConfigFixture;
using rocks::tracetest::MemoryOutput;
using rocks::tracetest::TempTracePath;

namespace {

TraceScopeName ControlScope{"Test.TraceControl"};

// 설정 fixture에 이 모듈의 타입·설정 struct·핸들을 더한다.
void PrepareTraceSettings(ConfigFixture& fixture) noexcept {
    REQUIRE(RegisterTraceControlTypes(fixture.registration));
    REQUIRE(RegisterTraceSettings(fixture.Context()));
}

TraceSessionSettings ManualSettings(Allocator& allocator) noexcept {
    TraceSessionSettings settings;
    settings.allocator = &allocator;
    settings.writerMode = TraceWriterMode::Manual;
    return settings;
}

// 콘솔 출력을 모은다.
// 콘솔 출력을 모으는 sink다(참조 계수).
struct ConsoleText final : config::ConsoleSink {
    explicit ConsoleText(Allocator& allocator) noexcept : text(allocator) {
    }

    Result<void> Write(StringView line) noexcept override {
        return text.TryAppend(line);
    }

    String text;
};

// trace 파일 하나의 Test.TraceControl 구간 수다(읽지 못하거나 스냅샷 여부가 다르면 실패를 기록하고
// 0이다).
usize CountScopes(Allocator& allocator, const TempTracePath& path, bool expectSnapshot) noexcept {
    Array<uint8> bytes(allocator);
    TraceModel model(allocator);
    if (!path.ReadAll(bytes) || !model.Build(bytes.View())) {
        CHECK(false);
        return 0;
    }

    CHECK(model.Summary().snapshot == expectSnapshot);
    usize found = 0;
    for (const TraceModelThread& thread : model.Threads()) {
        for (const TraceModelScope& scope : thread.scopes) {
            found += model.ScopeName(scope.nameId) == StringView("Test.TraceControl") ? 1 : 0;
        }
    }

    return found;
}

} // namespace

// 묶음은 설정 데이터다: 엔진 층의 목록을 프로젝트 층이 배열 연산으로 고친다. 묶음 안의 모르는
// 채널은 실패가 아니라 unknown으로 센다. 명령줄 `--trace=`는 나온 순서대로 적용하고 `-`는 끈다.
TEST(TraceControl_PresetsExpandFromConfigLayers) {
    ConfigFixture fixture;
    PrepareTraceSettings(fixture);
    config::ConfigContext& settings = fixture.Context();
    REQUIRE(settings.AddLayerText(
        config::ConfigLayer::Engine, "Engine.json", "Engine", "<engine>",
        R"({ "TracePresets": { "default": ["cpu", "task", "nosuch.channel"] } })"));
    REQUIRE(settings.AddLayerText(
        config::ConfigLayer::Project, "Engine.json", "Engine", "<project>",
        R"({ "TracePresets": { "default": { "$remove": ["task"], "$append": ["stats"] } } })"));
    REQUIRE(settings.Sync());
    REQUIRE(settings.Get<TracePresetSettings>().defaultChannels.Size() == 3);

    SystemAllocator allocator;
    TraceSession session;
    REQUIRE(session.Start(ManualSettings(allocator)));
    {
        TraceController control(allocator, session, settings);
        auto change = control.SetChannels("default", true);
        REQUIRE(change);
        CHECK(change.Value().applied == 2);
        CHECK(change.Value().unknown == 1);
        CHECK(TraceCpuChannel.IsEnabled());
        CHECK(TraceStatsChannel.IsEnabled());
        CHECK(!TraceTaskChannel.IsEnabled());

        const StringView arguments[] = {"--frames=3", "--trace=-cpu, mark", "--trace=bogus"};
        auto applied = control.ApplyCommandLine(Span<const StringView>(arguments, 3));
        REQUIRE(applied);
        CHECK(applied.Value().applied == 2);
        CHECK(applied.Value().unknown == 1);
        CHECK(!TraceCpuChannel.IsEnabled());
        CHECK(TraceMarkChannel.IsEnabled());
        CHECK(TraceStatsChannel.IsEnabled());
    }

    session.Stop();
}

// 콘솔 명령 trace.enable·trace.file·trace.stop·trace.snapshot·trace.disable은 Sync에서 실행되어
// 세션을 바꾼다. 파일과 스냅샷은 분석기로 읽을 수 있다.
TEST(TraceControl_ConsoleCommandsDriveSession) {
    ConfigFixture fixture;
    PrepareTraceSettings(fixture);
    config::ConfigContext& settings = fixture.Context();
    REQUIRE(settings.Sync());
    SystemAllocator allocator;
    TraceSession session;
    REQUIRE(session.Start(ManualSettings(allocator)));
    TempTracePath file;
    TempTracePath snapshot;
    {
        TraceController control(allocator, session, settings);
        REQUIRE(control.RegisterConsoleCommands());
        auto made = SharedPtr<ConsoleText>::TryMake(allocator, allocator);
        REQUIRE(made);
        SharedPtr<ConsoleText> console = Move(made.Value());
        auto run = [&](StringView line) noexcept {
            auto queued = settings.ExecuteConsoleLine(line, console);
            REQUIRE(queued);
            CHECK(queued.Value() == config::ConsoleLineResult::CommandQueued);
            REQUIRE(settings.Sync());
        };

        run("trace.enable cpu,counter");
        CHECK(TraceCpuChannel.IsEnabled());
        CHECK(TraceCounterChannel.IsEnabled());
        StringBuilder<192> fileCommand(allocator);
        REQUIRE(fileCommand.TryAppendFormat("trace.file {}", StringView(file.path)));
        run(fileCommand.View());
        {
            CpuScope scope(ControlScope);
        }

        REQUIRE(session.Pump());
        run("trace.stop");
        CHECK(control.CloseFile().Error() == ErrorCode::NotFound);
        StringBuilder<192> snapshotCommand(allocator);
        REQUIRE(snapshotCommand.TryAppendFormat("trace.snapshot {}", StringView(snapshot.path)));
        run(snapshotCommand.View());
        run("trace.disable counter");
        CHECK(!TraceCounterChannel.IsEnabled());
        CHECK(
            console->text.View().Contains("2 channel(s) enabled", StringComparison::CaseSensitive));
        CHECK(console->text.View().Contains("Trace file output stopped",
                                            StringComparison::CaseSensitive));
        CHECK(console->text.View().Contains("complete)", StringComparison::CaseSensitive));
    }

    session.Stop();
    CHECK(CountScopes(allocator, file, false) == 1);
    CHECK(CountScopes(allocator, snapshot, true) == 1);
}

// 세션 값(trace.* 핸들)은 설정 층에서 오고 봉인 뒤에 읽는다(부팅 값). 범위 밖(음수·백분율 100
// 초과)은 설정 단계에서 거부한다. 스냅샷 제한 시간만 실행 중에 바뀐다(CVar).
TEST(TraceControl_SessionSettingsFromConfigLayers) {
    ConfigFixture fixture;
    PrepareTraceSettings(fixture);
    config::ConfigContext& settings = fixture.Context();
    CHECK(ReadTraceCaptureSettings(settings).Error() == ErrorCode::InvalidArgument);
    REQUIRE(settings.AddAssignment(config::ConfigLayer::Session, "--set", "trace.pool.max_bytes",
                                   "1048576"));
    REQUIRE(settings.AddAssignment(config::ConfigLayer::Session, "--set", "trace.writer.period_ms",
                                   "5"));
    CHECK(
        !settings.AddAssignment(config::ConfigLayer::Session, "--set", "trace.recent.bytes", "-1"));
    CHECK(!settings.AddAssignment(config::ConfigLayer::Session, "--set", "trace.pool.warn_percent",
                                  "101"));
    REQUIRE(settings.Sync());
    REQUIRE(settings.CompleteStartup());
    auto read = ReadTraceCaptureSettings(settings);
    REQUIRE(read);
    CHECK(read.Value().poolMaxBytes == 1048576 && read.Value().writerPeriodMs == 5);
    CHECK(read.Value().recentBytes == 4 * 1024 * 1024 && read.Value().poolWarningPercent == 80);
    CHECK(read.Value().snapshotLockTimeoutMs == 1000);

    // 봉인 뒤: 부팅 값의 콘솔 쓰기는 거부하고, 스냅샷 제한 시간은 다음 Sync에 바뀐다.
    CHECK(!settings.AddAssignment(config::ConfigLayer::Console, "console", "trace.pool.max_bytes",
                                  "2097152"));
    REQUIRE(settings.AddAssignment(config::ConfigLayer::Console, "console",
                                   "trace.snapshot.lock_timeout_ms", "250"));
    REQUIRE(settings.Sync());
    auto again = ReadTraceCaptureSettings(settings);
    REQUIRE(again);
    CHECK(again.Value().poolMaxBytes == 1048576 && again.Value().snapshotLockTimeoutMs == 250);
}

// 세션 값은 세션 설정으로 옮길 때 음수를 0으로, 너무 큰 값을 그 칸의 최대값으로 본다.
TEST(TraceControl_SessionSettingsFromConfig) {
    SystemAllocator allocator;
    TraceCaptureSettings settings;
    settings.poolMaxBytes = 8 * 1024 * 1024;
    settings.recentBytes = -1;
    settings.poolWarningPercent = 90;
    settings.writerPeriodMs = 5;
    settings.statsIntervalPumps = 7;
    const TraceSessionSettings session = MakeTraceSessionSettings(settings, allocator);
    CHECK(session.allocator == &allocator);
    CHECK(session.poolMaxBytes == 8u * 1024 * 1024);
    CHECK(session.recentBytes == 0);
    CHECK(session.poolWarningPercent == 90);
    CHECK(session.writerPeriodMs == 5);
    CHECK(session.statsIntervalPumps == 7);
}
