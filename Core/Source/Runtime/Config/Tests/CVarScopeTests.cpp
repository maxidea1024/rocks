#include "ConfigTestSupport.h"

#include <Config/CVar.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>

// CVar CVR3 범위 계층 시험이다(Docs/Design/CVarSystem.ko.md 8·10.1·13.1절): 범위 사슬과 재연결,
// snapshot의 범위 표(게시 시점·이전 snapshot), 범위 쓰기의 검사·ScopeNotAllowed·치트 판정(받을 때·
// 큐 순서), 다른 스레드의 범위 쓰기, 콘솔 표시, ScopedOverride의 묶음 판. 순서는 Join·gate로
// 정한다(sleep 없음).
using namespace rocks;
using namespace rocks::config;
using namespace rocks::test::config;

namespace cvar_scope_test {

constinit CVar<int32> ScopeDistance =
    cvar::Var("render.scope.distance", 100).Help("Scoped distance").Range(0, 1000).Clamp();

constinit CVar<bool> ScopeGod = cvar::Var("cheat.scope.god", false).Help("Scoped god mode").Cheat();

constinit CVar<int32> ScopeShared =
    cvar::Var("net.scope.shared", 1).Help("Replicated value").Replicated();

constinit CVar<int32> ScopeMember = cvar::Var("render.scope.member", 1).Help("Group member");

constinit CVarGroup ScopeQuality = cvar::Group("quality.scope.level", 1)
                                       .Help("Scoped quality")
                                       .Level(0, {{ScopeMember, 10}})
                                       .Level(1, {{ScopeMember, 20}});

} // namespace cvar_scope_test

namespace {

using namespace cvar_scope_test;

// 설정 문맥에 시험 핸들을 등록한다.
void RegisterScopeHandles(ConfigContext& context) noexcept {
    REQUIRE(context.Register(ScopeDistance, ScopeGod, ScopeShared, ScopeMember, ScopeQuality));
    REQUIRE(context.Sync());
}

usize CountCode(ConfigContext& context, ConfigDiagnosticCode code) noexcept {
    usize count = 0;
    for (usize index = 0; index < context.Diagnostics().Count(); ++index) {
        count += context.Diagnostics().Get(index).code == code ? 1 : 0;
    }

    return count;
}

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
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    RegisterScopeHandles(context);
    auto world = context.CreateScope();
    REQUIRE(world);
    auto player = context.CreateScope(world.Value());
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
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    RegisterScopeHandles(context);
    auto scope = context.CreateScope();
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

// 범위 값의 치트 판정(13.1절): 콘솔이 출처인 Cheat 변수 범위 쓰기는 그 범위의 치트 허용
// 상태(`cheat.enabled` 범위 값, 없으면 Global)로 받을 때와 적용 때(큐 순서) 판정한다.
TEST(CVar_ScopeCheatPermission) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    RegisterScopeHandles(context);
    auto allowed = context.CreateScope();
    auto denied = context.CreateScope();
    REQUIRE(allowed && denied);
    REQUIRE(cvar::CheatsEnabled.Set(false));
    REQUIRE(cvar::CheatsEnabled.Set(true, {.scope = allowed.Value()}));
    REQUIRE(context.Sync());

    // 받을 때: Global은 꺼져 있지만 범위가 켰으면 받는다.
    ConfigDiagnosticCode reason = ConfigDiagnosticCode::InvalidSetting;
    CHECK(!ScopeGod.Set(true, {.layer = ConfigLayer::Console, .reason = &reason}) &&
          reason == ConfigDiagnosticCode::CheatsDisabled);
    reason = ConfigDiagnosticCode::InvalidSetting;
    CHECK(!ScopeGod.Set(
              true, {.layer = ConfigLayer::Console, .scope = denied.Value(), .reason = &reason}) &&
          reason == ConfigDiagnosticCode::CheatsDisabled);
    REQUIRE(ScopeGod.Set(true, {.layer = ConfigLayer::Console, .scope = allowed.Value()}));
    REQUIRE(context.Sync());
    CHECK(ScopeGod.Get(allowed.Value()) && !ScopeGod.Get(denied.Value()) && !ScopeGod.Get());

    // 적용 때(큐 순서): 같은 Sync에서 그 범위의 치트를 먼저 끄면 뒤의 쓰기는 거부한다.
    REQUIRE(ScopeGod.Clear({.layer = ConfigLayer::Console, .scope = allowed.Value()}));
    REQUIRE(context.Sync());
    REQUIRE(cvar::CheatsEnabled.Set(false, {.scope = allowed.Value()}));
    REQUIRE(ScopeGod.Set(true, {.layer = ConfigLayer::Console, .scope = allowed.Value()}));
    context.Diagnostics().Clear();
    REQUIRE(context.Sync());
    CHECK(!ScopeGod.Get(allowed.Value()));
    CHECK(CountCode(context, ConfigDiagnosticCode::CheatsDisabled) == 1);

    // 코드가 출처인 범위 쓰기는 치트 판정을 받지 않는다(Global 코드 층과 같다).
    REQUIRE(ScopeGod.Set(true, {.scope = denied.Value()}));
    REQUIRE(context.Sync());
    CHECK(ScopeGod.Get(denied.Value()));
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
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    RegisterScopeHandles(context);
    auto scope = context.CreateScope();
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

// ScopedOverride의 묶음 판: 묶음 등급을 범위 임시 변경하면 다음 Sync가 그 등급을 펼치고, 끝나면
// 원래 등급이 돌아온다. 정의되지 않은 등급은 InvalidArgument다.
TEST(CVar_ScopedOverrideOfGroup) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    RegisterScopeHandles(context);
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
