#include "ConfigTestSupport.h"

#include <Config/CVar.h>

// CVar CVR5 프리셋 묶음 시험이다(Docs/Design/CVarSystem.ko.md 10·11절, Config 6.1절): 묶음 등급이
// 품질 묶음 층에 펼쳐져 프로젝트 값을 이기고 더 강한 층(세션)에는 진다, 등급을 바꾸면 앞 펼침이
// 빠진다, 정의되지 않은 등급 거부(UndefinedLevel), 늦게 등록한 항목 변수는 등록 때 펼친 값을
// 받는다, `Quality.json`은 코드 항목의 값만 바꾼다(잘못된 값은 진단 한 번 뒤 코드 값, 파일 층만,
// ReportUnresolved의 모르는 묶음·등급·항목), 묶음의 묶음, 순환 묶음은 MaxGroupPasses 번 뒤
// GroupNoConverge, 조합 검증은 펼칠 등급만 낮추고 묶음 변수는 그대로(장치가 바뀌면 원래 선택 복귀),
// Sync 안에서 갔다 돌아온 칸은 세대가 그대로다.
using namespace rocks;
using namespace rocks::config;
using namespace rocks::test::config;

namespace cvar_group_test {

enum class ShadowTier : int32 {
    Off,
    Low,
    Medium,
    Epic,
    Count
};

constexpr Span<const StringView> CVarEnumNames(ShadowTier) noexcept {
    static constexpr StringView names[] = {"Off", "Low", "Medium", "Epic"};
    return names;
}

constinit CVar<ShadowTier> GroupShadowTier =
    cvar::Var("render.grp.shadow_tier", ShadowTier::Medium).Help("Shadow tier");

constinit CVar<float> GroupShadowDistance =
    cvar::Var("render.grp.shadow_distance", 6000.f).Help("Shadow distance").Range(0, 20000);

// 등급 1은 정의하지 않는다(사이의 등급도 거부한다).
constinit CVarGroup GroupShadow =
    cvar::Group("quality.grp.shadow", 2)
        .Help("Shadow scalability")
        .Level(0, {{GroupShadowTier, ShadowTier::Off}, {GroupShadowDistance, 1000}})
        .Level(2, {{GroupShadowTier, ShadowTier::Medium}, {GroupShadowDistance, 6000}})
        .Level(3, {{GroupShadowTier, ShadowTier::Epic}, {GroupShadowDistance, 15000}});

constinit CVarGroup GroupOverall = cvar::Group("quality.grp.overall", 1)
                                       .Help("Overall scalability")
                                       .Level(0, {{GroupShadow, 0}})
                                       .Level(1, {{GroupShadow, 3}});

constinit CVar<int32> GroupLateMember = cvar::Var("render.grp.late", 1).Help("Registered late");

constinit CVarGroup GroupWithLate = cvar::Group("quality.grp.late", 1)
                                        .Help("Late member group")
                                        .Level(0, {{GroupLateMember, 10}})
                                        .Level(1, {{GroupLateMember, 20}});

extern CVarGroup GroupCycleB;

// A가 B를, B가 A를 서로 다른 등급으로 바꾼다(주기 4 — 멈추지 않는다).
constinit CVarGroup GroupCycleA = cvar::Group("quality.grp.cycle_a", 0)
                                      .Help("Cycle A")
                                      .Level(0, {{GroupCycleB, 1}})
                                      .Level(1, {{GroupCycleB, 0}});

constinit CVarGroup GroupCycleB = cvar::Group("quality.grp.cycle_b", 0)
                                      .Help("Cycle B")
                                      .Level(0, {{GroupCycleA, 0}})
                                      .Level(1, {{GroupCycleA, 1}});

} // namespace cvar_group_test

namespace {

using namespace cvar_group_test;

usize CountCode(ConfigContext& context, ConfigDiagnosticCode code) noexcept {
    usize count = 0;
    for (usize index = 0; index < context.Diagnostics().Count(); ++index) {
        count += context.Diagnostics().Get(index).code == code ? 1 : 0;
    }

    return count;
}

} // namespace

// 묶음 등급이 품질 묶음 층에 펼쳐져 프로젝트 값을 이기고 세션 값에는 진다. 등급을 바꾸면 앞 등급의
// 펼침이 빠지고 새 등급 값이 된다. 정의되지 않은 등급(사이의 1 포함)은 거부한다.
TEST(CVar_GroupExpandsLevelsOverProjectValues) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(GroupShadowTier, GroupShadowDistance, GroupShadow));
    REQUIRE(context.AddLayerText(ConfigLayer::Project, "Render.json", "Render", "project",
                                 R"({"Settings": {"render.grp.shadow_distance": 500}})"));
    REQUIRE(context.Sync());
    CHECK(GroupShadow.Get() == 2);
    CHECK(GroupShadowDistance.Get() == 6000.f && GroupShadowTier.Get() == ShadowTier::Medium);

    REQUIRE(GroupShadow.Set(0));
    REQUIRE(context.Sync());
    CHECK(GroupShadow.Get() == 0);
    CHECK(GroupShadowDistance.Get() == 1000.f && GroupShadowTier.Get() == ShadowTier::Off);

    // 세션 값은 품질 묶음 층보다 강하다.
    REQUIRE(
        context.AddAssignment(ConfigLayer::Session, "--set", "render.grp.shadow_distance", "2500"));
    REQUIRE(GroupShadow.Set(3));
    REQUIRE(context.Sync());
    CHECK(GroupShadowDistance.Get() == 2500.f && GroupShadowTier.Get() == ShadowTier::Epic);

    // 정의되지 않은 등급은 쓸 때 거부한다.
    ConfigDiagnosticCode reason = ConfigDiagnosticCode::InvalidSetting;
    auto undefined = GroupShadow.Set(1, {.reason = &reason});
    CHECK(!undefined && undefined.Error() == ErrorCode::InvalidArgument);
    CHECK(reason == ConfigDiagnosticCode::UndefinedLevel);
    context.Diagnostics().Clear();
    auto file = context.AddAssignment(ConfigLayer::Session, "--set", "quality.grp.shadow", "5");
    CHECK(!file && fixture.HasDiagnostic(ConfigDiagnosticCode::UndefinedLevel));
    CHECK(GroupShadow.Get() == 3);
}

// 항목 변수를 묶음보다 늦게 등록하면 등록 때 펼친 값(대기 값)을 받는다. 묶음의 묶음은 다른 묶음의
// 등급을 품질 묶음 층에 쓴다.
TEST(CVar_GroupLateMembersAndNestedGroups) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(GroupWithLate));
    REQUIRE(context.Sync());
    REQUIRE(context.Register(GroupLateMember));
    CHECK(GroupLateMember.Get() == 20);
    REQUIRE(GroupWithLate.Set(0));
    REQUIRE(context.Sync());
    CHECK(GroupLateMember.Get() == 10);

    REQUIRE(context.Register(GroupShadowTier, GroupShadowDistance, GroupShadow, GroupOverall));
    REQUIRE(context.Sync());
    // overall 1 → shadow 3 → 항목 등급 3. 묶음 변수 shadow는 품질 묶음 층 값(3)이다.
    CHECK(GroupOverall.Get() == 1 && GroupShadow.Get() == 3);
    CHECK(GroupShadowDistance.Get() == 15000.f);
    REQUIRE(GroupOverall.Set(0));
    REQUIRE(context.Sync());
    CHECK(GroupShadow.Get() == 0 && GroupShadowDistance.Get() == 1000.f);
    // 더 강한 층(코드)에서 고른 shadow 등급이 overall의 펼침을 이긴다.
    REQUIRE(GroupShadow.Set(2));
    REQUIRE(context.Sync());
    CHECK(GroupShadow.Get() == 2 && GroupShadowDistance.Get() == 6000.f);
}

// 서로 바꾸는 묶음은 MaxGroupPasses 번 뒤 마지막 결과를 쓰고 GroupNoConverge를 남긴다.
TEST(CVar_GroupCycleStopsAfterMaxPasses) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(GroupCycleA, GroupCycleB));
    context.Diagnostics().Clear();
    REQUIRE(context.Sync());
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::GroupNoConverge));
    CHECK(CountCode(context, ConfigDiagnosticCode::GroupNoConverge) == 1);
}

// `Quality.json`은 코드 항목의 값만 바꾼다(가장 강한 파일이 이긴다). 항목 변수에 맞지 않는 값은
// 진단을 한 번 남기고 코드 값을 쓴다. 파일 층이 아니면 거부한다. ReportUnresolved가 모르는 묶음·
// 정의되지 않은 등급·코드 항목이 아닌 이름을 알린다.
TEST(CVar_GroupQualityFileChangesValuesOnly) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(GroupShadowTier, GroupShadowDistance, GroupShadow));
    REQUIRE(context.AddLayerText(
        ConfigLayer::Engine, "Quality.json", "Quality", "engine",
        R"({"quality.grp.shadow": {"2": {"render.grp.shadow_distance": 7000}}})"));
    REQUIRE(
        context.AddLayerText(ConfigLayer::Project, "Quality.json", "Quality", "project",
                             R"({"quality.grp.shadow": {"2": {"render.grp.shadow_distance": 7500, )"
                             R"("render.grp.shadow_tier": "Low"}}})"));
    REQUIRE(context.Sync());
    CHECK(GroupShadowDistance.Get() == 7500.f && GroupShadowTier.Get() == ShadowTier::Low);

    // 범위 밖 값은 진단 한 번 뒤 코드 값이다(다음 Sync에 다시 내지 않는다).
    context.Diagnostics().Clear();
    REQUIRE(context.ReplaceLayerText(
        ConfigLayer::Project, "Quality.json", "Quality", "project",
        R"({"quality.grp.shadow": {"2": {"render.grp.shadow_distance": 90000}}})"));
    REQUIRE(context.Sync());
    CHECK(GroupShadowDistance.Get() == 6000.f && GroupShadowTier.Get() == ShadowTier::Medium);
    CHECK(CountCode(context, ConfigDiagnosticCode::ValueOutOfRange) == 1);
    REQUIRE(GroupShadow.Set(2, {.layer = ConfigLayer::Console}));
    REQUIRE(context.Sync());
    CHECK(CountCode(context, ConfigDiagnosticCode::ValueOutOfRange) == 1);

    // 파일 층이 아니면 거부한다.
    CHECK(fixture.Rejects(ConfigLayer::Session, "Quality",
                          R"({"quality.grp.shadow": {"2": {"render.grp.shadow_distance": 1}}})"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::LayerNotAllowed));
    CHECK(
        fixture.Rejects(ConfigLayer::Engine, "Quality",
                        R"({"quality.grp.shadow": {"high": {"render.grp.shadow_distance": 1}}})"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidSetting));

    REQUIRE(
        context.AddLayerText(ConfigLayer::Engine, "Extra.json", "Quality", "extra",
                             R"({"quality.grp.unknown": {"0": {"render.grp.x": 1}},)"
                             R"( "quality.grp.shadow": {"1": {"render.grp.shadow_distance": 1},)"
                             R"( "0": {"render.grp.not_entry": 1}}})"));
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    context.Diagnostics().Clear();
    auto reported = context.ReportUnresolved();
    REQUIRE(reported);
    CHECK(reported.Value() == 3);
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::UnresolvedName));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::UndefinedLevel));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidSetting));
}

// 조합 검증은 펼칠 등급만 낮추고 묶음 변수(사용자가 고른 등급)는 그대로 둔다. 기기 특성이 바뀌면
// 원래 선택이 펼쳐진다. 낮춘 결과는 GetQualityLevels와 Note QualityLowered로 보인다.
TEST(CVar_GroupQualityValidationLowersExpandedLevelOnly) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(GroupShadowTier, GroupShadowDistance, GroupShadow));
    DeviceCondition bigMemory(fixture.system);
    REQUIRE(bigMemory.trait.TryAssign(DeviceTraitNames::GpuMemoryMiB));
    bigMemory.atLeast = 8192.0;
    const QualityLevelRule shadowRules[] = {{3, Span<const DeviceCondition>(&bigMemory, 1), {}}};
    const QualityGroupRule rules[] = {{"quality.grp.shadow", {}, 0, shadowRules}};
    DeviceTraits traits(fixture.system);
    REQUIRE(traits.SetNumber(DeviceTraitNames::GpuMemoryMiB, 4096));
    REQUIRE(context.SetQualityValidation(rules, {}, &traits));
    REQUIRE(GroupShadow.Set(3, {.layer = ConfigLayer::User}));
    context.Diagnostics().Clear();
    REQUIRE(context.Sync());
    CHECK(GroupShadow.Get() == 3);
    CHECK(GroupShadowDistance.Get() == 6000.f && GroupShadowTier.Get() == ShadowTier::Medium);
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::QualityLowered));
    QualityResolutionSet levels(fixture.system);
    REQUIRE(context.GetQualityLevels(levels));
    REQUIRE(levels.groups.Size() == 1);
    CHECK(levels.groups[0]->group.View() == "quality.grp.shadow" &&
          levels.groups[0]->requested == 3 && levels.groups[0]->applied == 2 &&
          !levels.groups[0]->reason.IsEmpty());

    // 같은 결과는 다시 알리지 않는다.
    context.Diagnostics().Clear();
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "--set", "render.grp.shadow_tier", "Low"));
    REQUIRE(context.Sync());
    CHECK(CountCode(context, ConfigDiagnosticCode::QualityLowered) == 0);

    // 장치가 바뀌면 원래 선택(3)이 펼쳐진다.
    REQUIRE(traits.SetNumber(DeviceTraitNames::GpuMemoryMiB, 16384));
    REQUIRE(context.SetQualityValidation(rules, {}, &traits));
    REQUIRE(context.Sync());
    CHECK(GroupShadowDistance.Get() == 15000.f);
    REQUIRE(context.GetQualityLevels(levels));
    CHECK(levels.groups[0]->applied == 3 && levels.groups[0]->reason.IsEmpty());
}

// 한 Sync 안에서 등급이 바뀌었다 돌아오면(같은 자리의 쓰기를 바꿈) 항목 칸의 세대는 그대로다.
TEST(CVar_GroupRoundTripKeepsGeneration) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(GroupShadowTier, GroupShadowDistance, GroupShadow));
    REQUIRE(context.Sync());
    const uint32 index = GroupShadowDistance.Index().value;
    const CVarSlot* before = context.Acquire()->FindSlot(index);
    REQUIRE(before != nullptr);
    const uint32 generation = before->generation;
    REQUIRE(GroupShadow.Set(0));
    REQUIRE(GroupShadow.Set(2));
    REQUIRE(context.Sync());
    const CVarSlot* after = context.Acquire()->FindSlot(index);
    REQUIRE(after != nullptr);
    CHECK(after->generation == generation && GroupShadowDistance.Get() == 6000.f);
}
