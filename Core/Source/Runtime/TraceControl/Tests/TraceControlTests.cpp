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

// 설정 fixture에 이 모듈의 타입과 설정 struct를 더한다.
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
struct ConsoleText {
    explicit ConsoleText(Allocator& allocator) noexcept : text(allocator) {
    }

    static Result<void> Write(void* context, StringView line) noexcept {
        return static_cast<ConsoleText*>(context)->text.TryAppend(line);
    }

    config::ConsoleOutput Output() noexcept {
        return {&ConsoleText::Write, this};
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
        ConsoleText console(allocator);
        auto run = [&](StringView line) noexcept {
            auto queued = settings.ExecuteConsoleLine(line, console.Output());
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
            console.text.View().Contains("2 channel(s) enabled", StringComparison::CaseSensitive));
        CHECK(console.text.View().Contains("Trace file output stopped",
                                           StringComparison::CaseSensitive));
        CHECK(console.text.View().Contains("complete)", StringComparison::CaseSensitive));
    }

    session.Stop();
    CHECK(CountScopes(allocator, file, false) == 1);
    CHECK(CountScopes(allocator, snapshot, true) == 1);
}

// 설정 섹션 Trace의 값은 세션 설정으로 옮길 때 음수를 0으로 본다.
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
