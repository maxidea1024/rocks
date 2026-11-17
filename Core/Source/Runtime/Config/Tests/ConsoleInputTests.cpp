#include "ConfigTestSupport.h"

#include <Core/Text/StringBuilder.h>

// 설정 CF2 콘솔 입력·명령·sync 지점·콜백·설정 snapshot
// 시험이다(Docs/Design/ConfigSystem.ko.md 5.2·5.3절): 입력 문법(`<이름>`·`<이름> ?`·`<이름>
// <값>`·`<명령> <인자…>`), 자동완성, 명령이 sync 지점에서만 실행되는지, 콜백이 Sync 끝에 구독
// 순서대로 불리는지(바뀐 대상만, 콜백 안 Sync는 Busy), snapshot 세대·재사용·불변.
using namespace rocks;
using namespace rocks::config;
using namespace rocks::test::config;

namespace {

struct Transcript {
    explicit Transcript(Allocator& allocator) noexcept : text(allocator) {
    }

    static Result<void> Write(void* context, StringView text) noexcept {
        return static_cast<Transcript*>(context)->text.TryAppend(text);
    }

    ConsoleOutput Output() noexcept {
        return ConsoleOutput{&Transcript::Write, this};
    }

    bool Contains(StringView needle) const noexcept {
        const bool found = text.View().Find(needle, StringComparison::CaseSensitive) != IndexNone;
        if (!found) {
            printf("transcript lacks '%.*s':\n%.*s", static_cast<int>(needle.ByteCount()),
                   needle.Data(), static_cast<int>(text.ByteCount()), text.Data());
        }

        return found;
    }

    String text;
};

struct ReloadLog {
    int32 calls = 0;
    int32 count = 0;
    bool targetAll = false;
};

Result<void> Reload(void* context, const void* arguments, ConsoleOutput& output) noexcept {
    auto& log = *static_cast<ReloadLog*>(context);
    const auto& args = *static_cast<const ReloadArgs*>(arguments);
    ++log.calls;
    log.count = args.count;
    log.targetAll = args.target.View() == "all";
    return output.write(output.context, "reloaded\n");
}

Result<void> Flush(void* context, const void* arguments, ConsoleOutput&) noexcept {
    ++*static_cast<int32*>(context);
    return arguments == nullptr ? Result<void>::Success()
                                : Result<void>::Failure(ErrorCode::InvalidArgument);
}

} // namespace

// `<이름>`은 값과 출처, `<이름> ?`는 도움말, `<이름> <값>`은 콘솔 층 쓰기를 큐에 넣는다(다음 Sync에
// 적용). 거부한 값과 모르는 이름은 출력으로 알린다.
TEST(Config_ConsoleInputGrammar) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    Transcript transcript(fixture.system);
    auto shown = context.ExecuteConsoleLine("render.graph.immediate", transcript.Output());
    REQUIRE(shown);
    CHECK(shown.Value() == ConsoleLineResult::Shown &&
          transcript.Contains("render.graph.immediate = false"));
    auto help = context.ExecuteConsoleLine("  render.graph.immediate ?  ", transcript.Output());
    CHECK(help && help.Value() == ConsoleLineResult::Help);

    auto queued = context.ExecuteConsoleLine("render.graph.immediate true", transcript.Output());
    CHECK(queued && queued.Value() == ConsoleLineResult::Queued);
    CHECK(!fixture.Graph().immediate);
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().immediate);
    transcript.text.Clear();
    REQUIRE(context.ExecuteConsoleLine("render.graph.immediate", transcript.Output()));
    CHECK(transcript.Contains("render.graph.immediate = true") &&
          transcript.Contains("Console console"));

    transcript.text.Clear();
    auto rejected = context.ExecuteConsoleLine("render.graph.immediate maybe", transcript.Output());
    CHECK(rejected && rejected.Value() == ConsoleLineResult::Rejected &&
          transcript.Contains("error FieldValue"));
    auto unknown = context.ExecuteConsoleLine("render.nothing 1", transcript.Output());
    CHECK(unknown && unknown.Value() == ConsoleLineResult::Unknown &&
          transcript.Contains("unknown console name"));
    auto empty = context.ExecuteConsoleLine("   ", transcript.Output());
    CHECK(!empty && empty.Error() == ErrorCode::InvalidArgument);

    // 독립 설정도 같은 문법이다. 출처 스택은 콘솔 층이다.
    ConsoleSettingDefinition pool;
    pool.name = "stream.pool_mb";
    pool.help = "pool size";
    pool.type = ConsoleValueType::Int32;
    pool.defaultValue = "256";
    REQUIRE(context.RegisterConsoleSetting(pool));
    REQUIRE(context.ExecuteConsoleLine("stream.pool_mb 300", transcript.Output()));
    REQUIRE(context.Sync());
    transcript.text.Clear();
    REQUIRE(context.ExecuteConsoleLine("stream.pool_mb", transcript.Output()));
    CHECK(transcript.Contains("stream.pool_mb = 300") && transcript.Contains("Console console"));

    // 자동완성은 이름 접두로 찾고 이름 순서다.
    Array<StringView> names(fixture.system);
    REQUIRE(context.CompleteConsoleName("render.graph.", names));
    REQUIRE(names.Size() == 3);
    CHECK(names[0] == "render.graph.debug_draw" && names[1] == "render.graph.immediate" &&
          names[2] == "render.graph.quality");
}

// 명령은 `<명령> <경로>=<값>…` 인자를 인자 struct에 대입해 큐에 넣고, 다음 Sync에서 한 번 실행한다.
// 인자 오류는 실행하지 않는다. 인자 없는 명령에 인자를 주면 거부한다.
TEST(Config_ConsoleCommandsRunAtSync) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    ReloadLog log;
    ConsoleCommandDefinition reload{"render.reload", "reloads render state", &Reload, &log};
    REQUIRE(context.RegisterConsoleCommand<ReloadArgs>(reload));
    int32 flushed = 0;
    ConsoleCommandDefinition flush{"log.flush", "flushes logs", &Flush, &flushed};
    REQUIRE(context.RegisterConsoleCommand(flush));
    CHECK(context.RegisterConsoleCommand(flush).Error() == ErrorCode::AlreadyExists);
    ConsoleCommandDefinition clash{"render.graph.immediate", "x", &Flush, &flushed};
    CHECK(context.RegisterConsoleCommand(clash).Error() == ErrorCode::AlreadyExists);

    Transcript transcript(fixture.system);
    auto queued =
        context.ExecuteConsoleLine("render.reload count=3 target=all", transcript.Output());
    CHECK(queued && queued.Value() == ConsoleLineResult::CommandQueued);
    CHECK(log.calls == 0 && context.HasPendingChanges());
    REQUIRE(context.Sync());
    CHECK(log.calls == 1 && log.count == 3 && log.targetAll && transcript.Contains("reloaded"));
    REQUIRE(context.Sync());
    CHECK(log.calls == 1);

    auto bogus = context.ExecuteConsoleLine("render.reload bogus=1", transcript.Output());
    CHECK(bogus && bogus.Value() == ConsoleLineResult::Rejected &&
          transcript.Contains("UnknownProperty"));
    auto malformed = context.ExecuteConsoleLine("render.reload count", transcript.Output());
    CHECK(malformed && malformed.Value() == ConsoleLineResult::Rejected);
    auto extra = context.ExecuteConsoleLine("log.flush now=1", transcript.Output());
    CHECK(extra && extra.Value() == ConsoleLineResult::Rejected);
    REQUIRE(context.ExecuteConsoleLine("log.flush", transcript.Output()));
    REQUIRE(context.ExecuteConsoleLine("render.reload", transcript.Output()));
    REQUIRE(context.Sync());
    CHECK(flushed == 1 && log.calls == 2 && log.count == 1 && !log.targetAll);
}

namespace {

struct ChangeLog {
    explicit ChangeLog(Allocator& allocator) noexcept : names(allocator) {
    }

    String names;
    ConfigContext* context = nullptr;
    ErrorCode nestedSync = ErrorCode::None;
};

void RecordChange(void* context, const ConfigChange& change) noexcept {
    auto& log = *static_cast<ChangeLog*>(context);
    static_cast<void>(log.names.TryAppend(change.name));
    static_cast<void>(log.names.TryAppend(";"));
    if (log.context != nullptr) {
        auto nested = log.context->Sync();
        log.nestedSync = nested ? ErrorCode::None : nested.Error();
    }
}

} // namespace

// 콜백은 Sync 끝에 값이 바뀐 대상에만, 구독 순서대로 불린다(Sync 전에는 불리지 않는다). 콜백 안의
// Sync는 Busy다. 구독을 끝내면 더 부르지 않는다.
TEST(Config_ChangeCallbacksRunAtSyncInOrder) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    ChangeLog log(fixture.system);
    auto gameplay = context.SubscribeSettings<GameplaySettings>(&RecordChange, &log);
    REQUIRE(gameplay);
    auto immediate = context.Subscribe("render.graph.immediate", &RecordChange, &log);
    REQUIRE(immediate);
    auto graph = context.SubscribeSettings<GraphSettings>(&RecordChange, &log);
    REQUIRE(graph);
    CHECK(context.Subscribe("render.nothing", &RecordChange, &log).Error() == ErrorCode::NotFound);

    REQUIRE(
        context.AddAssignment(ConfigLayer::Console, "console", "render.graph.immediate", "true"));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli", "Game.Gameplay.lives", "9"));
    CHECK(log.names.IsEmpty());
    REQUIRE(context.Sync());
    CHECK(log.names.View() == "Game.Gameplay;render.graph.immediate;Render.RenderGraph;");

    // 값이 같으면(같은 값을 다시 쓴다) 콜백이 없다. 바뀐 대상만 부른다.
    log.names.Clear();
    REQUIRE(context.AddAssignment(ConfigLayer::Console, "again", "render.graph.immediate", "true"));
    REQUIRE(
        context.AddAssignment(ConfigLayer::Session, "cli2", "Render.RenderGraph.maxPasses", "3"));
    REQUIRE(context.Sync());
    CHECK(log.names.View() == "Render.RenderGraph;");

    // 콜백 안의 Sync는 Busy다.
    log.names.Clear();
    log.context = &context;
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli3", "Game.Gameplay.lives", "8"));
    REQUIRE(context.Sync());
    CHECK(log.nestedSync == ErrorCode::Busy);
    log.context = nullptr;

    REQUIRE(context.Unsubscribe(gameplay.Value()));
    CHECK(context.Unsubscribe(gameplay.Value()).Error() == ErrorCode::NotFound);
    log.names.Clear();
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli4", "Game.Gameplay.lives", "7"));
    REQUIRE(context.Sync());
    CHECK(log.names.IsEmpty());
}

// 설정 snapshot은 sync 지점에서 RenderRead 값(콘솔 이름, 이름 없는 필드는
// `<Branch>.<Section>.<필드>`)으로 만든다. RenderRead 값이 바뀐 Sync만 세대가 오르고, 아니면 같은
// snapshot을 돌려준다. 프레임이 쥔 이전 snapshot은 바뀌지 않는다.
TEST(Config_SnapshotGenerationsAndReuse) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    SharedPtr<ConfigSnapshot> first = context.Snapshot();
    REQUIRE(first.Get() != nullptr);
    CHECK(first->Generation() == 1);
    REQUIRE(first->Count() == 3);
    CHECK(first->NameAt(0) == "Render.RenderGraph.lodBias" &&
          first->NameAt(1) == "render.graph.immediate" &&
          first->NameAt(2) == "render.graph.quality");
    CHECK(first->Find("render.graph.immediate") != nullptr &&
          !first->Find("render.graph.immediate")->boolean);
    CHECK(first->Find("Render.RenderGraph.lodBias")->float32 == 1.0f);
    CHECK(first->Find("render.graph.missing") == nullptr);

    // RenderRead가 아닌 값의 변경은 snapshot을 재사용한다.
    REQUIRE(
        context.AddAssignment(ConfigLayer::Session, "cli", "Render.RenderGraph.maxPasses", "3"));
    REQUIRE(context.Sync());
    CHECK(context.Snapshot().Get() == first.Get());

    // RenderRead 값의 변경은 새 세대다. 이전 snapshot은 그대로다. Sync 전에는 snapshot이 바뀌지
    // 않는다.
    REQUIRE(
        context.AddAssignment(ConfigLayer::Console, "console", "render.graph.immediate", "true"));
    REQUIRE(
        context.AddAssignment(ConfigLayer::Session, "cli", "Render.RenderGraph.lodBias", "0.5"));
    CHECK(context.Snapshot().Get() == first.Get());
    REQUIRE(context.Sync());
    SharedPtr<ConfigSnapshot> second = context.Snapshot();
    REQUIRE(second.Get() != nullptr && second.Get() != first.Get());
    CHECK(second->Generation() == 2);
    CHECK(second->Find("render.graph.immediate")->boolean &&
          second->Find("Render.RenderGraph.lodBias")->float32 == 0.5f);
    CHECK(!first->Find("render.graph.immediate")->boolean);

    // 새 RenderRead 독립 설정 등록은 다음 Sync에 snapshot에 들어간다.
    ConsoleSettingDefinition pool;
    pool.name = "stream.pool_mb";
    pool.help = "pool size";
    pool.type = ConsoleValueType::Int32;
    pool.defaultValue = "256";
    pool.flags = static_cast<uint32>(ConsoleSettingFlags::RenderRead);
    REQUIRE(context.RegisterConsoleSetting(pool));
    REQUIRE(context.Sync());
    SharedPtr<ConfigSnapshot> third = context.Snapshot();
    CHECK(third->Generation() == 3 && third->Find("stream.pool_mb")->signedInteger == 256);
    REQUIRE(context.Sync());
    CHECK(context.Snapshot().Get() == third.Get());
}
