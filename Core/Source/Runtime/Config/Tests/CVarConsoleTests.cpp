#include "ConfigTestSupport.h"

#include <Config/CVar.h>
#include <Platform/Thread.h>

// CVar CVR6 콘솔 도구 시험이다(Docs/Design/CVarSystem.ko.md 13·15절): 표시(enum `3
// (High)`·`[boot]`· 재시작 대기·품질 묶음 펼침·출처 스택)와 Explain, enum 수 입력, 봉인 뒤 오류
// 문구 형식(설정 struct 필드 포함), `cvar.find`·`cvar.explain`·`cvar.dump` 명령, 치트 허용
// 상태(변수 쓰기·명령), 네트워크 역할과 복제(클라이언트 거부·적용 때 재판정·Export/Import), 덤프
// 왕복(세션 층으로 다시 읽으면 같은 유효값), 다른 스레드의 콘솔 입력(snapshot 이름 색인 — 쓰기
// 큐·Sync 5단계).
using namespace rocks;
using namespace rocks::config;
using namespace rocks::test::config;

namespace cvar_console_test {

enum class ConsoleTier : int32 {
    Low = 1,
    Medium = 2,
    High = 3
};

constexpr Span<const cvar::EnumEntry> CVarEnumNames(ConsoleTier) noexcept {
    static constexpr cvar::EnumEntry entries[] = {{"Low", 1}, {"Medium", 2}, {"High", 3}};
    return entries;
}

constinit CVar<ConsoleTier> ConsoleTierVar =
    cvar::Var("render.console.tier", ConsoleTier::Medium).Help("Tier");

constinit CVar<float> ConsoleDistance =
    cvar::Var("render.console.distance", 8000.f).Help("Distance").Range(500, 20000).Clamp();

constinit CVar<String> ConsoleTitle = cvar::Var("game.console.title", "plain").Help("Title");

constinit BootCVar<int32> ConsoleWorkers =
    cvar::Boot("game.console.workers", 4).Help("Workers").UserSetting();

constinit CVar<bool> ConsoleGod = cvar::Var("cheat.console.god", false).Help("God mode").Cheat();

constinit CVar<int32> ConsoleShared =
    cvar::Var("net.console.shared", 1).Help("Replicated value").Replicated();

constinit CVar<int32> ConsoleOther =
    cvar::Var("net.console.other", 5).Help("Second replicated value").Replicated();

constinit CVar<int32> ConsoleLeaf = cvar::Var("render.console.leaf", 1).Help("Leaf");

constinit CVarGroup ConsoleGroup = cvar::Group("quality.console.group", 1)
                                       .Help("Console group")
                                       .Level(0, {{ConsoleLeaf, 10}})
                                       .Level(1, {{ConsoleLeaf, 20}});

struct Counter {
    int32 calls = 0;
};

Result<void> Spawn(cvar::CommandContext& context, int32 count) noexcept {
    context.Target<Counter>()->calls += count;
    return context.Write("spawned\n");
}

constinit ConsoleCommand ConsoleSpawn =
    cvar::Command<&Spawn>("cheat.console.spawn").Help("Spawn things").Cheat();

} // namespace cvar_console_test

namespace {

using namespace cvar_console_test;

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

Result<ConsoleLineResult> Run(ConfigContext& context, Transcript& transcript,
                              StringView line) noexcept {
    return context.ExecuteConsoleLine(line, transcript.Output());
}

usize CountCode(ConfigContext& context, ConfigDiagnosticCode code) noexcept {
    usize count = 0;
    for (usize index = 0; index < context.Diagnostics().Count(); ++index) {
        count += context.Diagnostics().Get(index).code == code ? 1 : 0;
    }

    return count;
}

// 주석 줄을 뺀 덤프다(출처는 문맥마다 다르다).
void StripComments(StringView text, String& out) noexcept {
    out.Clear();
    StringView rest = text;
    while (!rest.IsEmpty()) {
        StringView line = rest;
        StringView after;
        if (StringView left; rest.Split("\n", left, after, StringComparison::CaseSensitive)) {
            line = left;
        }

        rest = after;
        if (!line.TrimAscii().StartsWith("//", StringComparison::CaseSensitive)) {
            CHECK(out.TryAppend(line));
            CHECK(out.TryAppend("\n"));
        }
    }
}

} // namespace

// `<이름>`은 값(enum `2 (Medium)`)과 출처 스택, `?`는 종류·범위·도움말을 보인다. enum은 수로도
// 쓴다(표에 없는 수는 거부). 부팅 변수는 `[boot]`, 재시작 대기 값과 품질 묶음 펼침 줄이 붙는다.
TEST(CVar_ConsoleShowsValuesAndExplain) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(ConsoleTierVar, ConsoleDistance, ConsoleWorkers, ConsoleLeaf,
                             ConsoleGroup));
    REQUIRE(context.Sync());
    Transcript transcript(fixture.system);
    auto shown = Run(context, transcript, "render.console.tier");
    CHECK(shown && shown.Value() == ConsoleLineResult::Shown);
    CHECK(transcript.Contains("render.console.tier = 2 (Medium)") &&
          transcript.Contains("Default Medium code  <- applied"));
    REQUIRE(Run(context, transcript, "render.console.tier 3"));
    auto outside = Run(context, transcript, "render.console.tier 7");
    CHECK(outside && outside.Value() == ConsoleLineResult::Rejected &&
          transcript.Contains("render.console.tier the value is not one of the enum values"));
    REQUIRE(context.Sync());
    CHECK(ConsoleTierVar.Get() == ConsoleTier::High);

    transcript.text.Clear();
    auto explained = Run(context, transcript, "render.console.distance ?");
    CHECK(explained && explained.Value() == ConsoleLineResult::Explained);
    CHECK(transcript.Contains("render.console.distance = 8000 (float, range 500..20000, clamp)") &&
          transcript.Contains("  Distance"));

    // 부팅 변수와 재시작 대기, 품질 묶음 펼침.
    REQUIRE(context.CompleteStartup());
    REQUIRE(context.AddLayerText(ConfigLayer::User, "Game.json", "Game", "user",
                                 R"({"Settings": {"game.console.workers": 8}})"));
    REQUIRE(context.Sync());
    transcript.text.Clear();
    REQUIRE(Run(context, transcript, "game.console.workers"));
    CHECK(transcript.Contains("game.console.workers = 4 [boot]") &&
          transcript.Contains("  pending restart: 8"));
    REQUIRE(Run(context, transcript, "quality.console.group"));
    CHECK(transcript.Contains("expands quality.console.group@1"));
    REQUIRE(Run(context, transcript, "render.console.leaf"));
    CHECK(transcript.Contains("Quality 20 quality.console.group@1  <- applied"));

    // 봉인 뒤 부팅 값 직접 쓰기는 `<이름> <사유>` 한 줄이다(설정 struct 필드도 같다).
    transcript.text.Clear();
    auto sealed = Run(context, transcript, "game.console.workers 6");
    CHECK(sealed && sealed.Value() == ConsoleLineResult::Rejected &&
          transcript.Contains("game.console.workers is a boot variable; set it in a config file "
                              "or with --set and restart"));
    auto field = Run(context, transcript, "game.rules.seed 9");
    CHECK(field && field.Value() == ConsoleLineResult::Rejected &&
          transcript.Contains("game.rules.seed is a boot variable"));
    context.Diagnostics().Clear();
    auto path =
        context.AddAssignment(ConfigLayer::Console, "console", "Render.RenderGraph.workers", "4");
    CHECK(!path && fixture.HasDiagnostic(ConfigDiagnosticCode::BootSealed));
}

// cvar.find·cvar.explain·cvar.dump는 명령이다(Sync 5단계에 실행하고 sink에 쓴다).
TEST(CVar_ConsoleToolCommands) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(ConsoleTierVar, ConsoleDistance, ConsoleTitle));
    REQUIRE(context.Sync());
    Transcript transcript(fixture.system);
    auto found = Run(context, transcript, "cvar.find console.di");
    CHECK(found && found.Value() == ConsoleLineResult::CommandQueued);
    REQUIRE(Run(context, transcript, "cvar.explain render.console.tier"));
    REQUIRE(ConsoleTitle.Set("say \"hi\""));
    REQUIRE(context.Sync());
    REQUIRE(Run(context, transcript, "cvar.dump overrides"));
    REQUIRE(context.Sync());
    CHECK(transcript.Contains("render.console.distance = 8000  Distance"));
    CHECK(!transcript.Contains("render.console.tier = 2 (Medium)  Tier"));
    CHECK(transcript.Contains("render.console.tier = 2 (Medium) ("));
    CHECK(transcript.Contains("\"game.console.title\": \"say \\\"hi\\\"\""));
    CHECK(!transcript.Contains("\"render.console.distance\""));
    auto bad = Run(context, transcript, "cvar.dump everything");
    CHECK(bad && bad.Value() == ConsoleLineResult::CommandQueued);
    REQUIRE(context.Sync());
    CHECK(transcript.Contains("cvar.dump takes no argument or 'overrides'"));
}

// 치트 허용 상태가 꺼지면 치트 변수의 콘솔 쓰기와 치트 명령을 거부한다(CheatsDisabled). 켜면 다시
// 받는다. 파일·`--set`은 막지 않는다.
TEST(CVar_ConsoleCheatPermission) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(ConsoleGod, ConsoleSpawn));
    Counter counter;
    REQUIRE(context.BindCommand(ConsoleSpawn, counter));
    REQUIRE(context.Sync());
    Transcript transcript(fixture.system);
    REQUIRE(Run(context, transcript, "cheat.console.god true"));
    REQUIRE(Run(context, transcript, "cheat.console.spawn 2"));
    REQUIRE(context.Sync());
    CHECK(ConsoleGod.Get() && counter.calls == 2);

    REQUIRE(cvar::CheatsEnabled.Set(false));
    REQUIRE(context.Sync());
    context.Diagnostics().Clear();
    auto write = Run(context, transcript, "cheat.console.god false");
    CHECK(write && write.Value() == ConsoleLineResult::Rejected &&
          transcript.Contains("cheat.console.god is a cheat and cheats are disabled"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::CheatsDisabled));
    auto command = Run(context, transcript, "cheat.console.spawn 1");
    CHECK(command && command.Value() == ConsoleLineResult::Rejected);
    ConfigDiagnosticCode reason = ConfigDiagnosticCode::InvalidSetting;
    auto set = ConsoleGod.Set(false, {.layer = ConfigLayer::Console, .reason = &reason});
    CHECK(!set && reason == ConfigDiagnosticCode::CheatsDisabled);
    // 파일·`--set`은 막지 않는다(앞 콘솔 값이 더 강해 값은 그대로다).
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "--set", "cheat.console.god", "false"));
    REQUIRE(context.Sync());
    CHECK(ConsoleGod.Get() && counter.calls == 2);

    REQUIRE(cvar::CheatsEnabled.Clear());
    REQUIRE(context.Sync());
    REQUIRE(Run(context, transcript, "cheat.console.spawn 1"));
    REQUIRE(context.Sync());
    CHECK(counter.calls == 3);
}

// 클라이언트 역할에서 복제 변수의 콘솔 쓰기는 거부한다. 넣을 때 받은 쓰기도 적용 때 역할이
// 바뀌었으면 거부한다. 서버의 ExportReplicated 목록을 클라이언트가 ImportReplicated하면 서버 층
// 값이 되고, 다음 목록에 없는 값은 빠진다.
TEST(CVar_ConsoleReplicationRoles) {
    ConfigFixture server;
    ConfigContext& host = server.Context();
    REQUIRE(host.Register(ConsoleShared, ConsoleOther));
    REQUIRE(host.SetNetworkRole(NetworkRole::Server));
    REQUIRE(ConsoleShared.Set(42));
    REQUIRE(ConsoleOther.Set(9));
    REQUIRE(cvar::CheatsEnabled.Set(false));
    REQUIRE(host.Sync());
    Array<ReplicatedValue> values(server.system);
    REQUIRE(host.ExportReplicated(values));
    REQUIRE(values.Size() == 3);
    CHECK(values[0].name == "cheat.enabled" && values[0].value == "false");
    CHECK(values[1].name == "net.console.other" && values[1].value == "9");
    CHECK(values[2].name == "net.console.shared" && values[2].value == "42");
    // 문자열 사본(다른 문맥의 Sync 뒤에도 쓴다).
    String names[3] = {String(server.system), String(server.system), String(server.system)};
    String texts[3] = {String(server.system), String(server.system), String(server.system)};
    ReplicatedValue copies[3];
    for (usize index = 0; index < 3; ++index) {
        REQUIRE(names[index].TryAssign(values[index].name));
        REQUIRE(texts[index].TryAssign(values[index].value));
        copies[index] = {names[index].View(), texts[index].View()};
    }

    ConfigFixture client;
    ConfigContext& remote = client.Context();
    REQUIRE(remote.Register(ConsoleShared, ConsoleOther));
    REQUIRE(remote.SetNetworkRole(NetworkRole::Client));
    auto imported = remote.ImportReplicated(Span<const ReplicatedValue>(copies, 3));
    REQUIRE(imported);
    CHECK(imported.Value() == 3);
    REQUIRE(remote.Sync());
    CHECK(ConsoleShared.Get() == 42 && ConsoleOther.Get() == 9 && !cvar::CheatsEnabled.Get());
    Transcript transcript(client.system);
    auto rejected = Run(remote, transcript, "net.console.shared 1");
    CHECK(rejected && rejected.Value() == ConsoleLineResult::Rejected &&
          transcript.Contains("net.console.shared is replicated from the server"));

    // 다음 목록에 없는 값은 서버 층에서 빠진다.
    REQUIRE(remote.ImportReplicated(Span<const ReplicatedValue>(copies + 2, 1)));
    REQUIRE(remote.Sync());
    CHECK(ConsoleShared.Get() == 42 && ConsoleOther.Get() == 5 && cvar::CheatsEnabled.Get());

    // 넣을 때(단독) 받은 쓰기도 적용 때 클라이언트면 거부한다.
    REQUIRE(remote.SetNetworkRole(NetworkRole::Standalone));
    REQUIRE(Run(remote, transcript, "net.console.other 3"));
    REQUIRE(remote.SetNetworkRole(NetworkRole::Client));
    remote.Diagnostics().Clear();
    REQUIRE(remote.Sync());
    CHECK(ConsoleOther.Get() == 5 &&
          CountCode(remote, ConfigDiagnosticCode::ReplicatedOnClient) == 1);
    CHECK(transcript.Contains("net.console.other is replicated from the server"));

    // 복제 변수가 아닌 이름은 진단 뒤 건너뛴다.
    const ReplicatedValue unknown[] = {{"render.console.tier", "High"}};
    remote.Diagnostics().Clear();
    auto skipped = remote.ImportReplicated(Span<const ReplicatedValue>(unknown, 1));
    REQUIRE(skipped);
    CHECK(skipped.Value() == 0 && remote.Diagnostics().Count() == 1);
}

// 덤프(기본값과 다른 값)를 빈 문맥의 세션 층으로 읽으면 같은 유효값이다(파일·세션·콘솔·품질 묶음·
// enum·따옴표 든 문자열·설정 struct 필드·부팅 변수).
TEST(CVar_ConsoleDumpRoundTrip) {
    SystemAllocator allocator;
    String dumped(allocator);
    String expected(allocator);
    {
        ConfigFixture fixture;
        ConfigContext& context = fixture.Context();
        REQUIRE(context.Register(ConsoleTierVar, ConsoleDistance, ConsoleTitle, ConsoleWorkers,
                                 ConsoleLeaf, ConsoleGroup));
        REQUIRE(context.AddLayerText(
            ConfigLayer::Project, "Game.json", "Game", "project",
            R"({"Settings": {"render.console.distance": 1200.5, "game.rules.title": "a \"b\""}})"));
        REQUIRE(context.AddAssignment(ConfigLayer::Session, "--set", "game.console.workers", "6"));
        REQUIRE(context.AddAssignment(ConfigLayer::Console, "console", "render.console.tier", "1"));
        REQUIRE(context.AddAssignment(ConfigLayer::Console, "console", "render.graph.immediate",
                                      "true"));
        REQUIRE(ConsoleGroup.Set(0));
        REQUIRE(ConsoleTitle.Set("x\\y"));
        REQUIRE(context.Sync());
        REQUIRE(context.DumpConsoleValues(dumped, true));
        CHECK(dumped.View().Contains("// [boot] Session game.console.workers=6",
                                     StringComparison::CaseSensitive));
        REQUIRE(context.DumpConsoleValues(expected, false));
    }

    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(ConsoleTierVar, ConsoleDistance, ConsoleTitle, ConsoleWorkers,
                             ConsoleLeaf, ConsoleGroup));
    REQUIRE(context.AddLayerText(ConfigLayer::Session, "dump", "Engine", "dump", dumped.View()));
    REQUIRE(context.Sync());
    String again(allocator);
    REQUIRE(context.DumpConsoleValues(again, false));
    String left(allocator);
    String right(allocator);
    StripComments(expected.View(), left);
    StripComments(again.View(), right);
    CHECK(left.View() == right.View());
    CHECK(ConsoleTierVar.Get() == ConsoleTier::Low && ConsoleLeaf.Get() == 10 &&
          ConsoleTitle.Get() == "x\\y" && fixture.Graph().immediate);
}

namespace {

// 다른 스레드에서 콘솔 입력 넷을 넣는다(쓰기·표시·명령·모르는 이름).
struct OffThreadInput {
    ConfigContext* context = nullptr;
    Transcript* transcript = nullptr;
    ConsoleLineResult results[4]{};
    bool ok = true;
};

void SubmitOffThread(void* raw) noexcept {
    auto& input = *static_cast<OffThreadInput*>(raw);
    const StringView lines[] = {"render.console.tier High", "render.console.tier",
                                "cvar.find console.tier", "render.console.none 1"};
    for (usize index = 0; index < 4; ++index) {
        auto result = input.context->ExecuteConsoleLine(lines[index], input.transcript->Output());
        input.ok = input.ok && static_cast<bool>(result);
        input.results[index] = result ? result.Value() : ConsoleLineResult::Unknown;
    }
}

} // namespace

// 다른 스레드의 입력은 snapshot 이름 색인으로 해석해 쓰기는 큐에, 표시·명령은 다음 Sync 5단계에
// 게임 lane이 처리한다(출력은 그 입력의 sink). 모르는 이름은 바로 알린다.
TEST(CVar_ConsoleInputFromAnotherThread) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(ConsoleTierVar));
    REQUIRE(context.Sync());
    Transcript transcript(fixture.system);
    OffThreadInput input;
    input.context = &context;
    input.transcript = &transcript;
    platform::Thread thread;
    REQUIRE(thread.Start(&SubmitOffThread, &input) == platform::ThreadStatus::Success);
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    REQUIRE(input.ok);
    CHECK(input.results[0] == ConsoleLineResult::Queued);
    CHECK(input.results[1] == ConsoleLineResult::CommandQueued);
    CHECK(input.results[2] == ConsoleLineResult::CommandQueued);
    CHECK(input.results[3] == ConsoleLineResult::Unknown);
    CHECK(transcript.Contains("render.console.none is not a console variable or command"));
    CHECK(ConsoleTierVar.Get() == ConsoleTier::Medium);
    REQUIRE(context.Sync());
    CHECK(ConsoleTierVar.Get() == ConsoleTier::High);
    CHECK(transcript.Contains("render.console.tier = 3 (High)"));
    CHECK(transcript.Contains("render.console.tier = 3 (High)  Tier"));
}
