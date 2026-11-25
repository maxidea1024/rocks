#include "MaterialBuiltinNodes.h"

#include "MaterialDynamicPins.h"
#include <MaterialSchema/Nodes/Outputs/MaterialCacheOutputProperties.generated.h>
#include <MaterialSchema/Nodes/Outputs/MaterialCacheOutputProperties.h>
#include <MaterialSchema/Nodes/Outputs/NeuralNetworkInputProperties.generated.h>
#include <MaterialSchema/Nodes/Outputs/NeuralNetworkInputProperties.h>
#include <MaterialSchema/Nodes/Outputs/NeuralNetworkOutputProperties.generated.h>
#include <MaterialSchema/Nodes/Outputs/NeuralNetworkOutputProperties.h>
#include <MaterialSchema/Nodes/Outputs/VolumetricAdvancedOutputProperties.generated.h>
#include <MaterialSchema/Nodes/Outputs/VolumetricAdvancedOutputProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Outputs 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition BentNormalOutputPins[] = {
    MaterialInput("Input", MaterialPinType::Float3, "Input", {}, MaterialPinFlags::HideValue),
};

constexpr MaterialPinDefinition ClearCoatNormalOutputPins[] = {
    MaterialInput("Input", MaterialPinType::Float3, "Input", {}, MaterialPinFlags::HideValue),
};

constexpr MaterialPinDefinition TangentOutputPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {}, MaterialPinFlags::HideValue),
};

constexpr MaterialPinDefinition ThinTranslucentOutputPins[] = {
    MaterialInput("TransmittanceColor", MaterialPinType::Float3, "TransmittanceColor", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("SurfaceCoverage", MaterialPinType::Float1, "SurfaceCoverage", {},
                  MaterialPinFlags::HideValue),
};

constexpr MaterialPinDefinition AbsorptionMediumOutputPins[] = {
    MaterialInput("TransmittanceColor", MaterialPinType::Numeric, "TransmittanceColor", {},
                  MaterialPinFlags::HideValue),
};

constexpr MaterialPinDefinition SubsurfaceMediumOutputPins[] = {
    MaterialInput("MeanFreePath", MaterialPinType::Float3, "MeanFreePath", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("ScatteringDistribution", MaterialPinType::Float1, "ScatteringDistribution", {},
                  MaterialPinFlags::HideValue),
};

constexpr MaterialPinDefinition SingleLayerWaterOutputPins[] = {
    MaterialInput("ScatteringCoefficients", MaterialPinType::Numeric, "ScatteringCoefficients", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("AbsorptionCoefficients", MaterialPinType::Numeric, "AbsorptionCoefficients", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("PhaseG", MaterialPinType::Numeric, "PhaseG", {}, MaterialPinFlags::HideValue),
    MaterialInput("ColorScaleBehindWater", MaterialPinType::Numeric, "ColorScaleBehindWater", {},
                  MaterialPinFlags::HideValue),
};

constexpr MaterialPinDefinition VolumetricAdvancedOutputPins[] = {
    MaterialInput("PhaseG", MaterialPinType::Numeric, "PhaseG", MaterialPinDefault::Scalar(0.0f)),
    MaterialInput("PhaseG2", MaterialPinType::Numeric, "PhaseG2", MaterialPinDefault::Scalar(0.0f)),
    MaterialInput("PhaseBlend", MaterialPinType::Numeric, "PhaseBlend",
                  MaterialPinDefault::Scalar(0.0f)),
    MaterialInput("MultiScatteringContribution", MaterialPinType::Numeric,
                  "MultiScatteringContribution", MaterialPinDefault::Scalar(0.5f)),
    MaterialInput("MultiScatteringOcclusion", MaterialPinType::Numeric, "MultiScatteringOcclusion",
                  MaterialPinDefault::Scalar(0.5f)),
    MaterialInput("MultiScatteringEccentricity", MaterialPinType::Numeric,
                  "MultiScatteringEccentricity", MaterialPinDefault::Scalar(0.5f)),
    MaterialInput("ConservativeDensity", MaterialPinType::Numeric, "ConservativeDensity", {},
                  MaterialPinFlags::HideValue),
};

constexpr MaterialPinDefinition VolumetricAdvancedInputPins[] = {
    MaterialOutput("ConservativeDensity as Float3", MaterialPinType::Numeric,
                   "ConservativeDensity as Float3"),
    MaterialOutput("ConservativeDensity as Float4", MaterialPinType::Numeric,
                   "ConservativeDensity as Float4"),
};

constexpr MaterialPinDefinition CloudSampleAttributePins[] = {
    MaterialOutput("Altitude", MaterialPinType::Numeric, "Altitude"),
    MaterialOutput("AltitudeInLayer", MaterialPinType::Numeric, "AltitudeInLayer"),
    MaterialOutput("NormAltitudeInLayer", MaterialPinType::Numeric, "NormAltitudeInLayer"),
    MaterialOutput("ShadowSampleDistance", MaterialPinType::Numeric, "ShadowSampleDistance"),
};

constexpr MaterialPinDefinition VolumetricCloudEmptySpaceSkippingInputPins[] = {
    MaterialOutput("Sphere Center", MaterialPinType::Numeric, "Sphere Center"),
    MaterialOutput("Sphere Radius", MaterialPinType::Numeric, "Sphere Radius"),
};

constexpr MaterialPinDefinition VolumetricCloudEmptySpaceSkippingOutputPins[] = {
    MaterialInput("ContainsMatter", MaterialPinType::Numeric, "ContainsMatter", {},
                  MaterialPinFlags::HideValue),
};

constexpr MaterialPinDefinition FirstPersonOutputPins[] = {
    MaterialInput("FirstPersonInterpolationAlpha", MaterialPinType::Numeric,
                  "FirstPersonInterpolationAlpha", MaterialPinDefault::Scalar(1.0f)),
};

constexpr MaterialPinDefinition MotionVectorWorldOffsetOutputPins[] = {
    MaterialInput("Input", MaterialPinType::Float3, "Input", {}, MaterialPinFlags::HideValue),
};

constexpr MaterialPinDefinition TemporalResponsivenessOutputPins[] = {
    MaterialInput("Input", MaterialPinType::Float1, "Input", {}, MaterialPinFlags::HideValue),
};

constexpr MaterialPinDefinition RuntimeVirtualTextureOutputPins[] = {
    MaterialInput("BaseColor", MaterialPinType::Numeric, "BaseColor", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Specular", MaterialPinType::Numeric, "Specular", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Roughness", MaterialPinType::Numeric, "Roughness", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Normal", MaterialPinType::Numeric, "Normal", {}, MaterialPinFlags::HideValue),
    MaterialInput("WorldHeight", MaterialPinType::Numeric, "WorldHeight", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Opacity", MaterialPinType::Numeric, "Opacity", {}, MaterialPinFlags::HideValue),
    MaterialInput("Mask", MaterialPinType::Numeric, "Mask", {}, MaterialPinFlags::HideValue),
    MaterialInput("Displacement", MaterialPinType::Numeric, "Displacement", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Mask4", MaterialPinType::Numeric, "Mask4", {}, MaterialPinFlags::HideValue),
};

constexpr MaterialPinDefinition NeuralNetworkInputPins[] = {
    MaterialInput("Coordinates", MaterialPinType::Float4, "Coordinates", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Input0", MaterialPinType::Float3, "Input0", {}, MaterialPinFlags::HideValue),
    MaterialInput("Mask", MaterialPinType::Float1, "Mask", {}, MaterialPinFlags::HideValue),
};

constexpr MaterialPinDefinition NeuralNetworkOutputPins[] = {
    MaterialInput("Coordinates", MaterialPinType::Numeric, "Coordinates", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("RGBA", MaterialPinType::Float4, "RGBA"),
};

constexpr MaterialNodeDefinition OutputsDefinitions[] = {
    MaterialNode("Material.BentNormalOutput")
        .Pins(BentNormalOutputPins)
        .Menu("Bent Normal Output", "Outputs", "Utility BentNormalCustomOutput",
              "Bent Normal Output."),
    MaterialNode("Material.ClearCoatNormalOutput")
        .Pins(ClearCoatNormalOutputPins)
        .Menu("Clear Coat Normal Output", "Outputs", "Utility ClearCoatNormalCustomOutput",
              "Clear Coat Normal Output."),
    MaterialNode("Material.TangentOutput")
        .Pins(TangentOutputPins)
        .Menu("Tangent output", "Outputs", "Custom", "Tangent output."),
    MaterialNode("Material.ThinTranslucentOutput")
        .Pins(ThinTranslucentOutputPins)
        .Menu("Thin Translucent Output", "Outputs", "ThinTranslucent ThinTranslucentMaterialOutput",
              "Material output expression for setting absorption properties of thin translucent "
              "materials."),
    MaterialNode("Material.AbsorptionMediumOutput")
        .Pins(AbsorptionMediumOutputPins)
        .Menu("Absorption Medium Output", "Outputs", "Shading AbsorptionMediumMaterialOutput",
              "Material output expression for setting absorption properties of solid refractive "
              "glass (for the Path Tracer Only)."),
    MaterialNode("Material.SubsurfaceMediumOutput")
        .Pins(SubsurfaceMediumOutputPins)
        .Menu("Subsurface Medium Output", "Outputs", "Shading SubsurfaceMediumMaterialOutput",
              "Material output expression for setting the mean free path and scattering "
              "distribution properties of subsurface profile (for the Path Tracer Only)."),
    MaterialNode("Material.SingleLayerWaterOutput")
        .Pins(SingleLayerWaterOutputPins)
        .Menu(
            "Single Layer Water Output", "Outputs", "Water SingleLayerWaterMaterialOutput",
            "Material output expression for writing single layer water volume material properties.")
        .Features(ToMask(MaterialContextFeature::Water)),
    MaterialNode("Material.VolumetricAdvancedOutput")
        .Properties(&GeneratedType<VolumetricAdvancedOutputProperties>::Schema)
        .Pins(VolumetricAdvancedOutputPins)
        .Menu("Volumetric Advanced Output", "Outputs", "Volume VolumetricAdvancedMaterialOutput",
              "Material output expression for writing advanced volumetric material properties.")
        .Features(ToMask(MaterialContextFeature::Volume)),
    MaterialNode("Material.VolumetricAdvancedInput")
        .Pins(VolumetricAdvancedInputPins)
        .Menu("Volumetric Advanced Input", "Outputs", "Volume VolumetricAdvancedMaterialInput",
              "Volumetric Advanced Input.")
        .Features(ToMask(MaterialContextFeature::Volume)),
    MaterialNode("Material.CloudSampleAttribute")
        .Pins(CloudSampleAttributePins)
        .Menu("Cloud Sample Attributes", "Outputs", "Volume", "Cloud Sample Attributes.")
        .Features(ToMask(MaterialContextFeature::Volume)),
    MaterialNode("Material.VolumetricCloudEmptySpaceSkippingInput")
        .Pins(VolumetricCloudEmptySpaceSkippingInputPins)
        .Menu("Volumetric Cloud Empty Space Skipping Input <Experimental>", "Outputs", "Volume",
              "Volumetric Cloud Empty Space Skipping Input <Experimental>.")
        .Features(ToMask(MaterialContextFeature::Volume)),
    MaterialNode("Material.VolumetricCloudEmptySpaceSkippingOutput")
        .Pins(VolumetricCloudEmptySpaceSkippingOutputPins)
        .Menu("Volumetric Cloud Empty Space Skipping Output", "Outputs", "Volume",
              "USed to help the cloud system to fast skip empty space areas when ray marching.")
        .Features(ToMask(MaterialContextFeature::Volume)),
    MaterialNode("Material.FirstPersonOutput")
        .Pins(FirstPersonOutputPins)
        .Menu("First Person Output", "Outputs", "First Person",
              "Material output expression for writing first person rendering properties.")
        .Features(ToMask(MaterialContextFeature::FirstPerson)),
    MaterialNode("Material.MotionVectorWorldOffsetOutput")
        .Pins(MotionVectorWorldOffsetOutputPins)
        .Menu("Motion Vector World Offset (Per-Pixel)", "Outputs", "",
              "Material output expression for generating a per-pixel motion vector offset that "
              "will be added to the existing motion vector."),
    MaterialNode("Material.TemporalResponsivenessOutput")
        .Pins(TemporalResponsivenessOutputPins)
        .Menu("Temporal Responsiveness", "Outputs", "", "Temporal Responsiveness."),
    MaterialNode("Material.RuntimeVirtualTextureOutput")
        .Pins(RuntimeVirtualTextureOutputPins)
        .Menu("Runtime Virtual Texture Output", "Outputs", "VirtualTexture",
              "Material output expression for writing to a runtime virtual texture.")
        .Features(ToMask(MaterialContextFeature::RuntimeVirtualTexture)),
    MaterialNode("Material.NeuralNetworkInput")
        .Properties(&GeneratedType<NeuralNetworkInputProperties>::Schema)
        .Pins(NeuralNetworkInputPins)
        .Menu("Neural Input (Texture)", "Outputs", "NeuralNetwork", "Neural Input (Texture).")
        .Features(ToMask(MaterialContextFeature::Neural)),
    MaterialNode("Material.NeuralNetworkOutput")
        .Properties(&GeneratedType<NeuralNetworkOutputProperties>::Schema)
        .Pins(NeuralNetworkOutputPins)
        .Menu("Neural Output (Texture)", "Outputs", "NeuralNetwork", "Neural Output (Texture).")
        .Features(ToMask(MaterialContextFeature::Neural)),
    MaterialNode("Material.MaterialCacheOutput")
        .Properties(&GeneratedType<MaterialCacheOutputProperties>::Schema)
        .DynamicPins(&MaterialCacheDynamicPins)
        .Menu("Material Cache Output", "Outputs", "MaterialCache", "Material Cache Output.")
        .Missing(MaterialPrerequisite::VirtualTexture),
};

} // namespace

Span<const MaterialNodeDefinition> OutputsNodeDefinitions() noexcept {
    return OutputsDefinitions;
}

} // namespace rocks::material::detail
