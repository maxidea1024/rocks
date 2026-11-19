#include "ConfigTestSupport.h"

#include <Config/CVar.h>
#include <Core/Text/StringBuilder.h>

// 콘솔 입력·명령·sync 지점·콜백·설정 snapshot 시험이다(Docs/Design/ConfigSystem.ko.md 5.2·5.3절,
// CVarSystem.ko.md 15절): 입력 문법(`<이름>`·`<이름> ?`·`<이름> <값>`·`<이름> reset`·`<명령>
// <인자…>`), 자동완성, 명령(cvar::Command — 매개변수 인자, BindCommand 대상)이 sync 지점에서만
// 실행되는지, 콜백이 Sync 끝에 구독 순서대로 불리는지(바뀐 대상만, 콜백 안 Sync는 Busy), snapshot
// 세대·재사용·불변.
using namespace rocks;
using namespace rocks::config;
using namespace rocks::test::config;

namespace {

// 콘솔 출력을 모으는 sink다(참조 계수 — 큐에 든 일이 쥔다). text는 sink의 버퍼를 가리킨다.
struct Transcript {
    struct Buffer final : ConsoleSink {
        explicit Buffer(Allocator& allocator) noexcept : text(allocator) {
        }

        Result<void> Write(StringView value) noexcept override {
            return text.TryAppend(value);
        }

        String text;
    };

    explicit Transcript(Allocator& allocator) noexcept
        : buffer(MakeBuffer(allocator)), text(buffer->text) {
    }

    static SharedPtr<Buffer> MakeBuffer(Allocator& allocator) noexcept {
        auto made = SharedPtr<Buffer>::TryMake(allocator, allocator);
        if (!made) {
            platform::Fatal("Transcript buffer allocation failed");
        }

        return Move(made.Value());
    }

    SharedPtr<ConsoleSink> Output() const noexcept {
        return buffer;
    }

    bool Contains(StringView needle) const noexcept {
        const bool found = text.View().Find(needle, StringComparison::CaseSensitive) != IndexNone;
        if (!found) {
            printf("transcript lacks '%.*s':\n%.*s", static_cast<int>(needle.ByteCount()),
                   needle.Data(), static_cast<int>(text.ByteCount()), text.Data());
        }

        return found;
    }

    SharedPtr<Buffer> buffer;
    String& text;
};

struct ReloadLog {
    int32 calls = 0;
    int32 count = 0;
    bool targetAll = false;
};

} // namespace

namespace console_input_test {

Result<void> Reload(cvar::CommandContext& context, int32 count, StringView target) noexcept {
    ReloadLog& log = *context.Target<ReloadLog>();
    ++log.calls;
    log.count = count;
    log.targetAll = target == "all";
    return context.Write("reloaded\n");
}

Result<void> Flush(cvar::CommandContext& context) noexcept {
    ++*context.Target<int32>();
    return Result<void>::Success();
}

constinit ConsoleCommand ReloadCommand =
    cvar::Command<&Reload>("render.reload").Help("reloads render state");

constinit ConsoleCommand FlushCommand = cvar::Command<&Flush>("log.flush").Help("flushes logs");

constinit ConsoleCommand ClashCommand =
    cvar::Command<&Flush>("render.graph.immediate").Help("clashes with a setting");

} // namespace console_input_test

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
    CHECK(help && help.Value() == ConsoleLineResult::Explained && transcript.Contains("(bool)"));

    auto queued = context.ExecuteConsoleLine("render.graph.immediate true", transcript.Output());
    CHECK(queued && queued.Value() == ConsoleLineResult::Queued);
    CHECK(!fixture.Graph().immediate);
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().immediate);
    transcript.text.Clear();
    REQUIRE(context.ExecuteConsoleLine("render.graph.immediate", transcript.Output()));
    CHECK(transcript.Contains("render.graph.immediate = true") &&
          transcript.Contains("Console true render.graph.immediate=true  <- applied"));

    transcript.text.Clear();
    auto rejected = context.ExecuteConsoleLine("render.graph.immediate maybe", transcript.Output());
    CHECK(rejected && rejected.Value() == ConsoleLineResult::Rejected &&
          transcript.Contains("render.graph.immediate "));
    auto unknown = context.ExecuteConsoleLine("render.nothing 1", transcript.Output());
    CHECK(unknown && unknown.Value() == ConsoleLineResult::Unknown &&
          transcript.Contains("render.nothing is not a console variable or command"));

    // `reset`은 콘솔 층 값을 지운다(아래 층 값이 드러난다).
    auto reset = context.ExecuteConsoleLine("render.graph.immediate reset", transcript.Output());
    CHECK(reset && reset.Value() == ConsoleLineResult::Queued);
    REQUIRE(context.Sync());
    CHECK(!fixture.Graph().immediate);
    auto empty = context.ExecuteConsoleLine("   ", transcript.Output());
    CHECK(!empty && empty.Error() == ErrorCode::InvalidArgument);

    // 동적 변수도 같은 문법이다. 출처 스택은 콘솔 층이다.
    DynamicVarDefinition pool;
    pool.name = "stream.pool_mb";
    pool.help = "pool size";
    pool.type = ConsoleValueType::Int32;
    pool.defaultValue = "256";
    REQUIRE(context.RegisterDynamic(pool));
    REQUIRE(context.ExecuteConsoleLine("stream.pool_mb 300", transcript.Output()));
    REQUIRE(context.Sync());
    transcript.text.Clear();
    REQUIRE(context.ExecuteConsoleLine("stream.pool_mb", transcript.Output()));
    CHECK(transcript.Contains("stream.pool_mb = 300") && transcript.Contains("Console 300"));

    // 자동완성은 이름 접두로 찾고 이름 순서다.
    Array<StringView> names(fixture.system);
    REQUIRE(context.CompleteConsoleName("render.graph.", names));
    REQUIRE(names.Size() == 3);
    CHECK(names[0] == "render.graph.debug_draw" && names[1] == "render.graph.immediate" &&
          names[2] == "render.graph.quality");
}

// 명령은 공백으로 나눈 인자를 매개변수 타입으로 읽어(마지막 StringView는 나머지) 큐에 넣고 다음
// Sync에서 한 번 실행한다(대상은 BindCommand). 인자 오류는 넣을 때 거부한다. 같은 핸들 재등록은
// 멱등이고 다른 이름과 겹치면 AlreadyExists다.
TEST(Config_ConsoleCommandsRunAtSync) {
    using namespace console_input_test;
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    ReloadLog log;
    int32 flushed = 0;
    REQUIRE(context.Register(ReloadCommand, FlushCommand));
    REQUIRE(context.Register(FlushCommand));
    CHECK(context.Register(ClashCommand).Error() == ErrorCode::AlreadyExists);
    REQUIRE(context.BindCommand(ReloadCommand, log));
    REQUIRE(context.BindCommand(FlushCommand, flushed));
    CHECK(context.BindCommand(ClashCommand, flushed).Error() == ErrorCode::NotFound);

    Transcript transcript(fixture.system);
    auto queued = context.ExecuteConsoleLine("render.reload 3 all", transcript.Output());
    CHECK(queued && queued.Value() == ConsoleLineResult::CommandQueued);
    CHECK(log.calls == 0 && context.HasPendingChanges());
    REQUIRE(context.Sync());
    CHECK(log.calls == 1 && log.count == 3 && log.targetAll && transcript.Contains("reloaded"));
    REQUIRE(context.Sync());
    CHECK(log.calls == 1);

    auto bogus = context.ExecuteConsoleLine("render.reload many", transcript.Output());
    CHECK(bogus && bogus.Value() == ConsoleLineResult::Rejected);
    auto missing = context.ExecuteConsoleLine("render.reload", transcript.Output());
    CHECK(missing && missing.Value() == ConsoleLineResult::Rejected &&
          transcript.Contains("render.reload needs more arguments"));
    auto extra = context.ExecuteConsoleLine("log.flush now", transcript.Output());
    CHECK(extra && extra.Value() == ConsoleLineResult::Rejected &&
          transcript.Contains("log.flush takes fewer arguments"));
    auto explained = context.ExecuteConsoleLine("render.reload ?", transcript.Output());
    CHECK(explained && explained.Value() == ConsoleLineResult::Explained &&
          transcript.Contains("render.reload (command <int32> <string>)"));
    REQUIRE(context.ExecuteConsoleLine("log.flush", transcript.Output()));
    REQUIRE(context.ExecuteConsoleLine("render.reload 1", transcript.Output()));
    REQUIRE(context.Sync());
    CHECK(flushed == 1 && log.calls == 2 && log.count == 1 && !log.targetAll);

    // 인자는 공백과 탭으로 나눈다. int32 범위를 넘는 인자는 넣을 때 거부한다.
    auto tabbed = context.ExecuteConsoleLine("render.reload	5	all", transcript.Output());
    CHECK(tabbed && tabbed.Value() == ConsoleLineResult::CommandQueued);
    auto overflow = context.ExecuteConsoleLine("render.reload 99999999999 x", transcript.Output());
    CHECK(overflow && overflow.Value() == ConsoleLineResult::Rejected);
    REQUIRE(context.Sync());
    CHECK(log.calls == 3 && log.count == 5 && log.targetAll);
    REQUIRE(context.UnbindCommand(ReloadCommand, log));
    REQUIRE(context.UnbindCommand(FlushCommand, flushed));
}

// 설정 snapshot은 sync 지점에서 RenderRead 값(콘솔 이름, 이름 없는 필드는
// `<Branch>.<Section>.<필드>`)으로 만든다. RenderRead 값이 바뀐 Sync만 세대가 오르고, 아니면 같은
// snapshot을 돌려준다. 프레임이 쥔 이전 snapshot은 바뀌지 않는다.
TEST(Config_SnapshotGenerationsAndReuse) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    SharedPtr<const ConfigSnapshot> first = context.Acquire();
    REQUIRE(first.Get() != nullptr);
    // Initialize가 빈 snapshot(세대 1)을, 내장 핸들(cheat.enabled·cvar.*) 등록이 세대 2를, 첫
    // Sync(설정 struct 등록 — 값·이름 색인)가 세대 3을 게시한다.
    CHECK(first->Generation() == 3);
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
    CHECK(context.Acquire().Get() == first.Get());

    // RenderRead 값의 변경은 새 세대다. 이전 snapshot은 그대로다. Sync 전에는 snapshot이 바뀌지
    // 않는다.
    REQUIRE(
        context.AddAssignment(ConfigLayer::Console, "console", "render.graph.immediate", "true"));
    REQUIRE(
        context.AddAssignment(ConfigLayer::Session, "cli", "Render.RenderGraph.lodBias", "0.5"));
    CHECK(context.Acquire().Get() == first.Get());
    REQUIRE(context.Sync());
    SharedPtr<const ConfigSnapshot> second = context.Acquire();
    REQUIRE(second.Get() != nullptr && second.Get() != first.Get());
    CHECK(second->Generation() == 4);
    CHECK(second->Find("render.graph.immediate")->boolean &&
          second->Find("Render.RenderGraph.lodBias")->float32 == 0.5f);
    CHECK(!first->Find("render.graph.immediate")->boolean);

    // 동적 변수 등록은 새 칸을 더한 snapshot을 바로 게시한다(CVar 칸 — 이름 블록이 아니다). 그
    // 뒤 바뀐 것이 없는 Sync는 같은 snapshot이다.
    DynamicVarDefinition pool;
    pool.name = "stream.pool_mb";
    pool.help = "pool size";
    pool.type = ConsoleValueType::Int32;
    pool.defaultValue = "256";
    auto index = context.RegisterDynamic(pool);
    REQUIRE(index);
    SharedPtr<const ConfigSnapshot> third = context.Acquire();
    CHECK(third->Generation() == 5 && third->Find("stream.pool_mb") == nullptr);
    CHECK(cvar::ReadDynamic<int32>(*third, index.Value(), 0) == 256);
    CHECK(cvar::ReadDynamic<int32>(*second, index.Value(), 7) == 7);
    REQUIRE(context.Sync());
    CHECK(context.Acquire().Get() == third.Get());
}
