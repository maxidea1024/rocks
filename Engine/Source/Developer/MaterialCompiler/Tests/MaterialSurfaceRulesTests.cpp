#include <MaterialCompiler/MaterialSourceWriter.h>
#include <MaterialSchema/Nodes/MaterialAttributes/ShadingModelProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/ShadingModelProperties.h>
#include <MaterialSchema/Nodes/Outputs/VolumetricAdvancedOutputProperties.generated.h>
#include <MaterialSchema/Nodes/Outputs/VolumetricAdvancedOutputProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceEyeProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceEyeProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceFromShadingModelProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceFromShadingModelProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceSelectProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceSelectProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceSlabProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceSlabProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceVolumetricFogCloudProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceVolumetricFogCloudProperties.h>

#include "MaterialGraphFixture.h"

// MS2d 리뷰 반영 시험이다(Docs/Design/MaterialSystem 6.11절 개정 28): 내보내기의 haze → F90 순서,
// VolumetricFogCloud Extinction 원값, Eye IrisPlaneNormal → TangentOutput, Wrap opacity의 abs,
// Profile 문턱 미만 DefaultLit, 불투명 thin은 DefaultLit, 연산 결과값(Horizontal 무게·Add 합·Select
// 같음), 연산 아래 고전 preset의 정확한 shading model 집합과 동적 shading model, custom 출력 핀
// 사용자 값, Volume 영역 정의·AO 연결 기준, RVT 속성 마스크를 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_surface_rules_tests_file {

using namespace material_graph_fixture;

namespace {

uint32 RulesBit(rocks::ShadingModel model) noexcept {
    return 1u << static_cast<uint32>(model);
}

const IrResult* RulesEntry(const MaterialTranslation& out, IrEntry entry,
                           StringView slot) noexcept {
    for (const IrResult& result : out.module.Results()) {
        if (result.entry == entry && result.slot == slot) {
            return &result;
        }
    }

    return nullptr;
}

// 결과가 float 상수면 첫 성분을, 아니면 −1이다.
float RulesConstant(const MaterialTranslation& out, IrEntry entry, StringView slot) noexcept {
    const IrResult* result = RulesEntry(out, entry, slot);
    if (result == nullptr || ValueOf(out, result->value).op != IrOp::Constant) {
        return -1.0f;
    }

    return FloatOf(ValueOf(out, result->value), 0);
}

bool RulesNear(float value, float expected) noexcept {
    const float difference = value - expected;
    return difference < 1e-5f && difference > -1e-5f;
}

// root에서 닿는 값에 target이 있는가.
bool RulesReaches(const MaterialTranslation& out, IrValueId root, IrValueId target) noexcept {
    const Span<const IrValue> values = out.module.Values();
    SystemAllocator scratch;
    Array<IrValueId> stack(scratch);
    Array<bool> seen(scratch);
    if (!seen.TryResize(values.Size()) || !stack.TryPushBack(root)) {
        return false;
    }

    for (bool& item : seen) {
        item = false;
    }

    while (!stack.IsEmpty()) {
        const IrValueId id = stack[stack.Size() - 1];
        (void)stack.TryPopBack();
        if (id == target) {
            return true;
        }

        if (seen[id]) {
            continue;
        }

        seen[id] = true;
        for (uint8 operand = 0; operand < values[id].operandCount; ++operand) {
            if (!stack.TryPushBack(values[id].operands[operand])) {
                return false;
            }
        }
    }

    return false;
}

bool RulesText(StringView text, StringView part) noexcept {
    for (usize at = 0; at + part.ByteCount() <= text.ByteCount(); ++at) {
        if (StringView(text.Data() + at, part.ByteCount()) == part) {
            return true;
        }
    }

    return false;
}

Guid RulesSlab(GraphFixture& fixture, SubsurfaceType type = SubsurfaceType::Diffusion) noexcept {
    SurfaceSlabProperties properties;
    properties.subSurfaceType = type;
    return fixture.Add("Material.SurfaceSlab", fixture.env.Save(properties));
}

Guid RulesPreset(GraphFixture& fixture, ShadingModelPreset model) noexcept {
    SurfaceFromShadingModelProperties properties;
    properties.shadingModelOverride = model;
    return fixture.Add("Material.SurfaceFromShadingModel", fixture.env.Save(properties));
}

// child를 Weight 하나 아래에 두고 출력에 잇는다.
void RulesWeighted(GraphFixture& fixture, Guid child) noexcept {
    const Guid weight = fixture.Add("Material.SurfaceWeight");
    fixture.Connect(child, "Result", weight, "A");
    fixture.Connect(weight, "Result", OutputNodeId, "Surface");
}

} // namespace

// 내보내기는 haze를 먼저 거칠기에 굽고 그 거칠기로 F90을 굽는다(SE:1105–1109): baseColor 식이
// haze를 굽은 거칠기 값에 닿는다.
TEST(MaterialSurfaceRules_HazeBeforeF90) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid slab = RulesSlab(fixture);
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", slab, "F90");
    fixture.Connect(AddConstant(fixture, 0.6f), "Result", slab, "F0");
    fixture.Connect(AddConstant(fixture, 0.2f), "Result", slab, "Roughness");
    fixture.Connect(AddConstant(fixture, 0.6f), "Result", slab, "SecondRoughness");
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", slab, "SecondRoughnessWeight");
    fixture.Connect(slab, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const IrResult* roughness = RulesEntry(out, IrEntry::Surface, "Roughness");
    const IrResult* base = RulesEntry(out, IrEntry::Surface, "BaseColor");
    REQUIRE(roughness != nullptr);
    REQUIRE(base != nullptr);
    const IrValueId baked = ValueOf(out, roughness->value).operands[0];
    // haze 표지가 값이라 굽은 거칠기는 Select다.
    CHECK(ValueOf(out, baked).op == IrOp::Select);
    CHECK(RulesReaches(out, base->value, baked));
}

// VolumetricFogCloud의 Extinction은 saturate하지 않는다(UE 볼륨 경로는 원값).
TEST(MaterialSurfaceRules_ExtinctionRaw) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    SurfaceVolumetricFogCloudProperties properties;
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.SurfaceVolumetricFogCloud", env.Save(properties));
    fixture.Connect(AddConstant(fixture, 2.5f), "Result", node, "Extinction");
    fixture.Connect(node, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(BlendMode::Additive, MaterialDomain::Volume), out));
    REQUIRE(out.succeeded);
    CHECK(RulesNear(RulesConstant(out, IrEntry::Surface, "SubsurfaceColor"), 2.5f));
}

// Eye의 IrisPlaneNormal은 TangentOutput 출력이고 tangent 칸은 쓰지 않는다.
TEST(MaterialSurfaceRules_EyePlaneNormal) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    SurfaceEyeProperties properties;
    const Guid eye = fixture.Add("Material.SurfaceEye", env.Save(properties));
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", eye, "IrisPlaneNormal");
    fixture.Connect(eye, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(RulesEntry(out, IrEntry::CustomOutput, "TangentOutput0") != nullptr);
    const IrResult* tangent = RulesEntry(out, IrEntry::Surface, "Tangent");
    REQUIRE(tangent != nullptr);
    CHECK(ValueOf(out, tangent->value).op == IrOp::Extern);
}

// Wrap의 opacity는 1 − abs(위상 비등방)이다.
TEST(MaterialSurfaceRules_WrapOpacityAbs) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid slab = RulesSlab(fixture, SubsurfaceType::Wrap);
    fixture.Connect(AddConstant(fixture, 0.01f), "Result", slab, "SSSMFP");
    fixture.Connect(AddConstant(fixture, -0.5f), "Result", slab, "SSSPhaseAnisotropy");
    fixture.Connect(slab, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(RulesNear(RulesConstant(out, IrEntry::Surface, "SurfaceOpacity"), 0.5f));
}

// Profile 병합: 반경 배율이 문턱 위면 SubsurfaceProfile, 문턱 아래면 사슬을 내려가지 않고
// DefaultLit다.
TEST(MaterialSurfaceRules_ProfileThreshold) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const float mixes[] = {0.5f, 0.95f};
    const uint32 expected[] = {RulesBit(rocks::ShadingModel::SubsurfaceProfile),
                               RulesBit(rocks::ShadingModel::DefaultLit)};
    for (usize index = 0; index < 2; ++index) {
        GraphFixture fixture(env);
        const Guid mix = fixture.Add("Material.SurfaceHorizontalMix");
        fixture.Connect(RulesPreset(fixture, ShadingModelPreset::SubsurfaceProfile), "Result", mix,
                        "Background");
        fixture.Connect(RulesPreset(fixture, ShadingModelPreset::DefaultLit), "Result", mix,
                        "Foreground");
        fixture.Connect(AddConstant(fixture, mixes[index]), "Result", mix, "Mix");
        fixture.Connect(mix, "Result", OutputNodeId, "Surface");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        CHECK(out.shadingModels == expected[index]);
    }
}

// thin slab은 반투명일 때만 ThinTranslucent이고 불투명이면 DefaultLit다.
TEST(MaterialSurfaceRules_ThinOpaque) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid slab = RulesSlab(fixture, SubsurfaceType::SimpleVolume);
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", slab, "SSSMFP");
    fixture.Connect(slab, "Result", OutputNodeId, "Surface");
    MaterialTraitsDesc traits = Traits();
    traits.thinSurface = true;
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, traits, out));
    REQUIRE(out.succeeded);
    CHECK(out.shadingModels == RulesBit(rocks::ShadingModel::DefaultLit));
}

// 연산 결과값: Horizontal 무게(Mix 0.25 → 0.25), Add 합(0.3 + 0.4), Select 같은 값은 A다.
TEST(MaterialSurfaceRules_OperatorValues) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture horizontal(env);
    const Guid mix = horizontal.Add("Material.SurfaceHorizontalMix");
    const Guid background = RulesSlab(horizontal);
    const Guid foreground = RulesSlab(horizontal);
    horizontal.Connect(AddConstant(horizontal, 0.8f), "Result", foreground, "DiffuseAlbedo");
    horizontal.Connect(background, "Result", mix, "Background");
    horizontal.Connect(foreground, "Result", mix, "Foreground");
    horizontal.Connect(AddConstant(horizontal, 0.25f), "Result", mix, "Mix");
    horizontal.Connect(mix, "Result", OutputNodeId, "Surface");
    MaterialTranslation horizontalOut(env.allocator);
    REQUIRE(Translate(horizontal, Traits(), horizontalOut));
    REQUIRE(horizontalOut.succeeded);
    // w = 0.25(coverage 1)이므로 lerp(0.18, 0.8, 0.25)다.
    CHECK(RulesNear(RulesConstant(horizontalOut, IrEntry::Surface, "BaseColor"),
                    0.18f + (0.8f - 0.18f) * 0.25f));

    GraphFixture add(env);
    const Guid adder = add.Add("Material.SurfaceAdd");
    const Guid a = RulesSlab(add);
    const Guid b = RulesSlab(add);
    add.Connect(AddConstant(add, 0.3f), "Result", a, "DiffuseAlbedo");
    add.Connect(AddConstant(add, 0.4f), "Result", b, "DiffuseAlbedo");
    add.Connect(AddConstant(add, 0.0f), "Result", a, "F0");
    add.Connect(AddConstant(add, 0.0f), "Result", b, "F0");
    add.Connect(a, "Result", adder, "A");
    add.Connect(b, "Result", adder, "B");
    add.Connect(adder, "Result", OutputNodeId, "Surface");
    MaterialTranslation addOut(env.allocator);
    REQUIRE(Translate(add, Traits(), addOut));
    REQUIRE(addOut.succeeded);
    CHECK(RulesNear(RulesConstant(addOut, IrEntry::Surface, "BaseColor"), 0.7f));

    GraphFixture select(env);
    SurfaceSelectProperties properties;
    const Guid chooser = select.Add("Material.SurfaceSelect", env.Save(properties));
    const Guid first = RulesSlab(select);
    const Guid second = RulesSlab(select);
    select.Connect(AddConstant(select, 0.25f), "Result", first, "DiffuseAlbedo");
    select.Connect(AddConstant(select, 0.75f), "Result", second, "DiffuseAlbedo");
    select.Connect(first, "Result", chooser, "A");
    select.Connect(second, "Result", chooser, "B");
    select.Connect(AddConstant(select, 0.5f), "Result", chooser, "SelectValue");
    select.Connect(chooser, "Result", OutputNodeId, "Surface");
    MaterialTranslation selectOut(env.allocator);
    REQUIRE(Translate(select, Traits(), selectOut));
    REQUIRE(selectOut.succeeded);
    CHECK(RulesNear(RulesConstant(selectOut, IrEntry::Surface, "BaseColor"), 0.25f));
}

// 연산 아래 고전 preset의 shading model 집합은 정확히 그 모델이다.
TEST(MaterialSurfaceRules_ClassicExactSets) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    struct Case {
        ShadingModelPreset preset;
        uint32 models;
    };

    const Case cases[] = {
        {ShadingModelPreset::DefaultLit, RulesBit(rocks::ShadingModel::DefaultLit)},
        {ShadingModelPreset::Cloth, RulesBit(rocks::ShadingModel::Cloth)},
        {ShadingModelPreset::ClearCoat, RulesBit(rocks::ShadingModel::ClearCoat)},
        {ShadingModelPreset::SubsurfaceProfile, RulesBit(rocks::ShadingModel::SubsurfaceProfile)},
    };
    for (const Case& item : cases) {
        GraphFixture fixture(env);
        RulesWeighted(fixture, RulesPreset(fixture, item.preset));
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        CHECK(out.shadingModels == item.models);
    }
}

// 연산 아래 고전 preset의 ShadingModel 입력이 픽셀마다 갈리면 model == k Select로 고른다.
TEST(MaterialSurfaceRules_ClassicDynamicModel) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    ShadingModelProperties coat;
    coat.shadingModel = ShadingModelPreset::ClearCoat;
    ShadingModelProperties cloth;
    cloth.shadingModel = ShadingModelPreset::Cloth;
    const Guid pass = fixture.Add("Material.ShadowPassSwitch");
    fixture.Connect(fixture.Add("Material.ShadingModel", env.Save(coat)), "Result", pass,
                    "Default");
    fixture.Connect(fixture.Add("Material.ShadingModel", env.Save(cloth)), "Result", pass,
                    "Shadow");
    const Guid preset = RulesPreset(fixture, ShadingModelPreset::DefaultLit);
    fixture.Connect(pass, "Result", preset, "ShadingModel");
    RulesWeighted(fixture, preset);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    // 두 후보가 다 들어가고, 그 밖은 내보내기 사슬 끝의 DefaultLit뿐이다(static으로 닿는 가지 —
    // 픽셀에서는 쓰이지 않는다).
    const uint32 candidates =
        RulesBit(rocks::ShadingModel::ClearCoat) | RulesBit(rocks::ShadingModel::Cloth);
    CHECK((out.shadingModels & candidates) == candidates);
    CHECK((out.shadingModels & ~(candidates | RulesBit(rocks::ShadingModel::DefaultLit))) == 0);
}

// custom 출력은 연결이 없을 때 핀 사용자 값을 쓴다(UE Const* 속성).
TEST(MaterialSurfaceRules_CustomOutputPinValue) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    VolumetricAdvancedOutputProperties properties;
    const Guid node = fixture.Add("Material.VolumetricAdvancedOutput", env.Save(properties));
    fixture.SetValue(node, "PhaseG", PinValue(0.8f, 1));
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(BlendMode::Additive, MaterialDomain::Volume), out));
    REQUIRE(out.succeeded);
    CHECK(RulesNear(RulesConstant(out, IrEntry::CustomOutput, "VolumetricAdvancedMaterialOutput0"),
                    0.8f));
}

// Volume 정의는 Volume 영역에서만이고, AO 재정의는 VolumetricFogCloud AO 핀 연결을 본다.
TEST(MaterialSurfaceRules_VolumetricDefines) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    VolumetricAdvancedOutputProperties properties;
    GraphFixture surface(env);
    surface.Add("Material.VolumetricAdvancedOutput", env.Save(properties));
    MaterialTranslation surfaceOut(env.allocator);
    MaterialGeneratedSource surfaceSource(env.allocator);
    REQUIRE(Translate(surface, Traits(), surfaceOut));
    REQUIRE(surfaceOut.succeeded);
    REQUIRE(WriteMaterialSource(env.allocator, surfaceOut.module, Traits(), TestAssetId,
                                MainGraphId, surfaceSource)
                .IsOk());
    CHECK(!RulesText(surfaceSource.text.View(), "MATERIAL_VOLUMETRIC_ADVANCED "));

    const MaterialTraitsDesc volume = Traits(BlendMode::Additive, MaterialDomain::Volume);
    const bool connections[] = {false, true};
    const StringView lines[] = {
        "#define MATERIAL_VOLUMETRIC_ADVANCED_OVERRIDE_AMBIENT_OCCLUSION 0\n",
        "#define MATERIAL_VOLUMETRIC_ADVANCED_OVERRIDE_AMBIENT_OCCLUSION 1\n"};
    for (usize index = 0; index < 2; ++index) {
        GraphFixture fixture(env);
        fixture.Add("Material.VolumetricAdvancedOutput", env.Save(properties));
        SurfaceVolumetricFogCloudProperties cloud;
        const Guid fog = fixture.Add("Material.SurfaceVolumetricFogCloud", env.Save(cloud));
        if (connections[index]) {
            fixture.Connect(AddConstant(fixture, 0.5f), "Result", fog, "AmbientOcclusion");
        }

        fixture.Connect(fog, "Result", OutputNodeId, "Surface");
        MaterialTranslation out(env.allocator);
        MaterialGeneratedSource source(env.allocator);
        REQUIRE(Translate(fixture, volume, out));
        REQUIRE(out.succeeded);
        REQUIRE(
            WriteMaterialSource(env.allocator, out.module, volume, TestAssetId, MainGraphId, source)
                .IsOk());
        CHECK(RulesText(source.text.View(), lines[index]));
    }
}

// RVT 속성 마스크는 연결된 입력의 UE 속성 비트다(BaseColor 0, Mask 4).
TEST(MaterialSurfaceRules_VirtualTextureMask) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid rvt = fixture.Add("Material.RuntimeVirtualTextureOutput");
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", rvt, "BaseColor");
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", rvt, "Mask");
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", rvt, "Opacity");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(out.virtualTextureOutputMask == ((1u << 0) | (1u << 4)));
}

} // namespace material_surface_rules_tests_file
