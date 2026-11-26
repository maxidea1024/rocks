#include <MaterialCompiler/MaterialSourceWriter.h>
#include <MaterialSchema/MaterialContextValue.h>
#include <MaterialSchema/Nodes/MaterialAttributes/ShadingModelProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/ShadingModelProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceFromShadingModelProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceFromShadingModelProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceVolumetricFogCloudProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceVolumetricFogCloudProperties.h>

#include "MaterialGraphFixture.h"

// MS2d1 시험이다(Docs/Design/MaterialSystem 6.11절 표면 기반): 표면이 없을 때의 기본(정점 노멀·
// DefaultLit), 고전 preset의 칸·shading model 집합 정의, 대표 노멀(tangent 공간 변환·정규화)과 순환
// 검사, Unlit의 AO 비활성, 영역 표면과 영역 검사, ToDecal coverage, VolumetricFogCloud AO, 반투명
// opacity, 번호표 밖 shading model, TODO 속성·노드의 늦은 진단을 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_surface_lowering_tests_file {

using namespace material_graph_fixture;

namespace {

uint64 ContextIndex(StringView name) noexcept {
    const Span<const MaterialContextValue> table = MaterialContextValues();
    for (usize index = 0; index < table.Size(); ++index) {
        if (table[index].name == name) {
            return index;
        }
    }

    return table.Size();
}

const IrResult* FindEntry(const MaterialTranslation& out, IrEntry entry, StringView slot) noexcept {
    for (const IrResult& result : out.module.Results()) {
        if (result.entry == entry && result.slot == slot) {
            return &result;
        }
    }

    return nullptr;
}

bool HasText(StringView text, StringView part) noexcept {
    for (usize at = 0; at + part.ByteCount() <= text.ByteCount(); ++at) {
        if (StringView(text.Data() + at, part.ByteCount()) == part) {
            return true;
        }
    }

    return false;
}

bool HasMessage(const MaterialTranslation& out, StringView message) noexcept {
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        if (diagnostic.message.View() == message) {
            return true;
        }
    }

    return false;
}

bool Generate(GraphFixture& fixture, const MaterialTraitsDesc& traits, MaterialTranslation& out,
              MaterialGeneratedSource& source) noexcept {
    return fixture.ready && Translate(fixture, traits, out).IsOk() && out.succeeded &&
           WriteMaterialSource(fixture.env.allocator, out.module, traits, TestAssetId, MainGraphId,
                               source)
               .IsOk() &&
           source.succeeded;
}

Guid AddPreset(GraphFixture& fixture, ShadingModelPreset model) noexcept {
    SurfaceFromShadingModelProperties properties;
    properties.shadingModelOverride = model;
    return fixture.Add("Material.SurfaceFromShadingModel", fixture.env.Save(properties));
}

// 반투명 Surface(Opacity 출력 핀이 쓰이지 않는 Translucent)다.
MaterialTraitsDesc Translucent() noexcept {
    return Traits(BlendMode::Translucent);
}

} // namespace

// 표면이 없으면 대표 노멀은 정점 노멀이고 shading model은 DefaultLit 하나다.
TEST(MaterialSurface_DefaultWithoutSurface) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, Traits(), out, source));
    const IrResult* normal = FindEntry(out, IrEntry::Normal, "Normal");
    REQUIRE(normal != nullptr);
    CHECK(ValueOf(out, normal->value).op == IrOp::Extern);
    CHECK(ValueOf(out, normal->value).immediate[0] == ContextIndex("VertexNormal"));
    CHECK(out.shadingModels == (1u << static_cast<uint32>(rocks::ShadingModel::DefaultLit)));
    CHECK(HasText(source.text.View(), "float3 EvaluateMaterialNormal(in MaterialInputs c) {"));
    CHECK(HasText(source.text.View(), "#define MATERIAL_SHADING_MODELS 2u\n"));
    CHECK(HasText(source.text.View(), "#define MATERIAL_SINGLE_SHADING_MODEL 1\n"));
}

// 고전 preset: 입력이 칸으로 지나가고(saturate), shading model 속성이 집합·하나 정의가 된다.
TEST(MaterialSurface_ClassicPreset) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid preset = AddPreset(fixture, ShadingModelPreset::ClearCoat);
    fixture.Connect(AddConstant(fixture, 0.25f), "Result", preset, "Metallic");
    fixture.Connect(AddConstant(fixture, 0.75f), "Result", preset, "ClearCoat");
    fixture.Connect(preset, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, Traits(), out, source));
    CHECK(out.shadingModels == (1u << static_cast<uint32>(rocks::ShadingModel::ClearCoat)));
    const IrResult* metallic = FindEntry(out, IrEntry::Surface, "Metallic");
    REQUIRE(metallic != nullptr);
    CHECK(FloatOf(ValueOf(out, metallic->value), 0) == 0.25f);
    const IrResult* clearCoat = FindEntry(out, IrEntry::Surface, "CustomData0");
    REQUIRE(clearCoat != nullptr);
    CHECK(FloatOf(ValueOf(out, clearCoat->value), 0) == 0.75f);
    CHECK(HasText(source.text.View(), "#define MATERIAL_SINGLE_SHADING_MODEL 4\n"));
    CHECK(HasText(source.text.View(), "    s.shadingModel = 4u;\n"));
    CHECK(HasText(source.text.View(), "    s.customData0 = 0.75;\n"));
}

// 연결된 Normal은 tangent 공간이면 world로 바꿔 정규화한 대표 노멀이고, Surface normal 칸도 같은
// 값이다.
TEST(MaterialSurface_RepresentativeNormal) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid preset = AddPreset(fixture, ShadingModelPreset::DefaultLit);
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", preset, "Normal");
    fixture.Connect(preset, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const IrResult* normal = FindEntry(out, IrEntry::Normal, "Normal");
    const IrResult* field = FindEntry(out, IrEntry::Surface, "Normal");
    REQUIRE(normal != nullptr);
    REQUIRE(field != nullptr);
    CHECK(normal->value == field->value);
    const IrValue& normalized = ValueOf(out, normal->value);
    CHECK(normalized.op == IrOp::Normalize);
    const IrValue& world = ValueOf(out, normalized.operands[0]);
    CHECK(world.op == IrOp::Extern);
    CHECK(world.immediate[0] == ContextIndex("TransformTangentVectorToWorld"));
}

// 대표 노멀 경로에 PixelNormal이 닿으면 UE 문구의 순환 오류다. 다른 칸에서 읽는 것은 된다.
TEST(MaterialSurface_PixelNormalCycle) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid preset = AddPreset(fixture, ShadingModelPreset::DefaultLit);
    const Guid pixel = fixture.Add("Material.PixelNormal");
    fixture.Connect(pixel, "Result", preset, "Normal");
    fixture.Connect(preset, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(!out.succeeded);
    CHECK(HasErrorAt(out, MaterialDiagnosticCode::Cycle, pixel));
    CHECK(HasMessage(out, "Invalid node PixelNormalWS used for Normal input."));

    GraphFixture fine(env);
    const Guid other = AddPreset(fine, ShadingModelPreset::DefaultLit);
    fine.Connect(fine.Add("Material.PixelNormal"), "Result", other, "BaseColor");
    fine.Connect(other, "Result", OutputNodeId, "Surface");
    MaterialTranslation allowed(env.allocator);
    REQUIRE(Translate(fine, Traits(), allowed));
    CHECK(allowed.succeeded);
}

// Unlit 표면: shading model Unlit 하나, 연결된 AO는 비활성 경고로 버린다.
TEST(MaterialSurface_UnlitDropsAmbientOcclusion) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid unlit = fixture.Add("Material.SurfaceUnlit");
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", unlit, "EmissiveColor");
    fixture.Connect(unlit, "Result", OutputNodeId, "Surface");
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, Traits(), out, source));
    CHECK(out.shadingModels == (1u << static_cast<uint32>(rocks::ShadingModel::Unlit)));
    CHECK(FindEntry(out, IrEntry::Surface, "AmbientOcclusion") == nullptr);
    CHECK(CountDiagnostics(out, MaterialDiagnosticCode::InactiveOutput,
                           nodegraph::GraphDiagnosticSeverity::Warning) == 1);
    CHECK(HasText(source.text.View(), "#define MATERIAL_SINGLE_SHADING_MODEL 0\n"));
}

// 영역 표면: 영역과 맞지 않으면 V6, 맞으면 emissive·opacity 칸이다.
TEST(MaterialSurface_DomainSurfaces) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture wrong(env);
    const Guid post = wrong.Add("Material.SurfacePostProcess");
    wrong.Connect(post, "Result", OutputNodeId, "Surface");
    MaterialTranslation rejected(env.allocator);
    REQUIRE(Translate(wrong, Traits(), rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::UnsupportedCapability, post));
    CHECK(HasMessage(rejected, "Surface node is not valid for the material domain"));

    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.SurfacePostProcess");
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", node, "Color");
    fixture.Connect(AddConstant(fixture, 0.25f), "Result", node, "Opacity");
    fixture.Connect(node, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(BlendMode::Opaque, MaterialDomain::PostProcess), out));
    REQUIRE(out.succeeded);
    CHECK(FindEntry(out, IrEntry::Surface, "Emissive") != nullptr);
    const IrResult* opacity = FindEntry(out, IrEntry::Surface, "SurfaceOpacity");
    REQUIRE(opacity != nullptr);
    CHECK(FloatOf(ValueOf(out, opacity->value), 0) == 0.25f);
}

// ToDecal: 자식 slab(고전 preset)의 opacity에 saturate(coverage)를 곱한다. Unlit 자식은 UE 문구
// 오류다.
TEST(MaterialSurface_ToDecal) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid preset = AddPreset(fixture, ShadingModelPreset::DefaultLit);
    const Guid decal = fixture.Add("Material.SurfaceToDecal");
    fixture.Connect(preset, "Result", decal, "DecalMaterial");
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", decal, "Coverage");
    fixture.Connect(decal, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(BlendMode::Translucent, MaterialDomain::DeferredDecal), out));
    REQUIRE(out.succeeded);
    const IrResult* opacity = FindEntry(out, IrEntry::Surface, "SurfaceOpacity");
    REQUIRE(opacity != nullptr);
    CHECK(ValueOf(out, opacity->value).op == IrOp::Saturate);

    GraphFixture unlit(env);
    const Guid child = unlit.Add("Material.SurfaceUnlit");
    const Guid wrapper = unlit.Add("Material.SurfaceToDecal");
    unlit.Connect(child, "Result", wrapper, "DecalMaterial");
    unlit.Connect(wrapper, "Result", OutputNodeId, "Surface");
    MaterialTranslation rejected(env.allocator);
    REQUIRE(
        Translate(unlit, Traits(BlendMode::Translucent, MaterialDomain::DeferredDecal), rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::UnsupportedCapability, wrapper));
}

// VolumetricFogCloud: Volume 영역에서 albedo·extinction 칸과 표면 AO가 출력 노드 AO를 대신한다.
TEST(MaterialSurface_VolumetricFogCloud) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    SurfaceVolumetricFogCloudProperties properties;
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.SurfaceVolumetricFogCloud", env.Save(properties));
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", node, "Albedo");
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", node, "AmbientOcclusion");
    fixture.Connect(node, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(BlendMode::Additive, MaterialDomain::Volume), out));
    REQUIRE(out.succeeded);
    CHECK(FindEntry(out, IrEntry::Surface, "BaseColor") != nullptr);
    CHECK(FindEntry(out, IrEntry::Surface, "SubsurfaceColor") != nullptr);
}

// 반투명(Translucent)은 Opacity 출력 핀이 쓰이지 않으므로 표면 opacity가 Opacity 진입점이다.
TEST(MaterialSurface_TranslucentOpacity) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid preset = AddPreset(fixture, ShadingModelPreset::DefaultLit);
    fixture.Connect(AddVarying(fixture, 0.3f), "Result", preset, "Opacity");
    fixture.Connect(preset, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Translucent(), out));
    REQUIRE(out.succeeded);
    const IrResult* opacity = FindEntry(out, IrEntry::Opacity, "Opacity");
    REQUIRE(opacity != nullptr);
    CHECK(ValueOf(out, opacity->value).op == IrOp::Saturate);
}

// 번호표에 없는 shading model(PreintegratedSkin)은 UE 이름의 InvalidProperty다.
TEST(MaterialSurface_UnsupportedShadingModel) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    ShadingModelProperties model;
    model.shadingModel = ShadingModelPreset::PreintegratedSkin;
    const Guid node = fixture.Add("Material.ShadingModel", env.Save(model));
    const Guid preset = AddPreset(fixture, ShadingModelPreset::DefaultLit);
    fixture.Connect(node, "Result", preset, "ShadingModel");
    fixture.Connect(preset, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::InvalidProperty, node));
    CHECK(HasMessage(out, "Shading model is not supported: PreintegratedSkin"));
}

// 선행 없는 것: 값이 있는 subsurfaceProfile 속성, Toon 노드, Unlit 색 투과는 그 노드의 늦은 진단이다.
TEST(MaterialSurface_PrerequisiteMissing) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture profile(env);
    SurfaceFromShadingModelProperties properties;
    properties.shadingModelOverride = ShadingModelPreset::SubsurfaceProfile;
    properties.subsurfaceProfile.assetId = AssetId{Guid{0x50524f46ull, 1}};
    const Guid preset = profile.Add("Material.SurfaceFromShadingModel", env.Save(properties));
    profile.Connect(preset, "Result", OutputNodeId, "Surface");
    MaterialTranslation profiled(env.allocator);
    REQUIRE(Translate(profile, Traits(), profiled));
    CHECK(HasOnlyErrorAt(profiled, MaterialDiagnosticCode::PrerequisiteMissing, preset));

    GraphFixture toon(env);
    const Guid node = toon.Add("Material.SurfaceToon");
    toon.Connect(node, "Result", OutputNodeId, "Surface");
    MaterialTranslation toonOut(env.allocator);
    REQUIRE(Translate(toon, Traits(), toonOut));
    CHECK(HasOnlyErrorAt(toonOut, MaterialDiagnosticCode::PrerequisiteMissing, node));

    GraphFixture colored(env);
    const Guid unlit = colored.Add("Material.SurfaceUnlit");
    colored.Connect(AddVarying(colored, 0.5f), "Result", unlit, "TransmittanceColor");
    colored.Connect(unlit, "Result", OutputNodeId, "Surface");
    MaterialTranslation transmitted(env.allocator);
    REQUIRE(Translate(colored, Traits(), transmitted));
    CHECK(HasOnlyErrorAt(transmitted, MaterialDiagnosticCode::PrerequisiteMissing, unlit));
}

} // namespace material_surface_lowering_tests_file
