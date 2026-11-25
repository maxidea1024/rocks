#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/Nodes/Surface/HairAttributesProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/HairAttributesProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceAddProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceAddProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceEyeProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceEyeProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceFromAttributesProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceFromAttributesProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceFromShadingModelProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceFromShadingModelProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceHorizontalMixProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceHorizontalMixProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceSelectProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceSelectProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceSlabProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceSlabProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceToonProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceToonProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceVerticalLayerProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceVerticalLayerProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceVolumetricFogCloudProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceVolumetricFogCloudProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Surface 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition SurfaceSlabPins[] = {
    MaterialInput("DiffuseAlbedo", MaterialPinType::Float3, "Diffuse Albedo", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("F0", MaterialPinType::Float3, "F0", {}, MaterialPinFlags::HideValue),
    MaterialInput("F90", MaterialPinType::Float3, "F90", {}, MaterialPinFlags::HideValue),
    MaterialInput("Roughness", MaterialPinType::Float1, "Roughness", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Anisotropy", MaterialPinType::Float1, "Anisotropy", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Normal", MaterialPinType::Float3, "Normal", {}, MaterialPinFlags::HideValue),
    MaterialInput("Tangent", MaterialPinType::Float3, "Tangent", {}, MaterialPinFlags::HideValue),
    MaterialInput("SSSMFP", MaterialPinType::Float3, "SSS MFP", {}, MaterialPinFlags::HideValue),
    MaterialInput("SSSMFPScale", MaterialPinType::Float1, "SSS MFP Scale", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("SSSPhaseAnisotropy", MaterialPinType::Float1, "SSS Phase Anisotropy", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("EmissiveColor", MaterialPinType::Float3, "Emissive Color", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("SecondRoughness", MaterialPinType::Float1, "Second Roughness", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("SecondRoughnessWeight", MaterialPinType::Float1, "Second Roughness Weight", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("FuzzRoughness", MaterialPinType::Float1, "Fuzz Roughness", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("FuzzAmount", MaterialPinType::Float1, "Fuzz Amount", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("FuzzColor", MaterialPinType::Float3, "Fuzz Color", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("GlintValue", MaterialPinType::Numeric, "Glint Density", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("GlintUV", MaterialPinType::Float2, "Glint UVs", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition SurfaceSimpleClearCoatPins[] = {
    MaterialInput("DiffuseAlbedo", MaterialPinType::Float3, "Diffuse Albedo", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("F0", MaterialPinType::Float3, "F0", {}, MaterialPinFlags::HideValue),
    MaterialInput("Roughness", MaterialPinType::Float1, "Roughness", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("ClearCoatCoverage", MaterialPinType::Float1, "Clear Coat Coverage", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("ClearCoatRoughness", MaterialPinType::Float1, "Clear Coat Roughness", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Normal", MaterialPinType::Float3, "Normal", {}, MaterialPinFlags::HideValue),
    MaterialInput("EmissiveColor", MaterialPinType::Float3, "Emissive Color", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("BottomNormal", MaterialPinType::Float3, "Bottom Normal", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition SurfaceUnlitPins[] = {
    MaterialInput("EmissiveColor", MaterialPinType::Float3, "EmissiveColor", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("TransmittanceColor", MaterialPinType::Float3, "TransmittanceColor", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Normal", MaterialPinType::Float3, "Normal", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition SurfaceHairPins[] = {
    MaterialInput("BaseColor", MaterialPinType::Float3, "BaseColor", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Scatter", MaterialPinType::Float1, "Scatter", {}, MaterialPinFlags::HideValue),
    MaterialInput("Specular", MaterialPinType::Float1, "Specular", {}, MaterialPinFlags::HideValue),
    MaterialInput("Roughness", MaterialPinType::Float1, "Roughness", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Backlit", MaterialPinType::Float1, "Backlit", {}, MaterialPinFlags::HideValue),
    MaterialInput("Tangent", MaterialPinType::Float3, "Tangent", {}, MaterialPinFlags::HideValue),
    MaterialInput("EmissiveColor", MaterialPinType::Float3, "EmissiveColor", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition SurfaceEyePins[] = {
    MaterialInput("DiffuseColor", MaterialPinType::Float3, "DiffuseColor", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Roughness", MaterialPinType::Float1, "Roughness", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("CorneaNormal", MaterialPinType::Float3, "CorneaNormal", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("IrisNormal", MaterialPinType::Float3, "IrisNormal", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("IrisPlaneNormal", MaterialPinType::Float3, "IrisPlaneNormal", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("IrisMask", MaterialPinType::Float1, "IrisMask", {}, MaterialPinFlags::HideValue),
    MaterialInput("IrisDistance", MaterialPinType::Float1, "IrisDistance", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("EmissiveColor", MaterialPinType::Float3, "EmissiveColor", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition SurfaceSingleLayerWaterPins[] = {
    MaterialInput("BaseColor", MaterialPinType::Float3, "BaseColor", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Metallic", MaterialPinType::Float1, "Metallic", {}, MaterialPinFlags::HideValue),
    MaterialInput("Specular", MaterialPinType::Float1, "Specular", {}, MaterialPinFlags::HideValue),
    MaterialInput("Roughness", MaterialPinType::Float1, "Roughness", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Normal", MaterialPinType::Float3, "Normal", {}, MaterialPinFlags::HideValue),
    MaterialInput("EmissiveColor", MaterialPinType::Float3, "EmissiveColor", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("TopMaterialOpacity", MaterialPinType::Float1, "TopMaterialOpacity", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("WaterAlbedo", MaterialPinType::Float3, "WaterAlbedo", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("WaterExtinction", MaterialPinType::Float3, "WaterExtinction", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("WaterPhaseG", MaterialPinType::Float1, "WaterPhaseG", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("ColorScaleBehindWater", MaterialPinType::Float3, "ColorScaleBehindWater", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition SurfaceVolumetricFogCloudPins[] = {
    MaterialInput("Albedo", MaterialPinType::Float3, "Albedo", {}, MaterialPinFlags::HideValue),
    MaterialInput("Extinction", MaterialPinType::Float3, "Extinction", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("EmissiveColor", MaterialPinType::Float3, "EmissiveColor", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("AmbientOcclusion", MaterialPinType::Float1, "AmbientOcclusion", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition SurfaceHorizontalMixPins[] = {
    MaterialInput("Background", MaterialPinType::Surface, "Background", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Foreground", MaterialPinType::Surface, "Foreground", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Mix", MaterialPinType::Float1, "Mix", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition SurfaceVerticalLayerPins[] = {
    MaterialInput("Top", MaterialPinType::Surface, "Top", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Base", MaterialPinType::Surface, "Bottom", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Thickness", MaterialPinType::Numeric, "Top Thickness", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition SurfaceAddPins[] = {
    MaterialInput("A", MaterialPinType::Surface, "A", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("B", MaterialPinType::Surface, "B", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition SurfaceWeightPins[] = {
    MaterialInput("A", MaterialPinType::Surface, "A", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Weight", MaterialPinType::Float1, "Weight", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition SurfaceSelectPins[] = {
    MaterialInput("A", MaterialPinType::Surface, "A", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("B", MaterialPinType::Surface, "B", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("SelectValue", MaterialPinType::Float1, "SelectValue", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition SurfaceFromShadingModelPins[] = {
    MaterialInput("BaseColor", MaterialPinType::Float3, "BaseColor", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Metallic", MaterialPinType::Float1, "Metallic", {}, MaterialPinFlags::HideValue),
    MaterialInput("Specular", MaterialPinType::Float1, "Specular", {}, MaterialPinFlags::HideValue),
    MaterialInput("Roughness", MaterialPinType::Float1, "Roughness", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Anisotropy", MaterialPinType::Float1, "Anisotropy", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("EmissiveColor", MaterialPinType::Float3, "Emissive Color", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Normal", MaterialPinType::Float3, "Normal", {}, MaterialPinFlags::HideValue),
    MaterialInput("Tangent", MaterialPinType::Float3, "Tangent", {}, MaterialPinFlags::HideValue),
    MaterialInput("SubSurfaceColor", MaterialPinType::Float3, "Subsurface Color", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("ClearCoat", MaterialPinType::Float1, "Clear Coat", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("ClearCoatRoughness", MaterialPinType::Float1, "Clear Coat Roughness", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Opacity", MaterialPinType::Float1, "Opacity", {}, MaterialPinFlags::HideValue),
    MaterialInput("TransmittanceColor", MaterialPinType::Float3,
                  "Thin Translucent Transmittance Color", {}, MaterialPinFlags::HideValue),
    MaterialInput("WaterScatteringCoefficients", MaterialPinType::Float3,
                  "Water Scattering Coefficients", {}, MaterialPinFlags::HideValue),
    MaterialInput("WaterAbsorptionCoefficients", MaterialPinType::Float3,
                  "Water Absorption Coefficients", {}, MaterialPinFlags::HideValue),
    MaterialInput("WaterPhaseG", MaterialPinType::Float1, "Water Phase G", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("ColorScaleBehindWater", MaterialPinType::Float3, "Color Scale BehindWater", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("ClearCoatNormal", MaterialPinType::Float3, "Clear Coat Bottom Normal", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("CustomTangent", MaterialPinType::Float3, "Custom Tangent", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("ThinTranslucentSurfaceCoverage", MaterialPinType::Float1,
                  "Thin Translucent Surface Coverage", {}, MaterialPinFlags::HideValue),
    MaterialInput("ShadingModel", MaterialPinType::ShadingModel, "Single Shading Model", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition SurfaceFromAttributesPins[] = {
    MaterialInput("MaterialAttributes", MaterialPinType::MaterialAttributes, "Attributes", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("WaterScatteringCoefficients", MaterialPinType::Float3,
                  "Water Scattering Coefficients (Water)", {}, MaterialPinFlags::HideValue),
    MaterialInput("WaterAbsorptionCoefficients", MaterialPinType::Float3,
                  "Water Absorption Coefficients (Water)", {}, MaterialPinFlags::HideValue),
    MaterialInput("WaterPhaseG", MaterialPinType::Float1, "Water Phase G (Water)", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("ColorScaleBehindWater", MaterialPinType::Float3,
                  "Color Scale BehindWater (Water)", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition SurfaceLightFunctionPins[] = {
    MaterialInput("Color", MaterialPinType::Float3, "Color", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition SurfacePostProcessPins[] = {
    MaterialInput("Color", MaterialPinType::Float3, "Color", {}, MaterialPinFlags::HideValue),
    MaterialInput("Opacity", MaterialPinType::Float1, "Opacity", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition SurfaceUserInterfacePins[] = {
    MaterialInput("Color", MaterialPinType::Float3, "Color", {}, MaterialPinFlags::HideValue),
    MaterialInput("Opacity", MaterialPinType::Numeric, "Opacity", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition SurfaceToDecalPins[] = {
    MaterialInput("DecalMaterial", MaterialPinType::Surface, "DecalMaterial", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Coverage", MaterialPinType::Float1, "Coverage", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition TransmittanceToMeanFreePathPins[] = {
    MaterialInput("TransmittanceColor", MaterialPinType::Float3, "TransmittanceColor", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Thickness", MaterialPinType::Float1, "Thickness", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("MFP", MaterialPinType::Float3, "MFP"),
    MaterialOutput("ThicknessOut", MaterialPinType::Float1, "Thickness"),
};

constexpr MaterialPinDefinition MetalnessToDiffuseAlbedoF0Pins[] = {
    MaterialInput("BaseColor", MaterialPinType::Float3, "BaseColor", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Metallic", MaterialPinType::Float1, "Metallic", {}, MaterialPinFlags::HideValue),
    MaterialInput("Specular", MaterialPinType::Float1, "Specular", {}, MaterialPinFlags::HideValue),
    MaterialOutput("DiffuseAlbedo", MaterialPinType::Float3, "DiffuseAlbedo"),
    MaterialOutput("F0", MaterialPinType::Float3, "F0"),
};

constexpr MaterialPinDefinition HazinessToSecondaryRoughnessPins[] = {
    MaterialInput("BaseRoughness", MaterialPinType::Float1, "BaseRoughness", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Haziness", MaterialPinType::Float1, "Haziness", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Second Roughness", MaterialPinType::Float1, "Second Roughness"),
    MaterialOutput("Second Roughness Weight", MaterialPinType::Float1, "Second Roughness Weight"),
};

constexpr MaterialPinDefinition ThinFilmPins[] = {
    MaterialInput("Normal", MaterialPinType::Float3, "Normal", {}, MaterialPinFlags::HideValue),
    MaterialInput("F0", MaterialPinType::Float3, "F0", {}, MaterialPinFlags::HideValue),
    MaterialInput("F90", MaterialPinType::Float3, "F90", {}, MaterialPinFlags::HideValue),
    MaterialInput("Thickness", MaterialPinType::Float1, "Thickness", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("IOR", MaterialPinType::Float1, "IOR", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Specular Color", MaterialPinType::Float3, "Specular Color"),
    MaterialOutput("Edge Specular Color", MaterialPinType::Float3, "Edge Specular Color"),
};

constexpr MaterialPinDefinition SurfaceToonPins[] = {
    MaterialInput("BaseColor", MaterialPinType::Float3, "BaseColor", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Metallic", MaterialPinType::Float1, "Metallic", {}, MaterialPinFlags::HideValue),
    MaterialInput("Specular", MaterialPinType::Float1, "Specular", {}, MaterialPinFlags::HideValue),
    MaterialInput("Roughness", MaterialPinType::Float1, "Roughness", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Normal", MaterialPinType::Float3, "Normal", {}, MaterialPinFlags::HideValue),
    MaterialInput("EmissiveColor", MaterialPinType::Float3, "EmissiveColor", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("PatternUVs", MaterialPinType::Float2, "PatternUVs", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Anisotropy", MaterialPinType::Float1, "Anisotropy", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Tangent", MaterialPinType::Float3, "Tangent", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Surface, ""),
};

constexpr MaterialPinDefinition HairAttributesPins[] = {
    MaterialOutput("U", MaterialPinType::Float1, "U"),
    MaterialOutput("V", MaterialPinType::Float1, "V"),
    MaterialOutput("Length", MaterialPinType::Float1, "Length"),
    MaterialOutput("Radius", MaterialPinType::Float1, "Radius"),
    MaterialOutput("Seed", MaterialPinType::Float1, "Seed"),
    MaterialOutput("Tangent", MaterialPinType::Float3, "Tangent"),
    MaterialOutput("Root UV", MaterialPinType::Float2, "Root UV"),
    MaterialOutput("BaseColor", MaterialPinType::Float3, "BaseColor"),
    MaterialOutput("Roughness", MaterialPinType::Float1, "Roughness"),
    MaterialOutput("Depth", MaterialPinType::Float1, "Depth"),
    MaterialOutput("Coverage", MaterialPinType::Float1, "Coverage"),
    MaterialOutput("AuxilaryData", MaterialPinType::Float4, "AuxilaryData"),
    MaterialOutput("AtlasUVs", MaterialPinType::Float2, "AtlasUVs"),
    MaterialOutput("Group Index", MaterialPinType::Float1, "Group Index"),
    MaterialOutput("AO", MaterialPinType::Float1, "AO"),
    MaterialOutput("Clump ID", MaterialPinType::Float1, "Clump ID"),
};

constexpr MaterialPinDefinition HairColorPins[] = {
    MaterialInput("Melanin", MaterialPinType::Numeric, "Melanin", {}, MaterialPinFlags::HideValue),
    MaterialInput("Redness", MaterialPinType::Numeric, "Redness", {}, MaterialPinFlags::HideValue),
    MaterialInput("DyeColor", MaterialPinType::Numeric, "DyeColor", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Color", MaterialPinType::Float3, "Color"),
};

constexpr MaterialNodeDefinition SurfaceDefinitions[] = {
    MaterialNode("Material.SurfaceSlab")
        .Properties(&GeneratedType<SurfaceSlabProperties>::Schema)
        .Pins(SurfaceSlabPins)
        .Menu("Surface Slab", "Surface", "SubstrateSlabBSDF", "Surface Slab."),
    MaterialNode("Material.SurfaceSimpleClearCoat")
        .Pins(SurfaceSimpleClearCoatPins)
        .Menu("Surface Simple Clear Coat", "Surface", "SubstrateSimpleClearCoatBSDF",
              "Surface Simple Clear Coat."),
    MaterialNode("Material.SurfaceUnlit")
        .Pins(SurfaceUnlitPins)
        .Menu("Surface Unlit", "Surface", "SubstrateUnlitBSDF", "Surface Unlit."),
    MaterialNode("Material.SurfaceHair")
        .Pins(SurfaceHairPins)
        .Menu("Surface Hair", "Surface", "SubstrateHairBSDF", "Surface Hair."),
    MaterialNode("Material.SurfaceEye")
        .Properties(&GeneratedType<SurfaceEyeProperties>::Schema)
        .Pins(SurfaceEyePins)
        .Menu("Surface Eye", "Surface", "SubstrateEyeBSDF", "Surface Eye."),
    MaterialNode("Material.SurfaceSingleLayerWater")
        .Pins(SurfaceSingleLayerWaterPins)
        .Menu("Surface Single Layer Water", "Surface", "SubstrateSingleLayerWaterBSDF",
              "Surface Single Layer Water."),
    MaterialNode("Material.SurfaceVolumetricFogCloud")
        .Properties(&GeneratedType<SurfaceVolumetricFogCloudProperties>::Schema)
        .Pins(SurfaceVolumetricFogCloudPins)
        .Menu("Surface Volumetric Fog Cloud", "Surface", "SubstrateVolumetricFogCloudBSDF",
              "Surface Volumetric Fog Cloud."),
    MaterialNode("Material.SurfaceHorizontalMix")
        .Properties(&GeneratedType<SurfaceHorizontalMixProperties>::Schema)
        .Pins(SurfaceHorizontalMixPins)
        .Menu("Surface Horizontal Mix", "Surface", "SubstrateHorizontalMixing",
              "Surface Horizontal Mix."),
    MaterialNode("Material.SurfaceVerticalLayer")
        .Properties(&GeneratedType<SurfaceVerticalLayerProperties>::Schema)
        .Pins(SurfaceVerticalLayerPins)
        .Menu("Surface Vertical Layer", "Surface", "SubstrateVerticalLayering",
              "Surface Vertical Layer."),
    MaterialNode("Material.SurfaceAdd")
        .Properties(&GeneratedType<SurfaceAddProperties>::Schema)
        .Pins(SurfaceAddPins)
        .Menu("Surface Add", "Surface", "SubstrateAdd", "Surface Add."),
    MaterialNode("Material.SurfaceWeight")
        .Pins(SurfaceWeightPins)
        .Menu("Surface Weight", "Surface", "SubstrateWeight", "Surface Weight."),
    MaterialNode("Material.SurfaceSelect")
        .Properties(&GeneratedType<SurfaceSelectProperties>::Schema)
        .Pins(SurfaceSelectPins)
        .Menu("Surface Select", "Surface", "SubstrateSelect", "Surface Select."),
    MaterialNode("Material.SurfaceFromShadingModel")
        .Properties(&GeneratedType<SurfaceFromShadingModelProperties>::Schema)
        .Pins(SurfaceFromShadingModelPins)
        .Menu("Surface From Shading Model", "Surface", "SubstrateShadingModels",
              "Surface From Shading Model."),
    MaterialNode("Material.SurfaceFromAttributes")
        .Properties(&GeneratedType<SurfaceFromAttributesProperties>::Schema)
        .Pins(SurfaceFromAttributesPins)
        .Menu("Surface From Attributes", "Surface", "SubstrateConvertMaterialAttributes",
              "Surface From Attributes."),
    MaterialNode("Material.SurfaceLightFunction")
        .Pins(SurfaceLightFunctionPins)
        .Menu("Surface Light Function", "Surface", "SubstrateLightFunction",
              "Surface Light Function."),
    MaterialNode("Material.SurfacePostProcess")
        .Pins(SurfacePostProcessPins)
        .Menu("Surface Post Process", "Surface", "SubstratePostProcess", "Surface Post Process."),
    MaterialNode("Material.SurfaceUserInterface")
        .Pins(SurfaceUserInterfacePins)
        .Menu("Surface User Interface", "Surface", "SubstrateUI", "Surface User Interface."),
    MaterialNode("Material.SurfaceToDecal")
        .Pins(SurfaceToDecalPins)
        .Menu("Surface To Decal", "Surface", "SubstrateConvertToDecal", "Surface To Decal."),
    MaterialNode("Material.TransmittanceToMeanFreePath")
        .Pins(TransmittanceToMeanFreePathPins)
        .Menu("Transmittance To Mean Free Path", "Surface", "SubstrateTransmittanceToMFP",
              "Transmittance To Mean Free Path."),
    MaterialNode("Material.MetalnessToDiffuseAlbedoF0")
        .Pins(MetalnessToDiffuseAlbedoF0Pins)
        .Menu("Metalness To Diffuse Albedo F0", "Surface", "SubstrateMetalnessToDiffuseAlbedoF0",
              "Metalness To Diffuse Albedo F0."),
    MaterialNode("Material.HazinessToSecondaryRoughness")
        .Pins(HazinessToSecondaryRoughnessPins)
        .Menu("Haziness To Secondary Roughness", "Surface", "SubstrateHazinessToSecondaryRoughness",
              "Haziness To Secondary Roughness."),
    MaterialNode("Material.ThinFilm")
        .Pins(ThinFilmPins)
        .Menu("Thin Film", "Surface", "SubstrateThinFilm", "Thin Film."),
    MaterialNode("Material.SurfaceToon")
        .Properties(&GeneratedType<SurfaceToonProperties>::Schema)
        .Pins(SurfaceToonPins)
        .Menu("Surface Toon", "Surface", "SubstrateToonBSDF", "Surface Toon.")
        .Flags(MaterialNodeFlags::Experimental)
        .Missing(MaterialPrerequisite::ToonShading),
    MaterialNode("Material.HairAttributes")
        .Properties(&GeneratedType<HairAttributesProperties>::Schema)
        .Pins(HairAttributesPins)
        .Menu("Hair Attributes", "Surface", "Hair Attributes", "Hair Attributes.")
        .Features(ToMask(MaterialContextFeature::Hair)),
    MaterialNode("Material.HairColor")
        .Pins(HairColorPins)
        .Menu("Hair Color", "Surface", "Hair Color", "Hair Color."),
};

} // namespace

Span<const MaterialNodeDefinition> SurfaceNodeDefinitions() noexcept {
    return SurfaceDefinitions;
}

} // namespace rocks::material::detail
