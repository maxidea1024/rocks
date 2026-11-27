#include "CustomOutputTable.h"

// custom 출력 표 값이다(Docs/Design/MaterialSystem 6.11절 MS2d3 "출력 표" — 기본값은 UE Compile 값,
// MaterialExpressions.cpp:20262–23645, 3054–3129, 22466–22495).
namespace rocks::material::detail {

namespace {

// 입력 하나다(기본값 있음).
constexpr CustomOutputInput OutputValued(StringView key, uint8 width, float x, float y, float z,
                                         float w, StringView slot) noexcept {
    CustomOutputInput input;
    input.key = key;
    input.width = width;
    input.fallback[0] = x;
    input.fallback[1] = y;
    input.fallback[2] = z;
    input.fallback[3] = w;
    input.slot = slot;
    return input;
}

constexpr CustomOutputInput OutputSplat(StringView key, uint8 width, float value,
                                        StringView slot) noexcept {
    return OutputValued(key, width, value, value, value, value, slot);
}

// 기본값 없는 입력이다(연결이 없으면 함수를 내지 않는다).
constexpr CustomOutputInput OutputOptional(StringView key, uint8 width, StringView slot) noexcept {
    CustomOutputInput input = OutputSplat(key, width, 0.0f, slot);
    input.hasDefault = false;
    return input;
}

// 필수 입력이다(연결이 없으면 "Input missing").
constexpr CustomOutputInput OutputRequired(StringView key, uint8 width, StringView slot) noexcept {
    CustomOutputInput input = OutputOptional(key, width, slot);
    input.required = true;
    return input;
}

constexpr CustomOutputInput BentNormalInputs[] = {OutputRequired("Input", 3, "BentNormal0")};
constexpr CustomOutputInput ClearCoatNormalInputs[] = {
    OutputRequired("Input", 3, "ClearCoatBottomNormal0")};
constexpr CustomOutputInput TangentInputs[] = {OutputRequired("Input", 3, "TangentOutput0")};
constexpr CustomOutputInput ThinTranslucentInputs[] = {
    OutputSplat("TransmittanceColor", 3, 0.5f, "ThinTranslucentMaterialOutput0"),
    OutputSplat("SurfaceCoverage", 1, 1.0f, "ThinTranslucentMaterialOutput1")};
constexpr CustomOutputInput AbsorptionMediumInputs[] = {
    OutputSplat("TransmittanceColor", 3, 1.0f, "AbsorptionMediumMaterialOutput0")};
constexpr CustomOutputInput SubsurfaceMediumInputs[] = {
    OutputOptional("MeanFreePath", 3, "SubsurfaceMediumMaterialOutput0"),
    OutputOptional("ScatteringDistribution", 1, "SubsurfaceMediumMaterialOutput1")};
constexpr CustomOutputInput SingleLayerWaterInputs[] = {
    OutputSplat("ScatteringCoefficients", 3, 0.0f, "SingleLayerWaterMaterialOutput0"),
    OutputSplat("AbsorptionCoefficients", 3, 0.0f, "SingleLayerWaterMaterialOutput1"),
    OutputSplat("PhaseG", 1, 0.0f, "SingleLayerWaterMaterialOutput2"),
    OutputSplat("ColorScaleBehindWater", 3, 1.0f, "SingleLayerWaterMaterialOutput3")};
// 0–5는 핀 기본값(UE Const* 속성 기본값과 같다), 6은 float4로 맞춘 (1, 1, 1)이다.
constexpr CustomOutputInput VolumetricAdvancedInputs[] = {
    OutputSplat("PhaseG", 1, 0.0f, "VolumetricAdvancedMaterialOutput0"),
    OutputSplat("PhaseG2", 1, 0.0f, "VolumetricAdvancedMaterialOutput1"),
    OutputSplat("PhaseBlend", 1, 0.0f, "VolumetricAdvancedMaterialOutput2"),
    OutputSplat("MultiScatteringContribution", 1, 0.5f, "VolumetricAdvancedMaterialOutput3"),
    OutputSplat("MultiScatteringOcclusion", 1, 0.5f, "VolumetricAdvancedMaterialOutput4"),
    OutputSplat("MultiScatteringEccentricity", 1, 0.5f, "VolumetricAdvancedMaterialOutput5"),
    OutputValued("ConservativeDensity", 4, 1.0f, 1.0f, 1.0f, 0.0f,
                 "VolumetricAdvancedMaterialOutput6")};
constexpr CustomOutputInput EmptySpaceSkippingInputs[] = {
    OutputSplat("ContainsMatter", 1, 1.0f, "VolumetricCloudEmptySpaceSkippingOutput0")};
constexpr CustomOutputInput FirstPersonInputs[] = {
    OutputSplat("FirstPersonInterpolationAlpha", 1, 1.0f, "FirstPersonOutput0")};
constexpr CustomOutputInput MotionVectorInputs[] = {
    OutputOptional("Input", 3, "MotionVectorWorldOffsetOutput0")};
constexpr CustomOutputInput TemporalResponsivenessInputs[] = {
    OutputOptional("Input", 1, "TemporalResponsivenessOutput0")};
constexpr CustomOutputInput VirtualTextureInputs[] = {
    OutputSplat("BaseColor", 3, 0.0f, "VirtualTextureOutput0"),
    OutputSplat("Specular", 1, 0.5f, "VirtualTextureOutput1"),
    OutputSplat("Roughness", 1, 0.5f, "VirtualTextureOutput2"),
    OutputValued("Normal", 3, 0.0f, 0.0f, 1.0f, 0.0f, "VirtualTextureOutput3"),
    OutputSplat("WorldHeight", 1, 0.0f, "VirtualTextureOutput4"),
    OutputSplat("Opacity", 1, 1.0f, "VirtualTextureOutput5"),
    OutputSplat("Mask", 1, 1.0f, "VirtualTextureOutput6"),
    OutputSplat("Displacement", 1, 0.0f, "VirtualTextureOutput7"),
    OutputSplat("Mask4", 4, 0.0f, "VirtualTextureOutput8")};
// Coordinates(0)의 기본값은 번역기가 정한다(ViewportUv에 batch 번호를 붙인다 — 6.11절 MS2d3).
constexpr CustomOutputInput NeuralInputs[] = {OutputOptional("Coordinates", 4, "NeuralInput0"),
                                              OutputSplat("Input0", 3, 0.5f, "NeuralInput1"),
                                              OutputSplat("Mask", 1, 1.0f, "NeuralInput2")};

constexpr CustomOutputKind
OutputKind(StringView type, StringView name, StringView defineName,
           Span<const CustomOutputInput> inputs,
           CustomOutputFrequency frequency = CustomOutputFrequency::Pixel,
           bool normalRoot = false) noexcept {
    CustomOutputKind kind;
    kind.type = type;
    kind.name = name;
    kind.defineName = defineName;
    kind.inputs = inputs;
    kind.frequency = frequency;
    kind.normalRoot = normalRoot;
    return kind;
}

constexpr CustomOutputKind CustomOutputTableKinds[] = {
    OutputKind("Material.BentNormalOutput", "BentNormal", "BENTNORMAL", BentNormalInputs,
               CustomOutputFrequency::Pixel, true),
    OutputKind("Material.ClearCoatNormalOutput", "ClearCoatBottomNormal", "CLEARCOATBOTTOMNORMAL",
               ClearCoatNormalInputs, CustomOutputFrequency::Pixel, true),
    OutputKind("Material.TangentOutput", "TangentOutput", "TANGENTOUTPUT", TangentInputs,
               CustomOutputFrequency::Pixel, true),
    OutputKind("Material.ThinTranslucentOutput", "ThinTranslucentMaterialOutput",
               "THINTRANSLUCENTMATERIALOUTPUT", ThinTranslucentInputs),
    OutputKind("Material.AbsorptionMediumOutput", "AbsorptionMediumMaterialOutput",
               "ABSORPTIONMEDIUMMATERIALOUTPUT", AbsorptionMediumInputs),
    OutputKind("Material.SubsurfaceMediumOutput", "SubsurfaceMediumMaterialOutput",
               "SUBSURFACEMEDIUMMATERIALOUTPUT", SubsurfaceMediumInputs),
    OutputKind("Material.SingleLayerWaterOutput", "SingleLayerWaterMaterialOutput",
               "SINGLELAYERWATERMATERIALOUTPUT", SingleLayerWaterInputs),
    OutputKind("Material.VolumetricAdvancedOutput", "VolumetricAdvancedMaterialOutput",
               "VOLUMETRICADVANCEDMATERIALOUTPUT", VolumetricAdvancedInputs),
    OutputKind("Material.VolumetricCloudEmptySpaceSkippingOutput",
               "VolumetricCloudEmptySpaceSkippingOutput", "VOLUMETRICCLOUDEMPTYSPACESKIPPINGOUTPUT",
               EmptySpaceSkippingInputs),
    OutputKind("Material.FirstPersonOutput", "FirstPersonOutput", "FIRSTPERSONOUTPUT",
               FirstPersonInputs, CustomOutputFrequency::Vertex),
    OutputKind("Material.MotionVectorWorldOffsetOutput", "MotionVectorWorldOffsetOutput",
               "MOTIONVECTORWORLDOFFSETOUTPUT", MotionVectorInputs),
    OutputKind("Material.TemporalResponsivenessOutput", "TemporalResponsivenessOutput",
               "TEMPORALRESPONSIVENESSOUTPUT", TemporalResponsivenessInputs),
    OutputKind("Material.RuntimeVirtualTextureOutput", "VirtualTextureOutput",
               "VIRTUALTEXTUREOUTPUT", VirtualTextureInputs),
    OutputKind("Material.NeuralNetworkInput", "NeuralInput", "NEURALINPUT", NeuralInputs),
};

} // namespace

Span<const CustomOutputKind> CustomOutputKinds() noexcept {
    return CustomOutputTableKinds;
}

const CustomOutputKind* FindCustomOutputKind(StringView type) noexcept {
    for (const CustomOutputKind& kind : CustomOutputTableKinds) {
        if (kind.type == type) {
            return &kind;
        }
    }

    return nullptr;
}

const CustomOutputKind* FindCustomOutputByName(StringView name) noexcept {
    for (const CustomOutputKind& kind : CustomOutputTableKinds) {
        if (kind.name == name) {
            return &kind;
        }
    }

    return nullptr;
}

} // namespace rocks::material::detail
