#include "ConfigTestSupport.h"

#include <Config/QualityValidation.h>

// 설정 CF4 품질 조합 검증 시험이다(Docs/Design/ConfigSystem.ko.md 6.1절,
// Docs/Design/CVarSystem.ko.md 11절): 등급 요구가 거짓이면 정의된 바로 아래 등급으로
// 낮춘다(정의되지 않은 등급은 건너뛴다), 가장 낮은 등급도 맞지 않으면 그대로 두고 unmet, 예산은
// 우선순위가 낮은 묶음부터(같으면 이름 순서) 비용을 줄이는 한 칸씩 낮춘다, 맞출 수 없는 예산은
// 사유로 남는다, 기기 특성이 없거나 규칙이 없는 묶음은 요청 등급 그대로, 잘못된 요청은 거부,
// `quality.shadow@1 (requested 3, lowered: …)` 표기.
using namespace rocks;
using namespace rocks::config;

namespace {

// 시험 규칙 재료: shadow 등급 0·1·3(2는 정의되지 않음), 두 등급 묶음 0·1, 등급별 비용.
struct Rules {
    explicit Rules(Allocator& allocator) noexcept
        : bigTexture(allocator), lowestNeedsCompute(allocator) {
        CHECK(bigTexture.trait.TryAssign(DeviceTraitNames::GpuTextureSize2D));
        bigTexture.atLeast = 8192.0;
        CHECK(lowestNeedsCompute.trait.TryAssign(DeviceTraitNames::GpuComputeShaders));
        lowestNeedsCompute.atLeast = 1.0;
    }

    DeviceCondition bigTexture;
    DeviceCondition lowestNeedsCompute;
    const int32 shadowLevels[3] = {0, 1, 3};
    const int32 twoLevels[2] = {0, 1};
    const QualityCost shadowLow[1] = {{"gpu_memory", 100}};
    const QualityCost shadowMid[1] = {{"gpu_memory", 300}};
    const QualityCost shadowHigh[1] = {{"gpu_memory", 600}};
    const QualityCost effectsLow[1] = {{"gpu_memory", 50}};
    const QualityCost effectsHigh[1] = {{"gpu_memory", 400}};
    const QualityCost textureLow[1] = {{"gpu_memory", 10}};
    const QualityCost textureHigh[1] = {{"gpu_memory", 100}};
};

const QualityResolution* Find(const QualityResolutionSet& set, StringView group) noexcept {
    for (const UniquePtr<QualityResolution>& resolution : set.groups) {
        if (resolution->group.View() == group) {
            return resolution.Get();
        }
    }

    return nullptr;
}

} // namespace

// 요구(텍스처 한 변 ≥ 8192)가 거짓이면 3 → 1(2는 정의되지 않음)로 낮추고, 예산(메모리 1000 × 0.5 =
// 500)을 넘으면 우선순위가 낮은 effects부터 낮춘다. 결과는 요청 순서이고 사유와 표기가 남는다.
TEST(Config_QualityValidationLowersByRequirementsThenBudget) {
    SystemAllocator allocator;
    Rules rules(allocator);
    const QualityLevelRule shadowRules[] = {
        {0, {}, rules.shadowLow},
        {1, {}, rules.shadowMid},
        {3, Span<const DeviceCondition>(&rules.bigTexture, 1), rules.shadowHigh}};
    const QualityLevelRule effectsRules[] = {{0, {}, rules.effectsLow}, {1, {}, rules.effectsHigh}};
    const QualityGroupRule groups[] = {{"quality.shadow", rules.shadowLevels, 10, shadowRules},
                                       {"quality.effects", rules.twoLevels, 0, effectsRules}};
    const QualityBudgetRule budgets[] = {{"gpu_memory", DeviceTraitNames::GpuMemoryMiB, 0.5}};
    const QualityRequest requests[] = {{"quality.shadow", 3}, {"quality.effects", 1}};
    DeviceTraits traits(allocator);
    REQUIRE(traits.SetNumber(DeviceTraitNames::GpuTextureSize2D, 4096));
    REQUIRE(traits.SetNumber(DeviceTraitNames::GpuMemoryMiB, 1000));
    QualityResolutionSet resolved(allocator);
    REQUIRE(ResolveQualityLevels(groups, budgets, requests, &traits, resolved));
    REQUIRE(resolved.groups.Size() == 2);
    CHECK(resolved.groups[0]->group.View() == "quality.shadow");
    CHECK(resolved.groups[0]->requested == 3 && resolved.groups[0]->applied == 1);
    CHECK(resolved.groups[1]->requested == 1 && resolved.groups[1]->applied == 0);
    CHECK(resolved.unmetBudgets.IsEmpty());
    String text(allocator);
    REQUIRE(FormatQualityResolution(*resolved.groups[0], text));
    CHECK(text.View() ==
          "quality.shadow@1 (requested 3, lowered: gpu.texture_size_2d 4096 is below 8192)");
    REQUIRE(FormatQualityResolution(*resolved.groups[1], text));
    CHECK(text.View().StartsWith(
        "quality.effects@0 (requested 1, lowered: budget gpu_memory needs 700",
        StringComparison::CaseSensitive));

    // 특성이 없으면 검증하지 않는다.
    REQUIRE(ResolveQualityLevels(groups, budgets, requests, nullptr, resolved));
    CHECK(resolved.groups[0]->applied == 3 && resolved.groups[1]->applied == 1);
    REQUIRE(FormatQualityResolution(*resolved.groups[0], text));
    CHECK(text.View() == "quality.shadow@3");
}

// 예산 낮춤은 우선순위가 같으면 이름 순서(effects → texture)이고, 더 낮출 수 없으면 사유를 남기고
// 멈춘다. 가장 낮은 등급도 요구가 거짓이면 그 등급에 두고 unmet이다.
TEST(Config_QualityValidationBudgetOrderAndUnmet) {
    SystemAllocator allocator;
    Rules rules(allocator);
    const QualityLevelRule effectsRules[] = {{0, {}, rules.effectsLow}, {1, {}, rules.effectsHigh}};
    const QualityLevelRule textureRules[] = {
        {0, Span<const DeviceCondition>(&rules.lowestNeedsCompute, 1), rules.textureLow},
        {1, {}, rules.textureHigh}};
    const QualityGroupRule groups[] = {{"quality.texture", rules.twoLevels, 0, textureRules},
                                       {"quality.effects", rules.twoLevels, 0, effectsRules}};
    const QualityBudgetRule budgets[] = {{"gpu_memory", DeviceTraitNames::GpuMemoryMiB, 1.0},
                                         {"bandwidth", "gpu.bandwidth", 1.0}};
    const QualityRequest requests[] = {{"quality.texture", 1}, {"quality.effects", 1}};
    DeviceTraits traits(allocator);
    REQUIRE(traits.SetNumber(DeviceTraitNames::GpuMemoryMiB, 460));
    REQUIRE(traits.SetNumber(DeviceTraitNames::GpuComputeShaders, 0));
    QualityResolutionSet resolved(allocator);
    // 합 500 > 460: effects(이름이 앞선다)를 먼저 낮춘다(150 ≤ 460). 특성이 없는 bandwidth는 보지
    // 않는다.
    REQUIRE(ResolveQualityLevels(groups, budgets, requests, &traits, resolved));
    CHECK(Find(resolved, "quality.effects")->applied == 0);
    CHECK(Find(resolved, "quality.texture")->applied == 1);
    CHECK(resolved.unmetBudgets.IsEmpty());

    // 한도 40: effects 0(50)만으로도 넘는다 → texture도 0으로 낮추고(합 60) 더 낮출 수 없으면
    // 사유를 남긴다.
    REQUIRE(traits.SetNumber(DeviceTraitNames::GpuMemoryMiB, 40));
    REQUIRE(ResolveQualityLevels(groups, budgets, requests, &traits, resolved));
    CHECK(Find(resolved, "quality.effects")->applied == 0);
    CHECK(Find(resolved, "quality.texture")->applied == 0);
    REQUIRE(resolved.unmetBudgets.Size() == 1);
    CHECK(resolved.unmetBudgets[0].View().Contains("budget gpu_memory needs 60",
                                                   StringComparison::CaseSensitive));

    // 요청이 이미 가장 낮은 등급이고 그 요구가 거짓이면 unmet 표기다.
    const QualityRequest lowest[] = {{"quality.texture", 0}};
    REQUIRE(ResolveQualityLevels(groups, {}, lowest, &traits, resolved));
    CHECK(resolved.groups[0]->applied == 0 && resolved.groups[0]->unmet);
    String text(allocator);
    REQUIRE(FormatQualityResolution(*resolved.groups[0], text));
    CHECK(text.View().StartsWith("quality.texture@0 (requested 0, unmet: gpu.compute_shaders",
                                 StringComparison::CaseSensitive));
}

// 규칙이 없는 묶음은 요청 그대로다. 정의되지 않은 등급·같은 묶음 두 번은 거부한다(결과는 비운다).
TEST(Config_QualityValidationRejectsBadRequests) {
    SystemAllocator allocator;
    Rules rules(allocator);
    const QualityGroupRule groups[] = {{"quality.shadow", rules.shadowLevels, 0, {}}};
    DeviceTraits traits(allocator);
    QualityResolutionSet resolved(allocator);
    const QualityRequest unruled[] = {{"quality.unruled", 7}};
    REQUIRE(ResolveQualityLevels(groups, {}, unruled, &traits, resolved));
    CHECK(resolved.groups.Size() == 1 && resolved.groups[0]->applied == 7);

    const QualityRequest undefined[] = {{"quality.shadow", 2}};
    auto rejected = ResolveQualityLevels(groups, {}, undefined, &traits, resolved);
    CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument && resolved.groups.IsEmpty());
    const QualityRequest twice[] = {{"quality.shadow", 1}, {"quality.shadow", 3}};
    rejected = ResolveQualityLevels(groups, {}, twice, &traits, resolved);
    CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
}
