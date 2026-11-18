#include "ConfigTestSupport.h"

#include <Config/CVar.h>

// CVar CVR2 시험이다(Docs/Design/CVarSystem.ko.md 6·7·10·19절): 게임 lane의 `Get()`(등록 즉시 게시
// — VD17, 값은 Sync에서만 바뀐다), 조각 snapshot(바뀐 것이 없으면 재사용, 바뀐 칸만 세대 +1), 층
// 순서 (Hotfix·Session·Code·Scoped·Console·Server), `Set`·`Clear`·`ClearLayer`, `ScopedOverride`
// 중첩, `Watcher`·`OnSynced`. 여러 스레드 시험은 CVarThreadTests.cpp다.
using namespace rocks;
using namespace rocks::config;
using namespace rocks::test::config;

namespace cvar_snapshot_test {

enum class Detail : int32 {
    Low,
    High
};

constexpr Span<const StringView> CVarEnumNames(Detail) noexcept {
    static constexpr StringView names[] = {"Low", "High"};
    return names;
}

constinit CVar<float> SnapDistance =
    cvar::Var("render.snap.distance", 100.f).Help("SnapDistance").Range(10, 1000).Clamp();

constinit CVar<int32> SnapCount = cvar::Var("render.snap.count", 4).Help("SnapCount").Range(1, 8);

constinit CVar<bool> SnapEnabled = cvar::Var("render.snap.enabled", false).Help("SnapEnabled");

constinit CVar<Detail> DetailLevel = cvar::Var("render.snap.detail", Detail::Low).Help("Detail");

constinit CVar<String> SnapLabel = cvar::Var("render.snap.label", "first").Help("SnapLabel");

constinit CVar<rocks::Name> SnapTag =
    cvar::Var<rocks::Name>("render.snap.tag", "Alpha").Help("SnapTag");

constinit CVar<int32> SnapLocked =
    cvar::Var("render.snap.locked", 1).Help("Console cannot write").ConsoleReadOnly();

constinit BootCVar<int32> SnapThreads = cvar::Boot("render.snap.threads", 2).Help("SnapThreads");

constinit CVar<int32> SnapLate = cvar::Var("render.snap.late", 7).Help("Registered later");

} // namespace cvar_snapshot_test

namespace {

using namespace cvar_snapshot_test;

Result<void> RegisterSnapshotVars(ConfigContext& config) noexcept {
    return config.Register(SnapDistance, SnapCount, SnapEnabled, DetailLevel, SnapLabel, SnapTag,
                           SnapLocked, SnapThreads);
}

} // namespace

// 게임 lane은 문맥이 스스로 묶는다: 등록 즉시 같은 프레임에 읽을 수 있고(기본값·대기 값), 그 뒤
// 값은 Sync에서만 바뀐다. 모든 타입을 칸에서 읽는다(String은 view, Name은 문맥 pool의 토큰).
TEST(CVar_GetReadsTheGameLaneSnapshot) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.AddLayerText(ConfigLayer::Engine, "Render.json", "Render", "engine",
                                 R"({"Settings": {"render.snap.count": 6}})"));
    REQUIRE(context.Sync());
    REQUIRE(RegisterSnapshotVars(context));
    CHECK(SnapDistance.Get() == 100.f && SnapCount.Get() == 6 && !SnapEnabled.Get());
    CHECK(DetailLevel.Get() == Detail::Low && SnapLabel.Get() == "first" && SnapThreads.Get() == 2);
    auto alpha = fixture.names.TryIntern("Alpha");
    REQUIRE(alpha);
    CHECK(SnapTag.Get() == alpha.Value());

    REQUIRE(context.AddLayerText(ConfigLayer::Project, "Render.json", "Render", "project",
                                 "{\"Settings\": {\"render.snap.enabled\": true, "
                                 "\"render.snap.detail\": \"High\", \"render.snap.label\": "
                                 "\"second\", \"render.snap.tag\": \"Beta\"}}"));
    CHECK(!SnapEnabled.Get() && SnapLabel.Get() == "first");
    REQUIRE(context.Sync());
    CHECK(SnapEnabled.Get() && DetailLevel.Get() == Detail::High && SnapLabel.Get() == "second");
    auto beta = fixture.names.TryIntern("Beta");
    REQUIRE(beta);
    CHECK(SnapTag.Get() == beta.Value());
}

// 등록은 기존 칸을 바꾸지 않고 새 칸만 더한 snapshot을 바로 게시한다(VD17). 등록 전 snapshot에는 그
// 칸이 없다. 바뀐 것이 없는 Sync는 같은 snapshot을 남기고, 값이 바뀐 칸만 세대가 오른다.
TEST(CVar_RegisterPublishesAndSyncCopiesOnlyChanges) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(RegisterSnapshotVars(context));
    SharedPtr<const ConfigSnapshot> before = context.Acquire();
    REQUIRE(context.Register(SnapLate));
    SharedPtr<const ConfigSnapshot> after = context.Acquire();
    REQUIRE(after.Get() != before.Get());
    CHECK(after->Generation() == before->Generation() + 1);
    CHECK(before->FindSlot(SnapLate.Index().value) == nullptr);
    REQUIRE(after->FindSlot(SnapLate.Index().value) != nullptr);
    CHECK(after->FindSlot(SnapLate.Index().value)->generation == 1);
    CHECK(SnapLate.Get() == 7);
    const uint32 countGeneration = after->FindSlot(SnapCount.Index().value)->generation;
    CHECK(after->FindSlot(SnapDistance.Index().value)->generation ==
          before->FindSlot(SnapDistance.Index().value)->generation);

    // 바뀐 것이 없는 Sync(콘솔 설정이 아닌 섹션 값 변경)는 snapshot을 재사용한다.
    REQUIRE(
        context.AddAssignment(ConfigLayer::Session, "cli", "Render.RenderGraph.maxPasses", "3"));
    REQUIRE(context.Sync());
    CHECK(context.Acquire().Get() == after.Get());

    REQUIRE(SnapCount.Set(5));
    REQUIRE(context.Sync());
    SharedPtr<const ConfigSnapshot> changed = context.Acquire();
    REQUIRE(changed.Get() != after.Get());
    CHECK(changed->FindSlot(SnapCount.Index().value)->generation == countGeneration + 1);
    CHECK(changed->FindSlot(SnapDistance.Index().value)->generation ==
          after->FindSlot(SnapDistance.Index().value)->generation);
    CHECK(SnapCount.Get() == 5);
    // 옛 snapshot은 그대로다.
    const CVarSlot* old = after->FindSlot(SnapCount.Index().value);
    REQUIRE(old != nullptr);
    CHECK(static_cast<int64>(old->bits) == 4);
}

// 층 순서: Hotfix < Session < Code < Scoped < Console < Server(6.1절). Clear는 그 (층, 태그)를,
// ClearLayer는 층 전체를 지운다. 같은 (층, 태그)의 Set은 앞 Set을 바꾼다.
TEST(CVar_LayerOrderSetClearAndClearLayer) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(RegisterSnapshotVars(context));
    REQUIRE(SnapCount.Set(2, {.layer = ConfigLayer::Hotfix, .tag = "patch-1"}));
    REQUIRE(context.Sync());
    CHECK(SnapCount.Get() == 2);
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "--set", "render.snap.count", "3"));
    REQUIRE(context.Sync());
    CHECK(SnapCount.Get() == 3);
    REQUIRE(SnapCount.Set(4));
    REQUIRE(SnapCount.Set(5));
    REQUIRE(context.Sync());
    CHECK(SnapCount.Get() == 5);
    REQUIRE(SnapCount.Set(7, {.layer = ConfigLayer::Console}));
    REQUIRE(context.Sync());
    CHECK(SnapCount.Get() == 7);
    REQUIRE(SnapCount.Set(8, {.layer = ConfigLayer::Server}));
    REQUIRE(SnapCount.Set(1, {.layer = ConfigLayer::Console}));
    REQUIRE(context.Sync());
    CHECK(SnapCount.Get() == 8);

    REQUIRE(SnapCount.Clear({.layer = ConfigLayer::Server}));
    REQUIRE(context.Sync());
    CHECK(SnapCount.Get() == 1);
    REQUIRE(context.ClearLayer(ConfigLayer::Console));
    REQUIRE(context.Sync());
    CHECK(SnapCount.Get() == 5);
    REQUIRE(SnapCount.Clear());
    REQUIRE(context.Sync());
    CHECK(SnapCount.Get() == 3);
    REQUIRE(context.Unset(ConfigLayer::Session, "--set"));
    REQUIRE(context.ClearLayer(ConfigLayer::Hotfix));
    REQUIRE(context.Sync());
    CHECK(SnapCount.Get() == 4);
}

// Set은 범위·층 권한을 바로 검사한다(Clamp는 보정해 받고 적용 때 Clamped를 남긴다).
TEST(CVar_SetChecksImmediately) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(RegisterSnapshotVars(context));
    auto outOfRange = SnapCount.Set(9);
    CHECK(!outOfRange && outOfRange.Error() == ErrorCode::InvalidArgument);
    auto readOnly = SnapLocked.Set(2, {.layer = ConfigLayer::Console});
    CHECK(!readOnly && readOnly.Error() == ErrorCode::InvalidArgument);
    auto codeDefault = SnapCount.Set(2, {.layer = ConfigLayer::CodeDefault});
    CHECK(!codeDefault && codeDefault.Error() == ErrorCode::InvalidArgument);
    REQUIRE(SnapLocked.Set(3));
    REQUIRE(SnapDistance.Set(5000.f));
    context.Diagnostics().Clear();
    REQUIRE(context.Sync());
    CHECK(SnapLocked.Get() == 3 && SnapDistance.Get() == 1000.f);
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::Clamped));
}

// ScopedOverride는 Scoped 층에 고유 태그로 쓴다: 중첩하면 나중 것이 이기고, 어떤 순서로 풀어도 남은
// 것 중 가장 나중 것이 이기며, 모두 풀면 원래 값이다. 콘솔 값은 Scoped보다 강하다.
TEST(CVar_ScopedOverridesNestAndUnwindInAnyOrder) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(RegisterSnapshotVars(context));
    {
        cvar::ScopedOverride outer{SnapDistance, 500.f};
        REQUIRE(outer.Status());
        REQUIRE(context.Sync());
        CHECK(SnapDistance.Get() == 500.f);
        {
            cvar::ScopedOverride inner{SnapDistance, 600.f};
            REQUIRE(context.Sync());
            CHECK(SnapDistance.Get() == 600.f);
        }

        REQUIRE(context.Sync());
        CHECK(SnapDistance.Get() == 500.f);
    }

    REQUIRE(context.Sync());
    CHECK(SnapDistance.Get() == 100.f);

    // 먼저 만든 것을 먼저 푼다(LIFO가 아님).
    auto first =
        UniquePtr<cvar::ScopedOverride<float>>::TryMake(fixture.system, SnapDistance, 300.f);
    auto second =
        UniquePtr<cvar::ScopedOverride<float>>::TryMake(fixture.system, SnapDistance, 400.f);
    REQUIRE(first && second);
    REQUIRE(context.Sync());
    CHECK(SnapDistance.Get() == 400.f);
    first.Value() = nullptr;
    REQUIRE(context.Sync());
    CHECK(SnapDistance.Get() == 400.f);
    REQUIRE(SnapDistance.Set(700.f, {.layer = ConfigLayer::Console}));
    REQUIRE(context.Sync());
    CHECK(SnapDistance.Get() == 700.f);
    second.Value() = nullptr;
    REQUIRE(context.ClearLayer(ConfigLayer::Console));
    REQUIRE(context.Sync());
    CHECK(SnapDistance.Get() == 100.f);
}

// Watcher는 첫 호출이 true이고, 값이 바뀌지 않으면 false, 바뀐 Sync 뒤 true다. OnSynced는 Sync마다
// 한 번, 값이 바뀐 번호(이름 순서)와 바뀐 설정 struct를 넘긴다.
TEST(CVar_WatcherAndOnSynced) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(RegisterSnapshotVars(context));
    cvar::Watcher watcher{SnapCount};
    CHECK(watcher.Changed());
    CHECK(!watcher.Changed());

    struct Seen {
        uint32 calls = 0;
        Array<cvar::Index> variables;
        Array<TypeId> settings;
    };

    Seen seen{0, Array<cvar::Index>(fixture.system), Array<TypeId>(fixture.system)};
    auto added = context.OnSynced().TryAddLambda([&seen](const SyncChanges& changes) noexcept {
        ++seen.calls;
        seen.variables.Clear();
        seen.settings.Clear();
        static_cast<void>(seen.variables.TryAppend(changes.variables));
        static_cast<void>(seen.settings.TryAppend(changes.settings));
    });
    REQUIRE(added);
    REQUIRE(context.Sync());
    CHECK(seen.calls == 1 && seen.variables.IsEmpty() && seen.settings.IsEmpty());
    CHECK(!watcher.Changed());

    REQUIRE(SnapEnabled.Set(true));
    REQUIRE(SnapCount.Set(3));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli", "Game.Gameplay.seed", "9"));
    REQUIRE(context.Sync());
    CHECK(seen.calls == 2);
    // 이름 순서: render.snap.count < render.snap.enabled.
    REQUIRE(seen.variables.Size() == 2);
    CHECK(seen.variables[0] == SnapCount.Index() && seen.variables[1] == SnapEnabled.Index());
    REQUIRE(seen.settings.Size() == 1);
    CHECK(seen.settings[0] == GeneratedType<GameplaySettings>::Id());
    CHECK(watcher.Changed());
    CHECK(!watcher.Changed());
    CHECK(context.OnSynced().Remove(added.Value()));
}
