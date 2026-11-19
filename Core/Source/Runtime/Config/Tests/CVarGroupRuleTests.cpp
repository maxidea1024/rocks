#include "ConfigTestSupport.h"

#include <Config/CVar.h>

// CVar CVR5 리뷰 반영 시험이다(Docs/Design/CVarSystem.ko.md 10·11절): 묶음의 묶음 자기 참조·순환과
// 항목이 겹치는 두 묶음은 Register가 거부(GroupConflict), 코드 프리셋 값(범위 밖·정의되지 않은 묶음
// 등급·출시 규칙의 개발 전용 변수)은 등록 순서와 무관하게 Error 한 번이고 펼치지 않는다, 묶음
// 해결은 이력에 기대지 않는다(콘솔 값을 지우면 새 문맥과 같은 결과), 조합 검증 예산 + 묶음의 묶음이
// 낮춘 등급으로 다시 펼쳐 멈추고 갔다 돌아온 칸의 세대는 그대로, 멈추지 않는 검증 상호작용은
// MaxGroupPasses 번 뒤 같은 상태에 한 번 GroupNoConverge, `Quality.json` 비정규 등급 키 거부와
// 잘못된 값만 건너뛰기, 품질 묶음 층 `Set` 거부.
using namespace rocks;
using namespace rocks::config;
using namespace rocks::test::config;

namespace cvar_group_rule_test {

constinit CVarGroup RuleSelf =
    cvar::Group("quality.rule.self", 0).Help("Refers to itself").Level(0, {{RuleSelf, 1}}).Level(1);

extern CVarGroup RuleCycleB;

constinit CVarGroup RuleCycleA = cvar::Group("quality.rule.cycle_a", 0)
                                     .Help("Cycle A")
                                     .Level(0, {{RuleCycleB, 0}})
                                     .Level(1, {{RuleCycleB, 1}});

constinit CVarGroup RuleCycleB = cvar::Group("quality.rule.cycle_b", 0)
                                     .Help("Cycle B")
                                     .Level(0, {{RuleCycleA, 0}})
                                     .Level(1, {{RuleCycleA, 1}});

constinit CVar<int32> RuleShared = cvar::Var("render.rule.shared", 1).Help("Shared entry");

constinit CVarGroup RuleFirst =
    cvar::Group("quality.rule.first", 0).Help("First").Level(0, {{RuleShared, 10}});

constinit CVarGroup RuleSecond =
    cvar::Group("quality.rule.second", 0).Help("Second").Level(0, {{RuleShared, 20}});

constinit CVar<float> RuleRanged =
    cvar::Var("render.rule.ranged", 50.f).Help("Ranged").Range(0, 100);

constinit CVar<int32> RuleLeaf = cvar::Var("render.rule.leaf", 1).Help("Leaf");

constinit CVarGroup RuleChild = cvar::Group("quality.rule.child", 0)
                                    .Help("Child")
                                    .Level(0, {{RuleLeaf, 100}})
                                    .Level(1, {{RuleLeaf, 200}});

// 등급 0의 값은 범위 밖(500)과 자식 묶음이 정의하지 않은 등급(7)이다.
constinit CVarGroup RuleBadValues = cvar::Group("quality.rule.bad", 0)
                                        .Help("Bad presets")
                                        .Level(0, {{RuleRanged, 500}, {RuleChild, 7}})
                                        .Level(1, {{RuleRanged, 70}, {RuleChild, 1}});

constinit CVar<int32> RuleCheat = cvar::Var("render.rule.cheat", 1).Help("Cheat").Cheat();

constinit CVarGroup RuleCheatGroup =
    cvar::Group("quality.rule.cheat", 0).Help("Cheat presets").Level(0, {{RuleCheat, 5}});

constinit CVarGroup RuleParent = cvar::Group("quality.rule.parent", 1)
                                     .Help("Parent")
                                     .Level(0, {{RuleChild, 0}})
                                     .Level(1, {{RuleChild, 1}});

constinit CVar<float> RuleFileValue =
    cvar::Var("render.rule.file", 1.f).Help("File value").Range(0, 100);

constinit CVarGroup RuleFile =
    cvar::Group("quality.rule.file", 0).Help("File").Level(0, {{RuleFileValue, 10}});

} // namespace cvar_group_rule_test

namespace {

using namespace cvar_group_rule_test;

usize CountCode(ConfigContext& context, ConfigDiagnosticCode code) noexcept {
    usize count = 0;
    for (usize index = 0; index < context.Diagnostics().Count(); ++index) {
        count += context.Diagnostics().Get(index).code == code ? 1 : 0;
    }

    return count;
}

bool HasMessage(ConfigContext& context, ConfigDiagnosticCode code, StringView part) noexcept {
    for (usize index = 0; index < context.Diagnostics().Count(); ++index) {
        const ConfigDiagnostic item = context.Diagnostics().Get(index);
        if (item.code == code && item.message.Contains(part, StringComparison::CaseSensitive)) {
            return true;
        }
    }

    return false;
}

// 예산 + 묶음의 묶음 시험 재료: parent@1 비용 50, child@1 비용 60, 한도는 gpu.memory_mib × 1.
struct BudgetRules {
    BudgetRules(Allocator& allocator, int32 parentPriority, int32 childPriority) noexcept
        : traits(allocator) {
        parentRules[0] = {0, {}, Span<const QualityCost>(&zero, 1)};
        parentRules[1] = {1, {}, Span<const QualityCost>(&parentCost, 1)};
        childRules[0] = {0, {}, Span<const QualityCost>(&zero, 1)};
        childRules[1] = {1, {}, Span<const QualityCost>(&childCost, 1)};
        groups[0] = {"quality.rule.parent", parentPriority, parentRules};
        groups[1] = {"quality.rule.child", childPriority, childRules};
    }

    const QualityCost zero{"memory", 0};
    const QualityCost parentCost{"memory", 50};
    const QualityCost childCost{"memory", 60};
    QualityLevelRule parentRules[2];
    QualityLevelRule childRules[2];
    QualityGroupRule groups[2];
    const QualityBudgetRule budgets[1] = {{"memory", DeviceTraitNames::GpuMemoryMiB, 1.0}};
    DeviceTraits traits;
};

} // namespace

// 자기 참조·순환 묶음과 같은 변수를 항목으로 가진 두 묶음은 등록하지 않는다(진단에 순환 경로·두
// 묶음 이름). 거부된 호출은 아무것도 등록하지 않는다.
TEST(CVar_GroupRegisterRejectsCyclesAndOverlaps) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    context.Diagnostics().Clear();
    CHECK(context.Register(RuleSelf).Error() == ErrorCode::InvalidArgument);
    CHECK(HasMessage(context, ConfigDiagnosticCode::GroupConflict,
                     "quality.rule.self -> quality.rule.self"));
    CHECK(!RuleSelf.Index().IsValid() || !context.FindConsoleSetting("quality.rule.self"));

    context.Diagnostics().Clear();
    CHECK(context.Register(RuleCycleA, RuleCycleB).Error() == ErrorCode::InvalidArgument);
    CHECK(HasMessage(context, ConfigDiagnosticCode::GroupConflict,
                     "quality.rule.cycle_a -> quality.rule.cycle_b -> quality.rule.cycle_a"));
    // 한쪽을 먼저 등록하면 다른 쪽을 등록할 때 거부한다.
    REQUIRE(context.Register(RuleCycleA));
    CHECK(context.Register(RuleCycleB).Error() == ErrorCode::InvalidArgument);

    context.Diagnostics().Clear();
    REQUIRE(context.Register(RuleShared, RuleFirst));
    CHECK(context.Register(RuleSecond).Error() == ErrorCode::InvalidArgument);
    CHECK(HasMessage(context, ConfigDiagnosticCode::GroupConflict,
                     "'quality.rule.second' and 'quality.rule.first'"));
    CHECK(!context.FindConsoleSetting("quality.rule.second"));
}

// 코드 프리셋 값이 항목 변수에 맞지 않으면(범위 밖, 자식 묶음이 정의하지 않은 등급) 등록 순서와
// 무관하게 (묶음, 등급, 항목)마다 Error 한 번이고 그 항목은 펼치지 않는다. 맞는 등급은 펼친다.
TEST(CVar_GroupCodePresetValuesAreCheckedOnce) {
    {
        ConfigFixture fixture;
        ConfigContext& context = fixture.Context();
        context.Diagnostics().Clear();
        REQUIRE(context.Register(RuleRanged, RuleLeaf, RuleChild, RuleBadValues));
        CHECK(CountCode(context, ConfigDiagnosticCode::ValueOutOfRange) == 1);
        CHECK(CountCode(context, ConfigDiagnosticCode::UndefinedLevel) == 1);
        REQUIRE(context.Sync());
        REQUIRE(context.Sync());
        CHECK(CountCode(context, ConfigDiagnosticCode::ValueOutOfRange) == 1);
        CHECK(CountCode(context, ConfigDiagnosticCode::UndefinedLevel) == 1);
        CHECK(RuleRanged.Get() == 50.f && RuleChild.Get() == 0 && RuleLeaf.Get() == 100);
        REQUIRE(RuleBadValues.Set(1));
        REQUIRE(context.Sync());
        CHECK(RuleRanged.Get() == 70.f && RuleChild.Get() == 1 && RuleLeaf.Get() == 200);
    }

    // 항목 변수를 나중에 등록해도 같다(등록 때 한 번, 펼칠 때 다시 내지 않는다).
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(RuleBadValues));
    REQUIRE(context.Sync());
    context.Diagnostics().Clear();
    REQUIRE(context.Register(RuleRanged));
    CHECK(CountCode(context, ConfigDiagnosticCode::ValueOutOfRange) == 1);
    REQUIRE(context.Sync());
    CHECK(CountCode(context, ConfigDiagnosticCode::ValueOutOfRange) == 1);
    CHECK(RuleRanged.Get() == 50.f);
}

// 출시 규칙에서 개발 전용(치트) 변수는 품질 묶음 층 값도 받지 않는다 — 진단 한 번.
TEST(CVar_GroupPresetForDevelopmentOnlyUnderReleaseRules) {
    if constexpr (IsReleaseBuild) {
        return;
    }

    ConfigContextOptions options;
    options.releaseRules = true;
    ConfigFixture fixture(options);
    ConfigContext& context = fixture.Context();
    context.Diagnostics().Clear();
    REQUIRE(context.Register(RuleCheat, RuleCheatGroup));
    CHECK(CountCode(context, ConfigDiagnosticCode::LayerNotAllowed) == 1);
    REQUIRE(context.Sync());
    CHECK(CountCode(context, ConfigDiagnosticCode::LayerNotAllowed) == 1);
    CHECK(RuleCheat.Get() == 1);
}

// 묶음 해결은 이력에 기대지 않는다: 부모를 콘솔로 바꿨다가 지우면 새 문맥과 같은 결과다.
TEST(CVar_GroupResolutionIsHistoryIndependent) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(RuleLeaf, RuleChild, RuleParent));
    REQUIRE(context.Sync());
    CHECK(RuleChild.Get() == 1 && RuleLeaf.Get() == 200);
    REQUIRE(RuleParent.Set(0, {.layer = ConfigLayer::Console}));
    REQUIRE(context.Sync());
    CHECK(RuleChild.Get() == 0 && RuleLeaf.Get() == 100);
    REQUIRE(RuleParent.Clear({.layer = ConfigLayer::Console}));
    REQUIRE(context.Sync());
    CHECK(RuleParent.Get() == 1 && RuleChild.Get() == 1 && RuleLeaf.Get() == 200);

    ConfigFixture fresh;
    ConfigContext& other = fresh.Context();
    REQUIRE(other.Register(RuleLeaf, RuleChild, RuleParent));
    REQUIRE(other.Sync());
    CHECK(RuleParent.Get() == 1 && RuleChild.Get() == 1 && RuleLeaf.Get() == 200);
}

// 예산 + 묶음의 묶음: child가 먼저 낮아지면(우선순위가 낮다) 낮춘 등급으로 다시 펼쳐 멈춘다. 다음
// Sync에서 첫 차례가 낮추기 전 등급(200)을 펼쳤다가 낮춘 등급(100)으로 돌아와도 칸의 세대는
// 그대로다.
TEST(CVar_GroupBudgetLowersNestedGroupAndKeepsGeneration) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(RuleLeaf, RuleChild, RuleParent, RuleShared));
    BudgetRules rules(fixture.system, 1, 0);
    REQUIRE(rules.traits.SetNumber(DeviceTraitNames::GpuMemoryMiB, 100));
    REQUIRE(context.SetQualityValidation(rules.groups, rules.budgets, &rules.traits));
    context.Diagnostics().Clear();
    REQUIRE(context.Sync());
    CHECK(RuleParent.Get() == 1 && RuleChild.Get() == 1 && RuleLeaf.Get() == 100);
    CHECK(CountCode(context, ConfigDiagnosticCode::GroupNoConverge) == 0);
    QualityResolutionSet levels(fixture.system);
    REQUIRE(context.GetQualityLevels(levels));
    REQUIRE(levels.groups.Size() == 2);
    const QualityResolution& child =
        *levels.groups[levels.groups[0]->group.View() == "quality.rule.child" ? 0 : 1];
    CHECK(child.requested == 1 && child.applied == 0);

    const uint32 index = RuleLeaf.Index().value;
    const uint32 generation = context.Acquire()->FindSlot(index)->generation;
    REQUIRE(RuleShared.Set(7));
    REQUIRE(context.Sync());
    CHECK(RuleLeaf.Get() == 100);
    CHECK(context.Acquire()->FindSlot(index)->generation == generation);
}

// parent가 먼저 낮아지면 child 요청이 바뀌어 검증 결과가 번갈아 멈추지 않는다: MaxGroupPasses 번 뒤
// 마지막 결과를 쓰고 같은 상태에는 GroupNoConverge를 한 번만 낸다. 규칙을 없애면 요청 등급
// 그대로다.
TEST(CVar_GroupNoConvergeIsReportedOncePerState) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(RuleLeaf, RuleChild, RuleParent, RuleShared));
    BudgetRules rules(fixture.system, 0, 1);
    REQUIRE(rules.traits.SetNumber(DeviceTraitNames::GpuMemoryMiB, 100));
    REQUIRE(context.SetQualityValidation(rules.groups, rules.budgets, &rules.traits));
    context.Diagnostics().Clear();
    REQUIRE(context.Sync());
    CHECK(CountCode(context, ConfigDiagnosticCode::GroupNoConverge) == 1);
    REQUIRE(RuleShared.Set(8));
    REQUIRE(context.Sync());
    REQUIRE(RuleShared.Set(9));
    REQUIRE(context.Sync());
    CHECK(CountCode(context, ConfigDiagnosticCode::GroupNoConverge) == 1);

    REQUIRE(context.SetQualityValidation({}, {}, nullptr));
    REQUIRE(context.Sync());
    QualityResolutionSet levels(fixture.system);
    REQUIRE(context.GetQualityLevels(levels));
    for (const UniquePtr<QualityResolution>& level : levels.groups) {
        CHECK(level->applied == level->requested);
    }

    CHECK(RuleChild.Get() == 1 && RuleLeaf.Get() == 200);
}

// `Quality.json` 등급 키는 정규 10진 표기만 받는다. 가장 강한 파일의 값이 맞지 않으면 그 값만
// 건너뛰고(다음으로 강한 파일), 같은 잘못된 값을 다시 읽어도 진단은 입력마다 한 번이다.
TEST(CVar_GroupQualityFileKeysAndFallback) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(RuleFileValue, RuleFile));
    REQUIRE(context.Sync());
    CHECK(fixture.Rejects(ConfigLayer::Project, "Quality",
                          R"({"quality.rule.file": {"+0": {"render.rule.file": 33}}})"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidSetting));
    CHECK(fixture.Rejects(ConfigLayer::Project, "Quality",
                          R"({"quality.rule.file": {"00": {"render.rule.file": 44}}})"));

    REQUIRE(context.AddLayerText(ConfigLayer::Engine, "Quality.json", "Quality", "engine",
                                 R"({"quality.rule.file": {"0": {"render.rule.file": 40}}})"));
    REQUIRE(context.AddLayerText(ConfigLayer::Project, "Quality.json", "Quality", "project",
                                 R"({"quality.rule.file": {"0": {"render.rule.file": 400}}})"));
    context.Diagnostics().Clear();
    REQUIRE(context.Sync());
    CHECK(RuleFileValue.Get() == 40.f);
    CHECK(CountCode(context, ConfigDiagnosticCode::ValueOutOfRange) == 1);
    for (int32 round = 0; round < 3; ++round) {
        context.Diagnostics().Clear();
        REQUIRE(
            context.ReplaceLayerText(ConfigLayer::Project, "Quality.json", "Quality", "project",
                                     R"({"quality.rule.file": {"0": {"render.rule.file": 400}}})"));
        REQUIRE(context.Sync());
        CHECK(CountCode(context, ConfigDiagnosticCode::ValueOutOfRange) == 1);
        CHECK(RuleFileValue.Get() == 40.f);
    }

    // 품질 묶음 층에는 코드도 쓸 수 없다.
    ConfigDiagnosticCode reason = ConfigDiagnosticCode::InvalidSetting;
    auto quality = RuleFileValue.Set(5.f, {.layer = ConfigLayer::Quality, .reason = &reason});
    CHECK(!quality && quality.Error() == ErrorCode::InvalidArgument);
    CHECK(reason == ConfigDiagnosticCode::LayerNotAllowed);
}
