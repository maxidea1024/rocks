#include <MaterialCompiler/MaterialSourceWriter.h>
#include <MaterialSchema/MaterialContextValue.h>
#include <MaterialSchema/Nodes/Outputs/NeuralNetworkOutputProperties.generated.h>
#include <MaterialSchema/Nodes/Outputs/NeuralNetworkOutputProperties.h>
#include <MaterialSchema/Nodes/Outputs/VolumetricAdvancedOutputProperties.generated.h>
#include <MaterialSchema/Nodes/Outputs/VolumetricAdvancedOutputProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceFromShadingModelProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceFromShadingModelProperties.h>

#include "MaterialGraphFixture.h"

// MS2d3 시험이다(Docs/Design/MaterialSystem 6.11절 MS2d3): custom 출력 노드의 결과·존재 정의·생성
// 함수, 필수 입력("Input missing"), 기본값 없는 입력, 하나만 규칙, 정점 빈도(이전 프레임), RVT
// 범위, 노드 속성 정의(IR hash 포함), 표면이 주는 출력(노드 우선), 노멀 출력 순환 검사,
// MaterialCache 늦은 진단, 신경망 입출력, 볼륨·구름 입력 노드를 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_custom_output_tests_file {

using namespace material_graph_fixture;

namespace {

const IrResult* CustomEntry(const MaterialTranslation& out, IrEntry entry,
                            StringView slot) noexcept {
    for (const IrResult& result : out.module.Results()) {
        if (result.entry == entry && result.slot == slot) {
            return &result;
        }
    }

    return nullptr;
}

bool CustomText(StringView text, StringView part) noexcept {
    for (usize at = 0; at + part.ByteCount() <= text.ByteCount(); ++at) {
        if (StringView(text.Data() + at, part.ByteCount()) == part) {
            return true;
        }
    }

    return false;
}

bool CustomMessage(const MaterialTranslation& out, StringView message) noexcept {
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        if (diagnostic.message.View() == message) {
            return true;
        }
    }

    return false;
}

bool CustomUses(const MaterialTranslation& out, StringView name) noexcept {
    const Span<const MaterialContextValue> table = MaterialContextValues();
    for (const IrValue& value : out.module.Values()) {
        if (value.op == IrOp::Extern && value.immediate[0] < table.Size() &&
            table[static_cast<usize>(value.immediate[0])].name == name) {
            return true;
        }
    }

    return false;
}

bool CustomListed(const MaterialTranslation& out, StringView name) noexcept {
    for (StringView item : out.customOutputs) {
        if (item == name) {
            return true;
        }
    }

    return false;
}

bool CustomGenerate(GraphFixture& fixture, const MaterialTraitsDesc& traits,
                    MaterialTranslation& out, MaterialGeneratedSource& source) noexcept {
    return fixture.ready && Translate(fixture, traits, out).IsOk() && out.succeeded &&
           WriteMaterialSource(fixture.env.allocator, out.module, traits, TestAssetId, MainGraphId,
                               source)
               .IsOk() &&
           source.succeeded;
}

} // namespace

// BentNormal: 연결된 입력이 float4로 맞춘 결과이고, 출력 수·존재 정의와 함수가 생긴다.
TEST(MaterialCustomOutput_BentNormal) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.BentNormalOutput");
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", node, "Input");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(CustomGenerate(fixture, Traits(), out, source));
    const IrResult* result = CustomEntry(out, IrEntry::CustomOutput, "BentNormal0");
    REQUIRE(result != nullptr);
    CHECK(ValueOf(out, result->value).type == IrFloat(4));
    CHECK(CustomListed(out, "BentNormal"));
    const StringView text = source.text.View();
    CHECK(CustomText(text, "#define MATERIAL_CUSTOM_OUTPUT_BENTNORMAL 1\n"));
    CHECK(CustomText(text, "#define MATERIAL_HAS_CUSTOM_OUTPUT_BENTNORMAL0 1\n"));
    CHECK(
        CustomText(text, "float4 EvaluateMaterialCustomOutput_BentNormal0(in MaterialInputs c) {"));
}

// 필수 입력이 없으면 UE 문구 "Input missing"이다.
TEST(MaterialCustomOutput_RequiredInput) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.ClearCoatNormalOutput");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::MissingInput, node));
    CHECK(CustomMessage(out, "Input missing"));
}

// 기본값 있는 입력은 연결이 없어도 결과가 있고, 기본값 없는 입력은 연결된 번호만 낸다.
TEST(MaterialCustomOutput_DefaultsAndOptional) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    fixture.Add("Material.ThinTranslucentOutput");
    const Guid medium = fixture.Add("Material.SubsurfaceMediumOutput");
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", medium, "MeanFreePath");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(CustomGenerate(fixture, Traits(), out, source));
    const IrResult* transmittance =
        CustomEntry(out, IrEntry::CustomOutput, "ThinTranslucentMaterialOutput0");
    REQUIRE(transmittance != nullptr);
    const IrValue& value = ValueOf(out, transmittance->value);
    REQUIRE(value.op == IrOp::Constant);
    CHECK(FloatOf(value, 0) == 0.5f);
    CHECK(FloatOf(value, 3) == 0.0f);
    CHECK(CustomEntry(out, IrEntry::CustomOutput, "ThinTranslucentMaterialOutput1") != nullptr);
    CHECK(CustomEntry(out, IrEntry::CustomOutput, "SubsurfaceMediumMaterialOutput0") != nullptr);
    CHECK(CustomEntry(out, IrEntry::CustomOutput, "SubsurfaceMediumMaterialOutput1") == nullptr);
    const StringView text = source.text.View();
    CHECK(CustomText(text, "#define MATERIAL_CUSTOM_OUTPUT_SUBSURFACEMEDIUMMATERIALOUTPUT 2\n"));
    CHECK(
        CustomText(text, "#define MATERIAL_HAS_CUSTOM_OUTPUT_SUBSURFACEMEDIUMMATERIALOUTPUT0 1\n"));
    CHECK(!CustomText(text, "MATERIAL_HAS_CUSTOM_OUTPUT_SUBSURFACEMEDIUMMATERIALOUTPUT1"));
}

// 같은 종류의 출력 노드는 하나만이다(둘째 노드 위치의 UE 문구).
TEST(MaterialCustomOutput_OnlyOne) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    fixture.Add("Material.ThinTranslucentOutput");
    const Guid second = fixture.Add("Material.ThinTranslucentOutput");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::UnsupportedCapability, second));
    CHECK(CustomMessage(out, "The material can contain only one Thin Translucent Output node"));
}

// FirstPersonOutput은 정점 빈도이고 이전 프레임 함수도 낸다.
TEST(MaterialCustomOutput_VertexFrequency) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    fixture.Add("Material.FirstPersonOutput");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(CustomGenerate(fixture, Traits(), out, source));
    CHECK(CustomEntry(out, IrEntry::CustomOutputVertex, "FirstPersonOutput0") != nullptr);
    CHECK(CustomEntry(out, IrEntry::CustomOutputVertexPrevious, "FirstPersonOutput0") != nullptr);
    const StringView text = source.text.View();
    CHECK(CustomText(
        text,
        "float4 EvaluateMaterialCustomOutput_FirstPersonOutput0(in MaterialVertexInputs c) {"));
    CHECK(CustomText(text, "float4 EvaluateMaterialCustomOutputPrevious_FirstPersonOutput0(in "
                           "MaterialVertexInputs c) {"));
}

// RVT 범위: 같은 스위치가 RVT 출력 입력에서는 VirtualTextureOutput, 표면에서는 Default다.
TEST(MaterialCustomOutput_VirtualTextureScope) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid pass = fixture.Add("Material.RuntimeVirtualTexturePassSwitch");
    fixture.Connect(AddConstant(fixture, 0.25f), "Result", pass, "Default");
    fixture.Connect(AddConstant(fixture, 0.75f), "Result", pass, "VirtualTextureOutput");
    const Guid rvt = fixture.Add("Material.RuntimeVirtualTextureOutput");
    fixture.Connect(pass, "Result", rvt, "Roughness");
    const Guid slab = fixture.Add("Material.SurfaceSlab");
    fixture.Connect(pass, "Result", slab, "Roughness");
    fixture.Connect(slab, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const IrResult* scoped = CustomEntry(out, IrEntry::CustomOutput, "VirtualTextureOutput2");
    REQUIRE(scoped != nullptr);
    CHECK(FloatOf(ValueOf(out, scoped->value), 0) == 0.75f);
    const IrResult* roughness = CustomEntry(out, IrEntry::Surface, "Roughness");
    REQUIRE(roughness != nullptr);
    CHECK(FloatOf(ValueOf(out, roughness->value), 0) == 0.25f);
}

// VolumetricAdvancedOutput 속성이 UE 정의가 되고 IR hash가 그 값을 본다.
TEST(MaterialCustomOutput_VolumetricDefines) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    VolumetricAdvancedOutputProperties properties;
    properties.multiScatteringApproximationOctaveCount = 2;
    fixture.Add("Material.VolumetricAdvancedOutput", env.Save(properties));
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    const MaterialTraitsDesc traits = Traits(BlendMode::Additive, MaterialDomain::Volume);
    REQUIRE(CustomGenerate(fixture, traits, out, source));
    const StringView text = source.text.View();
    CHECK(CustomText(text, "#define MATERIAL_VOLUMETRIC_ADVANCED 1\n"));
    CHECK(CustomText(text, "#define MATERIAL_VOLUMETRIC_ADVANCED_PHASE_PERPIXEL 1\n"));
    CHECK(
        CustomText(text, "#define MATERIAL_VOLUMETRIC_ADVANCED_MULTISCATTERING_OCTAVE_COUNT 2\n"));
    CHECK(CustomText(text, "#define MATERIAL_VOLUMETRIC_ADVANCED_RAYMARCH_VOLUME_SHADOW 1\n"));
    CHECK(CustomText(text, "#define MATERIAL_CUSTOM_OUTPUT_VOLUMETRICADVANCEDMATERIALOUTPUT 7\n"));

    GraphFixture other(env);
    properties.multiScatteringApproximationOctaveCount = 3;
    other.Add("Material.VolumetricAdvancedOutput", env.Save(properties));
    MaterialTranslation otherOut(env.allocator);
    REQUIRE(Translate(other, traits, otherOut));
    REQUIRE(otherOut.succeeded);
    CHECK(!(out.module.Hash() == otherOut.module.Hash()));
}

// 표면이 주는 출력: 고전 ClearCoat preset의 ClearCoatNormal과 SingleLayerWater BSDF의 물 계수다.
// 같은 종류의 노드가 있으면 노드가 이긴다.
TEST(MaterialCustomOutput_FromSurface) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    SurfaceFromShadingModelProperties properties;
    properties.shadingModelOverride = ShadingModelPreset::ClearCoat;
    const Guid preset = fixture.Add("Material.SurfaceFromShadingModel", env.Save(properties));
    const Guid coat = AddVarying(fixture, 0.5f);
    fixture.Connect(coat, "Result", preset, "ClearCoatNormal");
    fixture.Connect(preset, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(CustomEntry(out, IrEntry::CustomOutput, "ClearCoatBottomNormal0") != nullptr);

    const Guid node = fixture.Add("Material.ClearCoatNormalOutput");
    fixture.Connect(AddConstant(fixture, 0.25f), "Result", node, "Input");
    MaterialTranslation withNode(env.allocator);
    REQUIRE(Translate(fixture, Traits(), withNode));
    REQUIRE(withNode.succeeded);
    const IrResult* result = CustomEntry(withNode, IrEntry::CustomOutput, "ClearCoatBottomNormal0");
    REQUIRE(result != nullptr);
    CHECK(ValueOf(withNode, result->value).op == IrOp::Constant);

    GraphFixture water(env);
    const Guid slw = water.Add("Material.SurfaceSingleLayerWater");
    water.Connect(slw, "Result", OutputNodeId, "Surface");
    MaterialTranslation waterOut(env.allocator);
    REQUIRE(Translate(water, Traits(), waterOut));
    REQUIRE(waterOut.succeeded);
    CHECK(CustomEntry(waterOut, IrEntry::CustomOutput, "SingleLayerWaterMaterialOutput3") !=
          nullptr);
    CHECK(CustomListed(waterOut, "SingleLayerWaterMaterialOutput"));
}

// 노멀 계열 출력에 PixelNormal이 닿으면 순환이다(19.3.3절).
TEST(MaterialCustomOutput_NormalCycle) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid pixel = fixture.Add("Material.PixelNormal");
    const Guid node = fixture.Add("Material.BentNormalOutput");
    fixture.Connect(pixel, "Result", node, "Input");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(HasErrorAt(out, MaterialDiagnosticCode::Cycle, pixel));
}

// MaterialCache 출력은 선행(MPQ-VirtualTexture)이 없어 그 노드의 늦은 진단이다.
TEST(MaterialCustomOutput_MaterialCachePending) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.MaterialCacheOutput");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::PrerequisiteMissing, node));
}

// 신경망: 입력 Coordinates 기본값은 (−1, 0, ViewportUv)이고, 출력 노드는 PostProcess에서만 된다.
TEST(MaterialCustomOutput_Neural) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture input(env);
    input.Add("Material.NeuralNetworkInput");
    MaterialTranslation inputOut(env.allocator);
    REQUIRE(Translate(input, Traits(), inputOut));
    REQUIRE(inputOut.succeeded);
    CHECK(CustomEntry(inputOut, IrEntry::CustomOutput, "NeuralInput0") != nullptr);
    CHECK(CustomUses(inputOut, "ViewportUv"));

    GraphFixture wrong(env);
    NeuralNetworkOutputProperties properties;
    const Guid output = wrong.Add("Material.NeuralNetworkOutput", env.Save(properties));
    const Guid unlit = wrong.Add("Material.SurfaceUnlit");
    wrong.Connect(output, "RGBA", unlit, "EmissiveColor");
    wrong.Connect(unlit, "Result", OutputNodeId, "Surface");
    MaterialTranslation wrongOut(env.allocator);
    REQUIRE(Translate(wrong, Traits(), wrongOut));
    CHECK(HasOnlyErrorAt(wrongOut, MaterialDiagnosticCode::UnsupportedCapability, output));

    GraphFixture post(env);
    const Guid neural = post.Add("Material.NeuralNetworkOutput", env.Save(properties));
    const Guid surface = post.Add("Material.SurfacePostProcess");
    post.Connect(neural, "RGBA", surface, "Color");
    post.Connect(surface, "Result", OutputNodeId, "Surface");
    MaterialTranslation postOut(env.allocator);
    REQUIRE(Translate(post, Traits(BlendMode::Opaque, MaterialDomain::PostProcess), postOut));
    REQUIRE(postOut.succeeded);
    CHECK(CustomUses(postOut, "NeuralTextureOutput"));
}

// 구름 입력 노드는 문맥 값을 낸다.
TEST(MaterialCustomOutput_CloudInputs) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid cloud = fixture.Add("Material.CloudSampleAttribute");
    const Guid unlit = fixture.Add("Material.SurfaceUnlit");
    fixture.Connect(cloud, "Altitude", unlit, "EmissiveColor");
    fixture.Connect(unlit, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(CustomUses(out, "CloudSampleAltitude"));
}

} // namespace material_custom_output_tests_file
