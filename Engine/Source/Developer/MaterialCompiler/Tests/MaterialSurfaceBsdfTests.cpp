#include <MaterialCompiler/MaterialSourceWriter.h>
#include <MaterialSchema/MaterialContextValue.h>
#include <MaterialSchema/Nodes/Surface/SurfaceEyeProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceEyeProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceSlabProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceSlabProperties.h>

#include "MaterialGraphFixture.h"

// MS2d2a 시험이다(Docs/Design/MaterialSystem 6.11절 MS2d2): Slab 기본값과 내보내기 식, 값으로
// 정하는 표지(상수면 shading model 하나, 값이면 합집합), Wrap SSS 계산 공간, 기능 충돌 경고, F90
// 굽기, glint 굽기, SimpleClearCoat·Hair·Eye·SingleLayerWater 칸, 반투명 coverage, profile 늦은
// 진단을 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_surface_bsdf_tests_file {

using namespace material_graph_fixture;

namespace {

uint32 ModelBit(rocks::ShadingModel model) noexcept {
    return 1u << static_cast<uint32>(model);
}

const IrResult* FindEntry(const MaterialTranslation& out, IrEntry entry, StringView slot) noexcept {
    for (const IrResult& result : out.module.Results()) {
        if (result.entry == entry && result.slot == slot) {
            return &result;
        }
    }

    return nullptr;
}

bool HasMessage(const MaterialTranslation& out, StringView message) noexcept {
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        if (diagnostic.message.View() == message) {
            return true;
        }
    }

    return false;
}

uint64 ContextIndex(StringView name) noexcept {
    const Span<const MaterialContextValue> table = MaterialContextValues();
    for (usize index = 0; index < table.Size(); ++index) {
        if (table[index].name == name) {
            return index;
        }
    }

    return table.Size();
}

// 결과가 float 상수면 첫 성분을, 아니면 −1을 돌려준다.
float ConstantResult(const MaterialTranslation& out, StringView slot) noexcept {
    const IrResult* result = FindEntry(out, IrEntry::Surface, slot);
    if (result == nullptr || ValueOf(out, result->value).op != IrOp::Constant) {
        return -1.0f;
    }

    return FloatOf(ValueOf(out, result->value), 0);
}

bool Near(float value, float expected) noexcept {
    const float difference = value - expected;
    return difference < 1e-5f && difference > -1e-5f;
}

Guid AddSlab(GraphFixture& fixture, SubsurfaceType type = SubsurfaceType::Diffusion) noexcept {
    SurfaceSlabProperties properties;
    properties.subSurfaceType = type;
    return fixture.Add("Material.SurfaceSlab", fixture.env.Save(properties));
}

} // namespace

// 기본 slab은 DefaultLit 하나이고, 내보낸 칸은 UE 식의 상수다(DiffuseAlbedo 0.18, F0 0.04 → 금속
// 0·specular 0.5 — 기본값 칸은 쓰지 않는다).
TEST(MaterialSurfaceBsdf_SlabDefaults) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid slab = AddSlab(fixture);
    fixture.Connect(slab, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(out.shadingModels == ModelBit(rocks::ShadingModel::DefaultLit));
    CHECK(Near(ConstantResult(out, "BaseColor"), 0.18f));
    CHECK(FindEntry(out, IrEntry::Surface, "Metallic") == nullptr);
    CHECK(FindEntry(out, IrEntry::Surface, "Specular") == nullptr);
    CHECK(FindEntry(out, IrEntry::Surface, "CustomData0") == nullptr);
    CHECK(FindEntry(out, IrEntry::Surface, "SubsurfaceColor") == nullptr);
    const IrResult* model = FindEntry(out, IrEntry::Surface, "ShadingModel");
    REQUIRE(model != nullptr);
    CHECK(ValueOf(out, model->value).op == IrOp::Constant);
    CHECK(ValueOf(out, model->value).immediate[0] ==
          static_cast<uint64>(rocks::ShadingModel::DefaultLit));
}

// 금속 F0: metal = saturate((max3(F0) − 0.08)/0.32), baseColor = lerp(DA, F0, metal).
TEST(MaterialSurfaceBsdf_SlabMetalExport) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid slab = AddSlab(fixture);
    fixture.Connect(AddConstant(fixture, 0.24f), "Result", slab, "F0");
    fixture.Connect(slab, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(Near(ConstantResult(out, "Metallic"), 0.5f));
    CHECK(Near(ConstantResult(out, "Specular"), 1.0f));
    const IrResult* base = FindEntry(out, IrEntry::Surface, "BaseColor");
    REQUIRE(base != nullptr);
    const IrValue& lerp = ValueOf(out, ValueOf(out, base->value).operands[0]);
    CHECK(lerp.op == IrOp::Lerp);
    CHECK(Near(FloatOf(ValueOf(out, lerp.operands[2]), 0), 0.5f));
}

// 상수 fuzz는 Cloth 하나(customData0 = FuzzAmount, subsurfaceColor = FuzzColor), 값 fuzz는 픽셀마다
// 골라 DefaultLit·Cloth 합집합이다.
TEST(MaterialSurfaceBsdf_FuzzFlag) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid slab = AddSlab(fixture);
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", slab, "FuzzAmount");
    fixture.Connect(AddConstant(fixture, 0.25f), "Result", slab, "FuzzColor");
    fixture.Connect(slab, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(out.shadingModels == ModelBit(rocks::ShadingModel::Cloth));
    CHECK(Near(ConstantResult(out, "CustomData0"), 0.5f));
    CHECK(Near(ConstantResult(out, "SubsurfaceColor"), 0.25f));

    GraphFixture varying(env);
    const Guid dynamic = AddSlab(varying);
    varying.Connect(AddVarying(varying, 0.5f), "Result", dynamic, "FuzzAmount");
    varying.Connect(dynamic, "Result", OutputNodeId, "Surface");
    MaterialTranslation mixed(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Translate(varying, Traits(), mixed));
    REQUIRE(mixed.succeeded);
    CHECK(mixed.shadingModels ==
          (ModelBit(rocks::ShadingModel::DefaultLit) | ModelBit(rocks::ShadingModel::Cloth)));
    REQUIRE(
        WriteMaterialSource(env.allocator, mixed.module, Traits(), TestAssetId, MainGraphId, source)
            .IsOk());
    REQUIRE(source.succeeded);
    const StringView text = source.text.View();
    bool single = false;
    const StringView define = "MATERIAL_SINGLE_SHADING_MODEL";
    for (usize at = 0; at + define.ByteCount() <= text.ByteCount(); ++at) {
        single = single || StringView(text.Data() + at, define.ByteCount()) == define;
    }

    CHECK(!single);
}

// Wrap SSS: MFP를 계산 공간(× 100/0.01)으로 바꿔 subsurfaceColor = exp(−1/(MFP·0.01)), opacity =
// 1 − PhaseAnisotropy, shading model Subsurface다. subSurfaceType None이면 SSS가 없다.
TEST(MaterialSurfaceBsdf_WrapSubsurface) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid slab = AddSlab(fixture, SubsurfaceType::Wrap);
    fixture.Connect(AddConstant(fixture, 0.01f), "Result", slab, "SSSMFP");
    fixture.Connect(AddConstant(fixture, 0.25f), "Result", slab, "SSSPhaseAnisotropy");
    fixture.Connect(slab, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(out.shadingModels == ModelBit(rocks::ShadingModel::Subsurface));
    // MFP 0.01 cm → 계산 공간 100 cm → exp(−1/1)(exp는 상수로 접지 않는다).
    const IrResult* color = FindEntry(out, IrEntry::Surface, "SubsurfaceColor");
    REQUIRE(color != nullptr);
    const IrValue& transmitted = ValueOf(out, ValueOf(out, color->value).operands[0]);
    CHECK(transmitted.op == IrOp::Exponential);
    CHECK(Near(FloatOf(ValueOf(out, transmitted.operands[0]), 0), -1.0f));
    CHECK(Near(ConstantResult(out, "SurfaceOpacity"), 0.75f));

    GraphFixture foliage(env);
    const Guid twoSided = AddSlab(foliage, SubsurfaceType::TwoSidedWrap);
    foliage.Connect(AddConstant(foliage, 0.01f), "Result", twoSided, "SSSMFP");
    foliage.Connect(twoSided, "Result", OutputNodeId, "Surface");
    MaterialTranslation foliageOut(env.allocator);
    REQUIRE(Translate(foliage, Traits(), foliageOut));
    CHECK(foliageOut.shadingModels == ModelBit(rocks::ShadingModel::TwoSidedFoliage));

    GraphFixture none(env);
    const Guid plain = AddSlab(none, SubsurfaceType::None);
    none.Connect(AddConstant(none, 0.01f), "Result", plain, "SSSMFP");
    none.Connect(plain, "Result", OutputNodeId, "Surface");
    MaterialTranslation noneOut(env.allocator);
    REQUIRE(Translate(none, Traits(), noneOut));
    CHECK(noneOut.shadingModels == ModelBit(rocks::ShadingModel::DefaultLit));

    // Diffusion(MFP만)은 UE Blendable처럼 DefaultLit다.
    GraphFixture diffusion(env);
    const Guid diffused = AddSlab(diffusion);
    diffusion.Connect(AddConstant(diffusion, 0.01f), "Result", diffused, "SSSMFP");
    diffusion.Connect(diffused, "Result", OutputNodeId, "Surface");
    MaterialTranslation diffusionOut(env.allocator);
    REQUIRE(Translate(diffusion, Traits(), diffusionOut));
    CHECK(diffusionOut.shadingModels == ModelBit(rocks::ShadingModel::DefaultLit));
}

// 상수 참 조건 둘(Wrap과 fuzz)은 우선순위(Subsurface)로 고르고 경고한다(10.3절).
TEST(MaterialSurfaceBsdf_FeatureConflictWarning) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid slab = AddSlab(fixture, SubsurfaceType::Wrap);
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", slab, "SSSMFP");
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", slab, "FuzzAmount");
    fixture.Connect(slab, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(out.shadingModels == ModelBit(rocks::ShadingModel::Subsurface));
    CHECK(CountDiagnostics(out, MaterialDiagnosticCode::UnsupportedCapability,
                           nodegraph::GraphDiagnosticSeverity::Warning) == 1);
    CHECK(HasMessage(out, "Surface combines features of several shading models; lower-priority "
                          "features are ignored"));
}

// 연결된 F90(< 1)은 정점 노멀·CameraVector의 NoV로 F0에 굽는다(baseColor가 시야에 따라 바뀐다).
TEST(MaterialSurfaceBsdf_F90Bake) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid slab = AddSlab(fixture);
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", slab, "F90");
    fixture.Connect(AddConstant(fixture, 0.6f), "Result", slab, "F0");
    fixture.Connect(AddConstant(fixture, 0.1f), "Result", slab, "Roughness");
    fixture.Connect(slab, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const IrResult* base = FindEntry(out, IrEntry::Surface, "BaseColor");
    REQUIRE(base != nullptr);
    CHECK(ValueOf(out, base->value).op != IrOp::Constant);
    bool camera = false;
    for (const IrValue& value : out.module.Values()) {
        camera = camera ||
                 (value.op == IrOp::Extern && value.immediate[0] == ContextIndex("CameraVector"));
    }

    CHECK(camera);
    CHECK(out.shadingModels == ModelBit(rocks::ShadingModel::DefaultLit));
}

// glint는 F0에 saturate(GlintValue)로 굽는다(Rocks에 glint가 없다 — UE 미지원 경로).
TEST(MaterialSurfaceBsdf_GlintBake) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid slab = AddSlab(fixture);
    fixture.Connect(AddConstant(fixture, 0.24f), "Result", slab, "F0");
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", slab, "GlintValue");
    fixture.Connect(slab, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    // F0 0.12 → metal 0.125, specular 1.
    CHECK(Near(ConstantResult(out, "Metallic"), 0.125f));
}

// SimpleClearCoat: ClearCoat 하나, customData0 = coverage, customData1 = clear coat 거칠기,
// 거칠기는 굽지 않는다.
TEST(MaterialSurfaceBsdf_SimpleClearCoat) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid coat = fixture.Add("Material.SurfaceSimpleClearCoat");
    fixture.Connect(AddConstant(fixture, 0.6f), "Result", coat, "ClearCoatCoverage");
    fixture.Connect(AddConstant(fixture, 0.3f), "Result", coat, "ClearCoatRoughness");
    fixture.Connect(AddConstant(fixture, 0.7f), "Result", coat, "Roughness");
    fixture.Connect(coat, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(out.shadingModels == ModelBit(rocks::ShadingModel::ClearCoat));
    CHECK(Near(ConstantResult(out, "CustomData0"), 0.6f));
    CHECK(Near(ConstantResult(out, "CustomData1"), 0.3f));
    CHECK(Near(ConstantResult(out, "Roughness"), 0.7f));
}

// slab의 non-simple haze(SecondRoughness)는 거칠기에 굽는다(lerp(R, R2, W)).
TEST(MaterialSurfaceBsdf_HazeBake) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid slab = AddSlab(fixture);
    fixture.Connect(AddConstant(fixture, 0.2f), "Result", slab, "Roughness");
    fixture.Connect(AddConstant(fixture, 0.6f), "Result", slab, "SecondRoughness");
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", slab, "SecondRoughnessWeight");
    fixture.Connect(slab, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(out.shadingModels == ModelBit(rocks::ShadingModel::DefaultLit));
    const IrResult* roughness = FindEntry(out, IrEntry::Surface, "Roughness");
    REQUIRE(roughness != nullptr);
    const IrValue& baked = ValueOf(out, ValueOf(out, roughness->value).operands[0]);
    CHECK(baked.op == IrOp::Lerp);
    CHECK(Near(FloatOf(ValueOf(out, baked.operands[1]), 0), 0.6f));
}

// Hair·Eye·SingleLayerWater 칸이다.
TEST(MaterialSurfaceBsdf_DedicatedBsdfs) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture hair(env);
    const Guid hairNode = hair.Add("Material.SurfaceHair");
    hair.Connect(AddConstant(hair, 0.3f), "Result", hairNode, "Scatter");
    hair.Connect(AddConstant(hair, 0.4f), "Result", hairNode, "Backlit");
    hair.Connect(hairNode, "Result", OutputNodeId, "Surface");
    MaterialTranslation hairOut(env.allocator);
    REQUIRE(Translate(hair, Traits(), hairOut));
    REQUIRE(hairOut.succeeded);
    CHECK(hairOut.shadingModels == ModelBit(rocks::ShadingModel::Hair));
    CHECK(Near(ConstantResult(hairOut, "Metallic"), 0.3f));
    CHECK(Near(ConstantResult(hairOut, "CustomData0"), 0.4f));
    const IrResult* hairNormal = FindEntry(hairOut, IrEntry::Normal, "Normal");
    REQUIRE(hairNormal != nullptr);
    CHECK(ValueOf(hairOut, hairNormal->value).immediate[0] == ContextIndex("VertexTangent"));

    GraphFixture eye(env);
    SurfaceEyeProperties eyeProperties;
    const Guid eyeNode = eye.Add("Material.SurfaceEye", env.Save(eyeProperties));
    eye.Connect(AddConstant(eye, 0.8f), "Result", eyeNode, "IrisMask");
    eye.Connect(eyeNode, "Result", OutputNodeId, "Surface");
    MaterialTranslation eyeOut(env.allocator);
    REQUIRE(Translate(eye, Traits(), eyeOut));
    REQUIRE(eyeOut.succeeded);
    CHECK(eyeOut.shadingModels == ModelBit(rocks::ShadingModel::Eye));
    CHECK(Near(ConstantResult(eyeOut, "Specular"), 0.35f));
    CHECK(Near(ConstantResult(eyeOut, "CustomData0"), 0.8f));

    GraphFixture water(env);
    const Guid waterNode = water.Add("Material.SurfaceSingleLayerWater");
    water.Connect(AddConstant(water, 0.9f), "Result", waterNode, "Metallic");
    water.Connect(AddConstant(water, 0.3f), "Result", waterNode, "TopMaterialOpacity");
    water.Connect(waterNode, "Result", OutputNodeId, "Surface");
    MaterialTranslation waterOut(env.allocator);
    REQUIRE(Translate(water, Traits(), waterOut));
    REQUIRE(waterOut.succeeded);
    CHECK(waterOut.shadingModels == ModelBit(rocks::ShadingModel::SingleLayerWater));
    CHECK(Near(ConstantResult(waterOut, "Metallic"), 0.0f));
    CHECK(Near(ConstantResult(waterOut, "SurfaceOpacity"), 0.3f));
}

// 반투명 slab은 coverage(1)가 Opacity 진입점이다.
TEST(MaterialSurfaceBsdf_TranslucentCoverage) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid slab = AddSlab(fixture);
    fixture.Connect(slab, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(BlendMode::Translucent), out));
    REQUIRE(out.succeeded);
    const IrResult* opacity = FindEntry(out, IrEntry::Opacity, "Opacity");
    REQUIRE(opacity != nullptr);
    CHECK(Near(FloatOf(ValueOf(out, opacity->value), 0), 1.0f));
}

// thinSurface 속성의 바닥 slab에 MFP가 있으면 ThinTranslucent다.
TEST(MaterialSurfaceBsdf_ThinSurface) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid slab = AddSlab(fixture, SubsurfaceType::SimpleVolume);
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", slab, "SSSMFP");
    fixture.Connect(slab, "Result", OutputNodeId, "Surface");
    MaterialTraitsDesc traits = Traits(BlendMode::Translucent);
    traits.thinSurface = true;
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, traits, out));
    REQUIRE(out.succeeded);
    CHECK(out.shadingModels == ModelBit(rocks::ShadingModel::ThinTranslucent));
}

// 값이 있는 profile 속성(subsurface·specular)과 Eye profile은 그 노드의 늦은 진단이다.
TEST(MaterialSurfaceBsdf_ProfilesPending) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const AssetId profile{Guid{0x50524f46ull, 2}};
    GraphFixture subsurface(env);
    SurfaceSlabProperties withSubsurface;
    withSubsurface.subsurfaceProfile.assetId = profile;
    const Guid first = subsurface.Add("Material.SurfaceSlab", env.Save(withSubsurface));
    subsurface.Connect(first, "Result", OutputNodeId, "Surface");
    MaterialTranslation firstOut(env.allocator);
    REQUIRE(Translate(subsurface, Traits(), firstOut));
    CHECK(HasOnlyErrorAt(firstOut, MaterialDiagnosticCode::PrerequisiteMissing, first));

    GraphFixture specular(env);
    SurfaceSlabProperties withSpecular;
    withSpecular.specularProfile.assetId = profile;
    const Guid second = specular.Add("Material.SurfaceSlab", env.Save(withSpecular));
    specular.Connect(second, "Result", OutputNodeId, "Surface");
    MaterialTranslation secondOut(env.allocator);
    REQUIRE(Translate(specular, Traits(), secondOut));
    CHECK(HasOnlyErrorAt(secondOut, MaterialDiagnosticCode::PrerequisiteMissing, second));

    GraphFixture eye(env);
    SurfaceEyeProperties withEye;
    withEye.subsurfaceProfile.assetId = profile;
    const Guid third = eye.Add("Material.SurfaceEye", env.Save(withEye));
    eye.Connect(third, "Result", OutputNodeId, "Surface");
    MaterialTranslation thirdOut(env.allocator);
    REQUIRE(Translate(eye, Traits(), thirdOut));
    CHECK(HasOnlyErrorAt(thirdOut, MaterialDiagnosticCode::PrerequisiteMissing, third));
}

} // namespace material_surface_bsdf_tests_file
