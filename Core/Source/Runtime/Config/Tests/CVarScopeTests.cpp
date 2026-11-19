#include "ConfigTestSupport.h"

#include <Config/CVar.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>

// CVar CVR3 범위 계층 시험이다(Docs/Design/CVarSystem.ko.md 8·10.1·13.1절): 범위 사슬과 재연결,
// snapshot의 범위 표(게시 시점·이전 snapshot), 범위 쓰기·지우기의 검사·ScopeNotAllowed·치트
// 판정(Global AND 범위 — 받을 때·큐 순서), String·Name 범위 값, 다른 스레드의 범위 읽기·쓰기와 게임
// lane의 범위 파괴 겹침, 콘솔 표시, ScopedOverride의 묶음 판. 순서는 Join·gate로 정한다(sleep
// 없음).
using namespace rocks;
namespace c_var_scope_tests_file {

using namespace rocks::config;
using namespace rocks::test::config;
} // namespace c_var_scope_tests_file

namespace cvar_scope_test {

constinit CVar<int32> ScopeDistance =
    cvar::Var("render.scope.distance", 100).Help("Scoped distance").Range(0, 1000).Clamp();

constinit CVar<bool> ScopeGod = cvar::Var("cheat.scope.god", false).Help("Scoped god mode").Cheat();

constinit CVar<int32> ScopeShared =
    cvar::Var("net.scope.shared", 1).Help("Replicated value").Replicated();

constinit CVar<int32> ScopeMember = cvar::Var("render.scope.member", 1).Help("Group member");

constinit CVar<String> ScopeTitle = cvar::Var("game.scope.title", "plain").Help("Scoped title");

constinit CVar<Name> ScopeMap = cvar::Var<Name>("game.scope.map", "Lobby").Help("Scoped map");

constinit CVar<bool> ScopeLocked =
    cvar::Var("render.scope.locked", false).Help("Read-only on the console").ConsoleReadOnly();

constinit CVarGroup ScopeQuality = cvar::Group("quality.scope.level", 1)
                                       .Help("Scoped quality")
                                       .Level(0, {{ScopeMember, 10}})
                                       .Level(1, {{ScopeMember, 20}});

} // namespace cvar_scope_test

namespace c_var_scope_tests_file {

namespace {

using namespace cvar_scope_test;

// 설정 문맥에 시험 핸들을 등록하고 Sync한다(결과는 호출부가 REQUIRE한다).
Result<void> RegisterScopeHandles(ConfigContext& context) noexcept {
    if (auto registered = context.Register(ScopeDistance, ScopeGod, ScopeShared, ScopeMember,
                                           ScopeQuality, ScopeTitle, ScopeMap, ScopeLocked);
        !registered) {
        return registered;
    }

    return context.Sync();
}

usize CountCode(ConfigContext& context, ConfigDiagnosticCode code) noexcept {
    usize count = 0;
    for (usize index = 0; index < context.Diagnostics().Count(); ++index) {
        count += context.Diagnostics().Get(index).code == code ? 1 : 0;
    }

    return count;
}

// 시험이 REQUIRE로 일찍 끝나도 만든 범위를 파괴한다(범위가 남은 채 문맥을 없애면 Fatal이라 시험
// 하나의 실패가 전체 실행을 멈추지 않게 한다 — CVar 19절 시험 정리). 이미 파괴한 범위는 NotFound라
// 무시한다.
class ScopeCleanup {
public:
    explicit ScopeCleanup(ConfigContext& context) noexcept : context_(context) {
    }

    ~ScopeCleanup() {
        for (usize index = 0; index < count_; ++index) {
            static_cast<void>(context_.DestroyScope(scopes_[index]));
        }
    }

    ScopeCleanup(const ScopeCleanup&) = delete;
    ScopeCleanup& operator=(const ScopeCleanup&) = delete;

    void Track(const Result<cvar::CVarScopeId>& scope) noexcept {
        if (scope && count_ < MaxScopes) {
            scopes_[count_++] = scope.Value();
        }
    }

private:
    static constexpr usize MaxScopes = 8;

    ConfigContext& context_;
    cvar::CVarScopeId scopes_[MaxScopes]{};
    usize count_ = 0;
};

// 콘솔 출력을 모으는 sink다.
struct ScopeText final : ConsoleSink {
    explicit ScopeText(Allocator& allocator) noexcept : text(allocator) {
    }

    Result<void> Write(StringView value) noexcept override {
        return text.TryAppend(value);
    }

    String text;
};

} // namespace

// 범위 사슬: 자식 범위 값이 부모를, 부모가 Global을 이긴다(층과 무관). 새 범위·범위 값은 다음
// Sync의 snapshot부터 보이고, 그 전과 없는 범위는 Global 값이다. 범위를 파괴하면 값은 버리고
// 자식은 조부모에 다시 붙는다. 이전 snapshot은 그 때의 사슬로 답한다.
TEST(CVar_ScopeChainAndReparenting) {
    ConfigFixture fixture(DevelopmentRules());
    ConfigContext& context = fixture.Context();
    ScopeCleanup cleanup(context);
    REQUIRE(RegisterScopeHandles(context));
    auto world = context.CreateScope();
    cleanup.Track(world);
    REQUIRE(world);
    auto player = context.CreateScope(world.Value());
    cleanup.Track(player);
    REQUIRE(player);
    CHECK(!world.Value().IsGlobal() && world.Value() != player.Value());
    REQUIRE(ScopeDistance.Set(300));
    REQUIRE(ScopeDistance.Set(400, {.scope = world.Value()}));
    REQUIRE(ScopeDistance.Set(500, {.layer = ConfigLayer::Console, .scope = player.Value()}));
    // Sync 전: 범위 표가 아직 없다.
    CHECK(ScopeDistance.Get(player.Value()) == 100);
    REQUIRE(context.Sync());
    CHECK(ScopeDistance.Get() == 300 && ScopeDistance.Get(cvar::CVarScopeId{}) == 300);
    CHECK(ScopeDistance.Get(world.Value()) == 400 && ScopeDistance.Get(player.Value()) == 500);
    CHECK(ScopeDistance.Get(cvar::CVarScopeId{~uint64{0}}) == 300);
    // 범위 값의 변경은 Global 칸의 세대를 바꾸지 않는다.
    cvar::Watcher<int32> watcher{ScopeDistance};
    CHECK(watcher.Changed());
    REQUIRE(ScopeDistance.Set(450, {.scope = world.Value()}));
    REQUIRE(context.Sync());
    CHECK(!watcher.Changed() && ScopeDistance.Get(world.Value()) == 450);

    // 자식 값을 지우면 부모 값이 드러난다.
    REQUIRE(ScopeDistance.Clear({.layer = ConfigLayer::Console, .scope = player.Value()}));
    REQUIRE(context.Sync());
    CHECK(ScopeDistance.Get(player.Value()) == 450);
    REQUIRE(ScopeDistance.Set(600, {.scope = player.Value()}));
    REQUIRE(context.Sync());
    SharedPtr<const ConfigSnapshot> before = context.Acquire();

    // World 범위를 파괴하면 player는 Global에 다시 붙는다(자기 값은 남는다). 이전 snapshot은
    // 그대로다.
    REQUIRE(context.DestroyScope(world.Value()));
    REQUIRE(context.Sync());
    CHECK(ScopeDistance.Get(world.Value()) == 300 && ScopeDistance.Get(player.Value()) == 600);
    REQUIRE(ScopeDistance.Clear({.scope = player.Value()}));
    REQUIRE(context.Sync());
    CHECK(ScopeDistance.Get(player.Value()) == 300);
    {
        cvar::SnapshotBinding bound{before};
        CHECK(ScopeDistance.Get(world.Value()) == 450 && ScopeDistance.Get(player.Value()) == 600);
    }

    // 파괴한 범위에 들어온 쓰기는 적용 때 버린다. 오류: 없는 부모·Global 파괴·없는 범위.
    REQUIRE(ScopeDistance.Set(700, {.scope = world.Value()}));
    REQUIRE(context.Sync());
    CHECK(ScopeDistance.Get(world.Value()) == 300);
    CHECK(context.CreateScope(world.Value()).Error() == ErrorCode::NotFound);
    CHECK(context.DestroyScope(cvar::CVarScopeId{}).Error() == ErrorCode::InvalidArgument);
    CHECK(context.DestroyScope(world.Value()).Error() == ErrorCode::NotFound);
    REQUIRE(context.DestroyScope(player.Value()));
    REQUIRE(context.Sync());
}

// 범위 쓰기의 검사: 값은 Global 쓰기와 같은 규칙(범위 밖 거부·Clamp는 적용 때 보정과 Clamped),
// 복제 변수·묶음 변수는 ScopeNotAllowed(cheat.enabled는 예외)다. 범위 값은 dump에 들지 않고 콘솔
// 표시에 `scope <id> ...` 줄로 보인다.
TEST(CVar_ScopeWriteRules) {
    ConfigFixture fixture(DevelopmentRules());
    ConfigContext& context = fixture.Context();
    ScopeCleanup cleanup(context);
    REQUIRE(RegisterScopeHandles(context));
    auto scope = context.CreateScope();
    cleanup.Track(scope);
    REQUIRE(scope);
    ConfigDiagnosticCode reason = ConfigDiagnosticCode::InvalidSetting;
    auto replicated = ScopeShared.Set(2, {.scope = scope.Value(), .reason = &reason});
    CHECK(!replicated && reason == ConfigDiagnosticCode::ScopeNotAllowed);
    reason = ConfigDiagnosticCode::InvalidSetting;
    auto grouped = ScopeQuality.Set(0, {.scope = scope.Value(), .reason = &reason});
    CHECK(!grouped && reason == ConfigDiagnosticCode::ScopeNotAllowed);
    REQUIRE(cvar::CheatsEnabled.Set(false, {.scope = scope.Value()}));
    REQUIRE(ScopeDistance.Set(5000, {.scope = scope.Value()}));
    context.Diagnostics().Clear();
    REQUIRE(context.Sync());
    CHECK(ScopeDistance.Get(scope.Value()) == 1000);
    CHECK(CountCode(context, ConfigDiagnosticCode::Clamped) == 1);
    CHECK(cvar::CheatsEnabled.Get() && !cvar::CheatsEnabled.Get(scope.Value()));

    auto sink = SharedPtr<ScopeText>::TryMake(fixture.system, fixture.system);
    REQUIRE(sink);
    REQUIRE(context.ExecuteConsoleLine("render.scope.distance", sink.Value()));
    StringBuilder<64> line(fixture.system);
    REQUIRE(line.TryAppendFormat("  scope {} Code 1000 ", scope.Value().value));
    CHECK(sink.Value()->text.View().Contains(line.View(), StringComparison::CaseSensitive));
    String dumped(fixture.system);
    REQUIRE(context.DumpConsoleValues(dumped, true));
    CHECK(!dumped.View().Contains("1000", StringComparison::CaseSensitive));
    REQUIRE(context.DestroyScope(scope.Value()));
    REQUIRE(context.Sync());
}

// 범위 값의 치트 판정(13.1절): 콘솔이 출처인 Cheat 변수 범위 쓰기·지우기는 Global 실효값 AND 범위
// 사슬의 `cheat.enabled`로 받을 때와 적용 때(큐 순서) 판정한다. 범위는 치트를 좁히기만 한다(서버가
// 끈 치트를 클라이언트 코드가 범위 값으로 켜지 못한다).
TEST(CVar_ScopeCheatPermission) {
    ConfigFixture fixture(DevelopmentRules());
    ConfigContext& context = fixture.Context();
    ScopeCleanup cleanup(context);
    REQUIRE(RegisterScopeHandles(context));
    // 출시 빌드는 치트 변수를 등록하지 않는다(범위 쓰기도 Unsupported, Get(scope)는 기본값 — 19절).
    if constexpr (IsReleaseBuild) {
        auto scope = context.CreateScope();
        cleanup.Track(scope);
        REQUIRE(scope);
        CHECK(ScopeGod.Set(true, {.scope = scope.Value()}).Error() == ErrorCode::Unsupported);
        REQUIRE(context.Sync());
        CHECK(!ScopeGod.Get(scope.Value()));
        return;
    }

    auto player = context.CreateScope();
    cleanup.Track(player);
    auto other = context.CreateScope();
    cleanup.Track(other);
    REQUIRE(player && other);
    // 서버가 끈 치트: 범위 값으로 켜도 판정은 거부다(값 읽기는 범위 값이 이긴다).
    const ReplicatedValue off[] = {{"cheat.enabled", "false"}};
    REQUIRE(context.ImportReplicated(Span<const ReplicatedValue>(off, 1)));
    REQUIRE(cvar::CheatsEnabled.Set(true, {.scope = player.Value()}));
    REQUIRE(context.Sync());
    CHECK(!cvar::CheatsEnabled.Get() && cvar::CheatsEnabled.Get(player.Value()));
    ConfigDiagnosticCode reason = ConfigDiagnosticCode::InvalidSetting;
    CHECK(!ScopeGod.Set(
              true, {.layer = ConfigLayer::Console, .scope = player.Value(), .reason = &reason}) &&
          reason == ConfigDiagnosticCode::CheatsDisabled);

    // Global이 켜졌으면 범위가 끈 곳만 거부한다(좁히기).
    const ReplicatedValue on[] = {{"cheat.enabled", "true"}};
    REQUIRE(context.ImportReplicated(Span<const ReplicatedValue>(on, 1)));
    REQUIRE(cvar::CheatsEnabled.Set(false, {.scope = other.Value()}));
    REQUIRE(context.Sync());
    reason = ConfigDiagnosticCode::InvalidSetting;
    CHECK(!ScopeGod.Set(
              true, {.layer = ConfigLayer::Console, .scope = other.Value(), .reason = &reason}) &&
          reason == ConfigDiagnosticCode::CheatsDisabled);
    REQUIRE(ScopeGod.Set(true, {.layer = ConfigLayer::Console, .scope = player.Value()}));
    REQUIRE(context.Sync());
    CHECK(ScopeGod.Get(player.Value()) && !ScopeGod.Get(other.Value()));

    // 적용 때(큐 순서): 같은 Sync에서 서버가 치트를 끈 뒤의 범위 지우기·쓰기는 거부한다(받을 때는
    // 아직 켜져 있었다).
    REQUIRE(context.ImportReplicated(Span<const ReplicatedValue>(off, 1)));
    REQUIRE(ScopeGod.Clear({.layer = ConfigLayer::Console, .scope = player.Value()}));
    context.Diagnostics().Clear();
    REQUIRE(context.Sync());
    CHECK(ScopeGod.Get(player.Value()));
    CHECK(CountCode(context, ConfigDiagnosticCode::CheatsDisabled) == 1);
    // 받을 때도 거부한다(지금은 Global이 꺼져 있다).
    reason = ConfigDiagnosticCode::InvalidSetting;
    CHECK(!ScopeGod.Clear(
              {.layer = ConfigLayer::Console, .scope = player.Value(), .reason = &reason}) &&
          reason == ConfigDiagnosticCode::CheatsDisabled);

    // 코드가 출처인 범위 쓰기는 치트 판정을 받지 않는다(Global 코드 층과 같다).
    REQUIRE(ScopeGod.Set(true, {.scope = other.Value()}));
    REQUIRE(context.Sync());
    CHECK(ScopeGod.Get(other.Value()));
    REQUIRE(context.DestroyScope(player.Value()));
    REQUIRE(context.DestroyScope(other.Value()));
    REQUIRE(context.Sync());
}

// 범위 지우기는 Global 층 지우기와 같은 뜻이다(6.2절): 저장된 값의 출처 층보다 약한 층의 지우기는
// 무시하고, 콘솔 지우기는 콘솔 읽기 전용을 받을 때 거부한다. 파괴된 범위의 지우기·쓰기는 진단 없이
// 버린다.
TEST(CVar_ScopeClearFollowsLayers) {
    ConfigFixture fixture(DevelopmentRules());
    ConfigContext& context = fixture.Context();
    ScopeCleanup cleanup(context);
    REQUIRE(RegisterScopeHandles(context));
    auto scope = context.CreateScope();
    cleanup.Track(scope);
    REQUIRE(scope);
    REQUIRE(ScopeDistance.Set(400, {.layer = ConfigLayer::Console, .scope = scope.Value()}));
    REQUIRE(ScopeLocked.Set(true, {.scope = scope.Value()}));
    REQUIRE(context.Sync());
    // 코드 층(콘솔보다 약하다)의 지우기는 콘솔이 쓴 값을 지우지 못한다.
    REQUIRE(ScopeDistance.Clear({.scope = scope.Value()}));
    REQUIRE(context.Sync());
    CHECK(ScopeDistance.Get(scope.Value()) == 400);
    REQUIRE(ScopeDistance.Clear({.layer = ConfigLayer::Console, .scope = scope.Value()}));
    REQUIRE(context.Sync());
    CHECK(ScopeDistance.Get(scope.Value()) == 100);
    ConfigDiagnosticCode reason = ConfigDiagnosticCode::InvalidSetting;
    CHECK(!ScopeLocked.Clear(
              {.layer = ConfigLayer::Console, .scope = scope.Value(), .reason = &reason}) &&
          reason == ConfigDiagnosticCode::ConsoleReadOnly);
    REQUIRE(context.Sync());
    CHECK(ScopeLocked.Get(scope.Value()));

    // 파괴된 범위: 쓰기·지우기는 진단 없이 버린다.
    REQUIRE(ScopeLocked.Clear({.scope = scope.Value()}));
    REQUIRE(context.DestroyScope(scope.Value()));
    REQUIRE(ScopeDistance.Set(5000, {.scope = scope.Value()}));
    context.Diagnostics().Clear();
    REQUIRE(context.Sync());
    CHECK(ScopeDistance.Get(scope.Value()) == 100 && !ScopeLocked.Get(scope.Value()));
    CHECK(CountCode(context, ConfigDiagnosticCode::Clamped) == 1);
    CHECK(context.Diagnostics().Count() == 1);
}

// String·Name 범위 값: 텍스트 블록을 범위 노드가 쥐고, Get(scope)는 범위 값·부모·Global 순서다.
TEST(CVar_ScopeTextValues) {
    ConfigFixture fixture(DevelopmentRules());
    ConfigContext& context = fixture.Context();
    ScopeCleanup cleanup(context);
    REQUIRE(RegisterScopeHandles(context));
    auto world = context.CreateScope();
    cleanup.Track(world);
    REQUIRE(world);
    auto player = context.CreateScope(world.Value());
    cleanup.Track(player);
    REQUIRE(player);
    REQUIRE(ScopeTitle.Set("world title", {.scope = world.Value()}));
    REQUIRE(ScopeMap.Set("Arena", {.scope = player.Value()}));
    REQUIRE(context.Sync());
    CHECK(ScopeTitle.Get() == "plain" && ScopeTitle.Get(world.Value()) == "world title");
    CHECK(ScopeTitle.Get(player.Value()) == "world title");
    auto arena = fixture.names.Acquire(ScopeMap.Get(player.Value()));
    CHECK(arena && arena.Value().View() == "Arena");
    auto lobby = fixture.names.Acquire(ScopeMap.Get(world.Value()));
    CHECK(lobby && lobby.Value().View() == "Lobby");
    REQUIRE(context.DestroyScope(world.Value()));
    REQUIRE(context.DestroyScope(player.Value()));
    REQUIRE(context.Sync());
}

namespace {

// 다른 스레드에서 범위 값을 쓴다(snapshot을 묶어 그 문맥의 쓰기 큐에 넣는다).
struct ScopeWriter {
    SharedPtr<const ConfigSnapshot> snapshot;
    cvar::CVarScopeId scope{};
    ErrorCode set = ErrorCode::Cancelled;
    ErrorCode created = ErrorCode::Cancelled;
    ConfigContext* context = nullptr;
};

void WriteScopeFromWorker(void* raw) noexcept {
    auto& writer = *static_cast<ScopeWriter*>(raw);
    {
        cvar::SnapshotBinding bound{writer.snapshot};
        auto written = ScopeDistance.Set(250, {.scope = writer.scope});
        writer.set = written ? ErrorCode::None : written.Error();
    }

    writer.snapshot = SharedPtr<const ConfigSnapshot>();
    auto created = writer.context->CreateScope();
    writer.created = created ? ErrorCode::None : created.Error();
}

} // namespace

// 다른 스레드의 범위 쓰기는 쓰기 큐로 들어와 다음 Sync에 적용된다. 범위 생성은 게임 lane
// 전용이다(WrongThread). Join이 순서를 정한다.
TEST(CVar_ScopeWritesFromAnotherThread) {
    ConfigFixture fixture(DevelopmentRules());
    ConfigContext& context = fixture.Context();
    ScopeCleanup cleanup(context);
    REQUIRE(RegisterScopeHandles(context));
    auto scope = context.CreateScope();
    cleanup.Track(scope);
    REQUIRE(scope);
    REQUIRE(context.Sync());
    ScopeWriter writer;
    writer.snapshot = context.Acquire();
    writer.scope = scope.Value();
    writer.context = &context;
    platform::Thread thread;
    REQUIRE(thread.Start(&WriteScopeFromWorker, &writer) == platform::ThreadStatus::Success);
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    CHECK(writer.set == ErrorCode::None && writer.created == ErrorCode::WrongThread);
    CHECK(ScopeDistance.Get(scope.Value()) == 100);
    REQUIRE(context.Sync());
    CHECK(ScopeDistance.Get(scope.Value()) == 250);
    REQUIRE(context.DestroyScope(scope.Value()));
    REQUIRE(context.Sync());
}

namespace {

// 다른 스레드의 범위 읽기·쓰기와 게임 lane의 범위 파괴를 gate로 엇갈린다.
struct ScopeOverlap {
    platform::Semaphore bound{0, 1};
    platform::Semaphore destroyed{0, 1};
    SharedPtr<const ConfigSnapshot> before;
    ConfigContext* context = nullptr;
    cvar::CVarScopeId scope{};
    int32 oldRead = 0;
    int32 newRead = 0;
    ErrorCode set = ErrorCode::Cancelled;
};

void ReadAcrossDestroy(void* raw) noexcept {
    auto& overlap = *static_cast<ScopeOverlap*>(raw);
    {
        cvar::SnapshotBinding bound{overlap.before};
        static_cast<void>(overlap.bound.Release(1));
        static_cast<void>(overlap.destroyed.Wait());
        // 게임 lane이 범위를 파괴하고 새 snapshot을 게시한 뒤다: 묶은 snapshot은 그대로 답하고,
        // 그 범위에 넣는 쓰기는 받는다(넣을 때는 범위 표를 보지 않는다 — 적용 때 버린다).
        overlap.oldRead = ScopeDistance.Get(overlap.scope);
        auto written = ScopeDistance.Set(300, {.scope = overlap.scope});
        overlap.set = written ? ErrorCode::None : written.Error();
    }

    overlap.before = SharedPtr<const ConfigSnapshot>();
    cvar::SnapshotBinding latest{overlap.context->Acquire()};
    overlap.newRead = ScopeDistance.Get(overlap.scope);
}

} // namespace

// 다른 스레드가 쥔 snapshot의 범위는 게임 lane이 범위를 파괴하고 Sync해도 그대로 답하고, 새
// snapshot은 Global 값이다. 파괴된 범위로 들어온 쓰기는 적용 때 버린다(10.1절 — gate로 순서를
// 정한다).
TEST(CVar_ScopeReadsOverlapDestroy) {
    ConfigFixture fixture(DevelopmentRules());
    ConfigContext& context = fixture.Context();
    ScopeCleanup cleanup(context);
    REQUIRE(RegisterScopeHandles(context));
    auto scope = context.CreateScope();
    cleanup.Track(scope);
    REQUIRE(scope);
    REQUIRE(ScopeDistance.Set(250, {.scope = scope.Value()}));
    REQUIRE(context.Sync());
    ScopeOverlap overlap;
    overlap.before = context.Acquire();
    overlap.context = &context;
    overlap.scope = scope.Value();
    platform::Thread thread;
    REQUIRE(thread.Start(&ReadAcrossDestroy, &overlap) == platform::ThreadStatus::Success);
    REQUIRE(overlap.bound.Wait() == platform::WaitResult::Signaled);
    REQUIRE(context.DestroyScope(scope.Value()));
    REQUIRE(context.Sync());
    REQUIRE(overlap.destroyed.Release(1));
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    CHECK(overlap.oldRead == 250 && overlap.newRead == 100 && overlap.set == ErrorCode::None);
    REQUIRE(context.Sync());
    CHECK(ScopeDistance.Get(scope.Value()) == 100 && ScopeDistance.Get() == 100);
}

// ScopedOverride의 묶음 판: 묶음 등급을 범위 임시 변경하면 다음 Sync가 그 등급을 펼치고, 끝나면
// 원래 등급이 돌아온다. 정의되지 않은 등급은 InvalidArgument다.
TEST(CVar_ScopedOverrideOfGroup) {
    ConfigFixture fixture(DevelopmentRules());
    ConfigContext& context = fixture.Context();
    ScopeCleanup cleanup(context);
    REQUIRE(RegisterScopeHandles(context));
    REQUIRE(context.Sync());
    CHECK(ScopeQuality.Get() == 1 && ScopeMember.Get() == 20);
    {
        cvar::ScopedOverride low{ScopeQuality, 0};
        REQUIRE(low.Status());
        REQUIRE(context.Sync());
        CHECK(ScopeQuality.Get() == 0 && ScopeMember.Get() == 10);
        cvar::ScopedOverride undefined{ScopeQuality, 7};
        CHECK(undefined.Status().Error() == ErrorCode::InvalidArgument);
    }

    REQUIRE(context.Sync());
    REQUIRE(context.Sync());
    CHECK(ScopeQuality.Get() == 1 && ScopeMember.Get() == 20);
}
} // namespace c_var_scope_tests_file
