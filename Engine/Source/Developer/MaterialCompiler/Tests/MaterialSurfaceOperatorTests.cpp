#include <MaterialSchema/MaterialContextValue.h>
#include <MaterialSchema/Nodes/Surface/SurfaceFromShadingModelProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceFromShadingModelProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceSelectProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceSelectProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceSlabProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceSlabProperties.h>

#include "MaterialGraphFixture.h"

// MS2d2b·MS2d2c 시험이다(Docs/Design/MaterialSystem 6.11절 MS2d2): 연산 접기(Horizontal·Vertical·
// Add·Weight·Select)의 결과 shading model·노멀 섞기, 허용 표와 UE 문구(Decal
// 뿌리·SingleLayerWater), 뿌리 Select의 칸 선택(Hair 섞임), Weight 아래 Unlit, 연산 아래 고전
// preset의 slab 변환, 도우미
// 넷(TransmittanceToMeanFreePath·MetalnessToDiffuseAlbedoF0·HazinessToSecondaryRoughness·ThinFilm)을
// 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_surface_operator_tests_file {

using namespace material_graph_fixture;

namespace {

uint32 OperatorModelBit(rocks::ShadingModel model) noexcept {
    return 1u << static_cast<uint32>(model);
}

const IrResult* OperatorEntry(const MaterialTranslation& out, IrEntry entry,
                              StringView slot) noexcept {
    for (const IrResult& result : out.module.Results()) {
        if (result.entry == entry && result.slot == slot) {
            return &result;
        }
    }

    return nullptr;
}

bool OperatorMessage(const MaterialTranslation& out, StringView message) noexcept {
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        if (diagnostic.message.View() == message) {
            return true;
        }
    }

    return false;
}

bool OperatorUses(const MaterialTranslation& out, StringView name) noexcept {
    const Span<const MaterialContextValue> table = MaterialContextValues();
    for (const IrValue& value : out.module.Values()) {
        if (value.op == IrOp::Extern && value.immediate[0] < table.Size() &&
            table[static_cast<usize>(value.immediate[0])].name == name) {
            return true;
        }
    }

    return false;
}

float OperatorConstant(const MaterialTranslation& out, StringView slot) noexcept {
    const IrResult* result = OperatorEntry(out, IrEntry::Surface, slot);
    if (result == nullptr || ValueOf(out, result->value).op != IrOp::Constant) {
        return -1.0f;
    }

    return FloatOf(ValueOf(out, result->value), 0);
}

bool OperatorNear(float value, float expected) noexcept {
    const float difference = value - expected;
    return difference < 1e-5f && difference > -1e-5f;
}

Guid OperatorSlab(GraphFixture& fixture, SubsurfaceType type = SubsurfaceType::Diffusion) noexcept {
    SurfaceSlabProperties properties;
    properties.subSurfaceType = type;
    return fixture.Add("Material.SurfaceSlab", fixture.env.Save(properties));
}

Guid OperatorPreset(GraphFixture& fixture, ShadingModelPreset model) noexcept {
    SurfaceFromShadingModelProperties properties;
    properties.shadingModelOverride = model;
    return fixture.Add("Material.SurfaceFromShadingModel", fixture.env.Save(properties));
}

// 두 표면을 type 연산의 (first, second)에 잇고 출력 Surface에 잇는다.
Guid OperatorJoin(GraphFixture& fixture, StringView type, Guid first, StringView firstKey,
                  Guid second, StringView secondKey) noexcept {
    const Guid node = fixture.Add(type);
    fixture.Connect(first, "Result", node, firstKey);
    fixture.Connect(second, "Result", node, secondKey);
    fixture.Connect(node, "Result", OutputNodeId, "Surface");
    return node;
}

} // namespace

// Horizontal: 두 slab을 접어 DefaultLit 하나, 노멀은 w로 섞어 정규화한다.
TEST(MaterialSurfaceOperator_HorizontalMix) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid background = OperatorSlab(fixture);
    const Guid foreground = OperatorSlab(fixture);
    fixture.Connect(AddConstant(fixture, 0.8f), "Result", foreground, "DiffuseAlbedo");
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", foreground, "Normal");
    OperatorJoin(fixture, "Material.SurfaceHorizontalMix", background, "Background", foreground,
                 "Foreground");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(out.shadingModels == OperatorModelBit(rocks::ShadingModel::DefaultLit));
    const IrResult* normal = OperatorEntry(out, IrEntry::Normal, "Normal");
    REQUIRE(normal != nullptr);
    CHECK(ValueOf(out, normal->value).op == IrOp::Normalize);
    // 상수 albedo는 w = 0.5로 접힌다(0.18과 0.8의 평균).
    CHECK(OperatorNear(OperatorConstant(out, "BaseColor"), 0.49f));
}

// Horizontal의 표지는 OR다: fuzz slab과 섞으면 Cloth다.
TEST(MaterialSurfaceOperator_HorizontalFlags) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid plain = OperatorSlab(fixture);
    const Guid fuzzy = OperatorSlab(fixture);
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", fuzzy, "FuzzAmount");
    OperatorJoin(fixture, "Material.SurfaceHorizontalMix", plain, "Background", fuzzy,
                 "Foreground");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(out.shadingModels == OperatorModelBit(rocks::ShadingModel::Cloth));
}

// Vertical: 위층 투과(exp)로 섞고 anisotropy는 버린다. 아래층 SSS만 남는다.
TEST(MaterialSurfaceOperator_VerticalLayer) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid top = OperatorSlab(fixture);
    const Guid base = OperatorSlab(fixture);
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", base, "Anisotropy");
    OperatorJoin(fixture, "Material.SurfaceVerticalLayer", top, "Top", base, "Base");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(out.shadingModels == OperatorModelBit(rocks::ShadingModel::DefaultLit));
    CHECK(OperatorEntry(out, IrEntry::Surface, "Anisotropy") == nullptr);
    bool exponential = false;
    for (const IrValue& value : out.module.Values()) {
        exponential = exponential || value.op == IrOp::Exponential;
    }

    CHECK(exponential);
    CHECK(OperatorUses(out, "CameraVector"));
}

// Add: coverage 합의 saturate가 반투명 Opacity 진입점이다.
TEST(MaterialSurfaceOperator_Add) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    OperatorJoin(fixture, "Material.SurfaceAdd", OperatorSlab(fixture), "A", OperatorSlab(fixture),
                 "B");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(BlendMode::Translucent), out));
    REQUIRE(out.succeeded);
    const IrResult* opacity = OperatorEntry(out, IrEntry::Opacity, "Opacity");
    REQUIRE(opacity != nullptr);
    CHECK(OperatorNear(FloatOf(ValueOf(out, opacity->value), 0), 1.0f));
}

// Weight: slab의 coverage가 Opacity 진입점이 되고, Unlit 위에서는 emissive에 곱한다.
TEST(MaterialSurfaceOperator_Weight) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid weight = fixture.Add("Material.SurfaceWeight");
    fixture.Connect(OperatorSlab(fixture), "Result", weight, "A");
    fixture.Connect(AddConstant(fixture, 0.25f), "Result", weight, "Weight");
    fixture.Connect(weight, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(BlendMode::Translucent), out));
    REQUIRE(out.succeeded);
    const IrResult* opacity = OperatorEntry(out, IrEntry::Opacity, "Opacity");
    REQUIRE(opacity != nullptr);
    CHECK(OperatorNear(FloatOf(ValueOf(out, opacity->value), 0), 0.25f));

    GraphFixture unlit(env);
    const Guid emitter = unlit.Add("Material.SurfaceUnlit");
    unlit.Connect(AddConstant(unlit, 0.8f), "Result", emitter, "EmissiveColor");
    const Guid scaled = unlit.Add("Material.SurfaceWeight");
    unlit.Connect(emitter, "Result", scaled, "A");
    unlit.Connect(AddConstant(unlit, 0.5f), "Result", scaled, "Weight");
    unlit.Connect(scaled, "Result", OutputNodeId, "Surface");
    MaterialTranslation unlitOut(env.allocator);
    REQUIRE(Translate(unlit, Traits(), unlitOut));
    REQUIRE(unlitOut.succeeded);
    CHECK(unlitOut.shadingModels == OperatorModelBit(rocks::ShadingModel::Unlit));
    CHECK(OperatorNear(OperatorConstant(unlitOut, "Emissive"), 0.4f));
}

// 뿌리 Select: 상수 SelectValue(0 ≤ Threshold)면 A, 값이면 칸마다 골라 Hair·DefaultLit 합집합이다.
TEST(MaterialSurfaceOperator_SelectRoot) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid hair = fixture.Add("Material.SurfaceHair");
    const Guid select =
        OperatorJoin(fixture, "Material.SurfaceSelect", hair, "A", OperatorSlab(fixture), "B");
    (void)select;
    MaterialTranslation constant(env.allocator);
    REQUIRE(Translate(fixture, Traits(), constant));
    REQUIRE(constant.succeeded);
    CHECK(constant.shadingModels == OperatorModelBit(rocks::ShadingModel::Hair));

    GraphFixture varying(env);
    SurfaceSelectProperties properties;
    properties.threshold = 0.25f;
    const Guid chooser = varying.Add("Material.SurfaceSelect", env.Save(properties));
    varying.Connect(varying.Add("Material.SurfaceHair"), "Result", chooser, "A");
    varying.Connect(OperatorSlab(varying), "Result", chooser, "B");
    varying.Connect(AddVarying(varying, 0.5f), "Result", chooser, "SelectValue");
    varying.Connect(chooser, "Result", OutputNodeId, "Surface");
    MaterialTranslation mixed(env.allocator);
    REQUIRE(Translate(varying, Traits(), mixed));
    REQUIRE(mixed.succeeded);
    CHECK(mixed.shadingModels == (OperatorModelBit(rocks::ShadingModel::Hair) |
                                  OperatorModelBit(rocks::ShadingModel::DefaultLit)));
    const IrResult* model = OperatorEntry(mixed, IrEntry::Surface, "ShadingModel");
    REQUIRE(model != nullptr);
    CHECK(ValueOf(mixed, model->value).op == IrOp::Select);
}

// 허용 표: Horizontal에 Hair가 흐르면 UE 문구(허용되지 않는 종류만)다.
TEST(MaterialSurfaceOperator_AllowedList) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid mix =
        OperatorJoin(fixture, "Material.SurfaceHorizontalMix", fixture.Add("Material.SurfaceHair"),
                     "Background", OperatorSlab(fixture), "Foreground");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::UnsupportedCapability, mix));
    CHECK(OperatorMessage(out, "Substrate error: Operator Horizontal cannot be used with the "
                               "following data flowing through {Hair }."));

    GraphFixture water(env);
    const Guid weight = water.Add("Material.SurfaceWeight");
    water.Connect(water.Add("Material.SurfaceSingleLayerWater"), "Result", weight, "A");
    water.Connect(weight, "Result", OutputNodeId, "Surface");
    MaterialTranslation waterOut(env.allocator);
    REQUIRE(Translate(water, Traits(), waterOut));
    CHECK(HasOnlyErrorAt(waterOut, MaterialDiagnosticCode::UnsupportedCapability, weight));
    CHECK(OperatorMessage(waterOut, "Substrate SingleLayerWater BSDF node cannot be used with "
                                    "parameter blending."));

    GraphFixture decal(env);
    const Guid toDecal = decal.Add("Material.SurfaceToDecal");
    decal.Connect(OperatorSlab(decal), "Result", toDecal, "DecalMaterial");
    const Guid weighted = decal.Add("Material.SurfaceWeight");
    decal.Connect(toDecal, "Result", weighted, "A");
    decal.Connect(weighted, "Result", OutputNodeId, "Surface");
    MaterialTranslation decalOut(env.allocator);
    REQUIRE(
        Translate(decal, Traits(BlendMode::Translucent, MaterialDomain::DeferredDecal), decalOut));
    CHECK(HasOnlyErrorAt(decalOut, MaterialDiagnosticCode::UnsupportedCapability, weighted));

    // Unlit은 Weight에는 되지만 Add에는 안 된다.
    GraphFixture unlit(env);
    const Guid add = OperatorJoin(unlit, "Material.SurfaceAdd", unlit.Add("Material.SurfaceUnlit"),
                                  "A", OperatorSlab(unlit), "B");
    MaterialTranslation unlitOut(env.allocator);
    REQUIRE(Translate(unlit, Traits(), unlitOut));
    CHECK(HasOnlyErrorAt(unlitOut, MaterialDiagnosticCode::UnsupportedCapability, add));
    CHECK(OperatorMessage(unlitOut, "Substrate error: Operator Add cannot be used with the "
                                    "following data flowing through {Unlit }."));
}

// ToDecal은 접은 연산 트리를 받는다.
TEST(MaterialSurfaceOperator_DecalOfOperator) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid mix = fixture.Add("Material.SurfaceHorizontalMix");
    fixture.Connect(OperatorSlab(fixture), "Result", mix, "Background");
    fixture.Connect(OperatorSlab(fixture), "Result", mix, "Foreground");
    const Guid decal = fixture.Add("Material.SurfaceToDecal");
    fixture.Connect(mix, "Result", decal, "DecalMaterial");
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", decal, "Coverage");
    fixture.Connect(decal, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(BlendMode::Translucent, MaterialDomain::DeferredDecal), out));
    REQUIRE(out.succeeded);
    CHECK(OperatorEntry(out, IrEntry::Opacity, "Opacity") != nullptr);
}

// 연산 아래 고전 preset은 slab으로 바뀐다: Cloth preset은 fuzz slab(Cloth), ClearCoat preset은
// SimpleClearCoat(ClearCoat), Subsurface preset은 Wrap(Subsurface)이다.
TEST(MaterialSurfaceOperator_ClassicUnderOperator) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    struct Case {
        ShadingModelPreset preset;
        rocks::ShadingModel model;
    };

    const Case cases[] = {
        {ShadingModelPreset::Cloth, rocks::ShadingModel::Cloth},
        {ShadingModelPreset::ClearCoat, rocks::ShadingModel::ClearCoat},
        {ShadingModelPreset::Subsurface, rocks::ShadingModel::Subsurface},
        {ShadingModelPreset::DefaultLit, rocks::ShadingModel::DefaultLit},
    };
    for (const Case& item : cases) {
        GraphFixture fixture(env);
        const Guid weight = fixture.Add("Material.SurfaceWeight");
        fixture.Connect(OperatorPreset(fixture, item.preset), "Result", weight, "A");
        fixture.Connect(weight, "Result", OutputNodeId, "Surface");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        CHECK((out.shadingModels & OperatorModelBit(item.model)) != 0);
    }

    // Hair preset은 static이면 Hair 종류다(Weight는 받지 않는다).
    GraphFixture hair(env);
    const Guid weight = hair.Add("Material.SurfaceWeight");
    hair.Connect(OperatorPreset(hair, ShadingModelPreset::Hair), "Result", weight, "A");
    hair.Connect(weight, "Result", OutputNodeId, "Surface");
    MaterialTranslation hairOut(env.allocator);
    REQUIRE(Translate(hair, Traits(), hairOut));
    CHECK(HasOnlyErrorAt(hairOut, MaterialDiagnosticCode::UnsupportedCapability, weight));
}

// 도우미: MetalnessToDiffuseAlbedoF0(금속 1 → DiffuseAlbedo 0·F0 = BaseColor), 나머지 셋도 slab
// 입력으로 번역된다(ThinFilm은 CameraVector를 쓴다).
TEST(MaterialSurfaceOperator_Helpers) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid metalness = fixture.Add("Material.MetalnessToDiffuseAlbedoF0");
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", metalness, "BaseColor");
    fixture.Connect(AddConstant(fixture, 1.0f), "Result", metalness, "Metallic");
    const Guid slab = OperatorSlab(fixture);
    fixture.Connect(metalness, "DiffuseAlbedo", slab, "DiffuseAlbedo");
    fixture.Connect(metalness, "F0", slab, "F0");
    fixture.Connect(slab, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(OperatorNear(OperatorConstant(out, "BaseColor"), 0.5f));
    CHECK(OperatorNear(OperatorConstant(out, "Metallic"), 1.0f));

    GraphFixture others(env);
    const Guid haze = others.Add("Material.HazinessToSecondaryRoughness");
    const Guid film = others.Add("Material.ThinFilm");
    const Guid mfp = others.Add("Material.TransmittanceToMeanFreePath");
    const Guid layered = OperatorSlab(others, SubsurfaceType::Wrap);
    others.Connect(haze, "Second Roughness", layered, "SecondRoughness");
    others.Connect(haze, "Second Roughness Weight", layered, "SecondRoughnessWeight");
    others.Connect(film, "Specular Color", layered, "F0");
    others.Connect(film, "Edge Specular Color", layered, "F90");
    others.Connect(mfp, "MFP", layered, "SSSMFP");
    others.Connect(layered, "Result", OutputNodeId, "Surface");
    MaterialTranslation othersOut(env.allocator);
    REQUIRE(Translate(others, Traits(), othersOut));
    REQUIRE(othersOut.succeeded);
    CHECK(OperatorUses(othersOut, "CameraVector"));
    CHECK((othersOut.shadingModels & OperatorModelBit(rocks::ShadingModel::Subsurface)) != 0);
}

} // namespace material_surface_operator_tests_file
