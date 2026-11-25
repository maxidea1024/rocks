#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/Nodes/Switches/FormatFactSwitchProperties.generated.h>
#include <MaterialSchema/Nodes/Switches/FormatFactSwitchProperties.h>
#include <MaterialSchema/Nodes/Switches/SamplerBudgetSwitchProperties.generated.h>
#include <MaterialSchema/Nodes/Switches/SamplerBudgetSwitchProperties.h>
#include <MaterialSchema/Nodes/Switches/StaticSwitchProperties.generated.h>
#include <MaterialSchema/Nodes/Switches/StaticSwitchProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Switches 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition StaticSwitchPins[] = {
    MaterialInput("A", MaterialPinType::Any, "True", {}, MaterialPinFlags::HideValue),
    MaterialInput("B", MaterialPinType::Any, "False", {}, MaterialPinFlags::HideValue),
    MaterialInput("Value", MaterialPinType::StaticBool, "Value", {},
                  MaterialPinFlags::StaticOnly | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Any, ""),
};

constexpr MaterialPinDefinition QualitySwitchPins[] = {
    MaterialInput("Default", MaterialPinType::Numeric, "Default", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Low", MaterialPinType::Numeric, "Low", {}, MaterialPinFlags::HideValue),
    MaterialInput("High", MaterialPinType::Numeric, "High", {}, MaterialPinFlags::HideValue),
    MaterialInput("Medium", MaterialPinType::Numeric, "Medium", {}, MaterialPinFlags::HideValue),
    MaterialInput("Epic", MaterialPinType::Numeric, "Epic", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition RenderPathSwitchPins[] = {
    MaterialInput("Default", MaterialPinType::Numeric, "Default", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Deferred", MaterialPinType::Numeric, "Deferred", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Forward", MaterialPinType::Numeric, "Forward+", {}, MaterialPinFlags::HideValue),
    MaterialInput("Mobile", MaterialPinType::Numeric, "Mobile", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition CapabilityTierSwitchPins[] = {
    MaterialInput("Default", MaterialPinType::Numeric, "Default", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("ES3_1", MaterialPinType::Numeric, "Mobile", {}, MaterialPinFlags::HideValue),
    MaterialInput("SM5", MaterialPinType::Numeric, "SM5", {}, MaterialPinFlags::HideValue),
    MaterialInput("SM6", MaterialPinType::Numeric, "SM6", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition StageSwitchPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition FormatFactSwitchPins[] = {
    MaterialInput("InputTrue", MaterialPinType::Numeric, "True", {}, MaterialPinFlags::HideValue),
    MaterialInput("InputFalse", MaterialPinType::Numeric, "False", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SamplerBudgetSwitchPins[] = {
    MaterialInput("InputTrue", MaterialPinType::Numeric, "Within platform limit", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("InputFalse", MaterialPinType::Numeric, "Over platform limit", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition BindlessFeatureSwitchPins[] = {
    MaterialInput("Default", MaterialPinType::Numeric, "Default", {}, MaterialPinFlags::HideValue),
    MaterialInput("Bindless", MaterialPinType::Numeric, "Bindless", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition PreviousFrameSwitchPins[] = {
    MaterialInput("CurrentFrame", MaterialPinType::Any, "Current Frame", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("PreviousFrame", MaterialPinType::Any, "PreviousFrame", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Any, ""),
};

constexpr MaterialPinDefinition ReflectionCaptureSwitchPins[] = {
    MaterialInput("Default", MaterialPinType::Numeric, "Default", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Reflection", MaterialPinType::Numeric, "Reflection", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ShadowPassSwitchPins[] = {
    MaterialInput("Default", MaterialPinType::Numeric, "Default", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Shadow", MaterialPinType::Numeric, "Shadow", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition RayTracingQualitySwitchPins[] = {
    MaterialInput("Normal", MaterialPinType::Any, "Normal", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("RayTraced", MaterialPinType::Any, "RayTraced", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Any, ""),
};

constexpr MaterialPinDefinition PathTracingQualitySwitchPins[] = {
    MaterialInput("Normal", MaterialPinType::Any, "Normal", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("PathTraced", MaterialPinType::Any, "PathTraced", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Any, ""),
};

constexpr MaterialPinDefinition PathTracingRayTypeSwitchPins[] = {
    MaterialInput("Main", MaterialPinType::Numeric, "Main", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Shadow", MaterialPinType::Numeric, "Shadow", {}, MaterialPinFlags::HideValue),
    MaterialInput("IndirectDiffuse", MaterialPinType::Numeric, "IndirectDiffuse", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("IndirectSpecular", MaterialPinType::Numeric, "IndirectSpecular", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("IndirectVolume", MaterialPinType::Numeric, "IndirectVolume", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition GlobalIlluminationPassSwitchPins[] = {
    MaterialInput("Default", MaterialPinType::Numeric, "Default", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("StaticIndirect", MaterialPinType::Numeric, "StaticIndirect", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("DynamicIndirect", MaterialPinType::Numeric, "DynamicIndirect", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition LightBakePassSwitchPins[] = {
    MaterialInput("Realtime", MaterialPinType::Numeric, "Realtime", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Lightmass", MaterialPinType::Numeric, "LightBake", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ProxyBakePassSwitchPins[] = {
    MaterialInput("Realtime", MaterialPinType::Numeric, "Realtime", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("MaterialProxy", MaterialPinType::Numeric, "MaterialProxy", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition VirtualizedGeometryPassSwitchPins[] = {
    MaterialInput("Default", MaterialPinType::Numeric, "Default", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Nanite", MaterialPinType::Numeric, "VirtualizedGeometry", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition DistanceFieldFeatureSwitchPins[] = {
    MaterialInput("No", MaterialPinType::Numeric, "No", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Yes", MaterialPinType::Numeric, "Yes", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition VirtualTextureFeatureSwitchPins[] = {
    MaterialInput("No", MaterialPinType::Numeric, "No", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Yes", MaterialPinType::Numeric, "Yes", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition RuntimeVirtualTexturePassSwitchPins[] = {
    MaterialInput("Default", MaterialPinType::Numeric, "Default", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("VirtualTextureOutput", MaterialPinType::Numeric, "VirtualTextureOutput", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition MeshPaintTextureSwitchPins[] = {
    MaterialInput("Default", MaterialPinType::Numeric, "Default", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("MeshPaintTexture", MaterialPinType::Numeric, "MeshPaintTexture", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition SwitchesDefinitions[] = {
    MaterialNode("Material.StaticSwitch")
        .Properties(&GeneratedType<StaticSwitchProperties>::Schema)
        .Pins(StaticSwitchPins)
        .Menu("Static Switch", "Switches", "Functions", "Static Switch."),
    MaterialNode("Material.QualitySwitch")
        .Pins(QualitySwitchPins)
        .Menu("Quality Switch", "Switches", "Utility", "Quality Switch."),
    MaterialNode("Material.RenderPathSwitch")
        .Pins(RenderPathSwitchPins)
        .Menu("Render Path Switch", "Switches", "Utility ShadingPathSwitch", "Render Path Switch."),
    MaterialNode("Material.CapabilityTierSwitch")
        .Pins(CapabilityTierSwitchPins)
        .Menu("Capability Tier Switch", "Switches", "Utility FeatureLevelSwitch",
              "Capability Tier Switch."),
    MaterialNode("Material.StageSwitch")
        .Pins(StageSwitchPins)
        .Menu("Stage Switch", "Switches", "Utility ShaderStageSwitch", "Stage Switch."),
    MaterialNode("Material.FormatFactSwitch")
        .Properties(&GeneratedType<FormatFactSwitchProperties>::Schema)
        .Pins(FormatFactSwitchPins)
        .Menu("Format Fact Switch", "Switches", "Utility DataDrivenShaderPlatformInfoSwitch",
              "Format Fact Switch."),
    MaterialNode("Material.SamplerBudgetSwitch")
        .Properties(&GeneratedType<SamplerBudgetSwitchProperties>::Schema)
        .Pins(SamplerBudgetSwitchPins)
        .Menu("Sampler Budget Switch", "Switches", "Utility RequiredSamplersSwitch",
              "Sampler Budget Switch."),
    MaterialNode("Material.BindlessFeatureSwitch")
        .Pins(BindlessFeatureSwitchPins)
        .Menu("Bindless Feature Switch", "Switches", "Utility BindlessSwitch",
              "Bindless Feature Switch."),
    MaterialNode("Material.PreviousFrameSwitch")
        .Pins(PreviousFrameSwitchPins)
        .Menu("Previous Frame Switch", "Switches", "Functions", "Previous Frame Switch."),
    MaterialNode("Material.ReflectionCaptureSwitch")
        .Pins(ReflectionCaptureSwitchPins)
        .Menu("Reflection Capture Switch", "Switches", "Utility ReflectionCapturePassSwitch",
              "Reflection Capture Switch."),
    MaterialNode("Material.ShadowPassSwitch")
        .Pins(ShadowPassSwitchPins)
        .Menu("Shadow Pass Switch", "Switches", "Utility ShadowReplace", "Shadow Pass Switch."),
    MaterialNode("Material.RayTracingQualitySwitch")
        .Pins(RayTracingQualitySwitchPins)
        .Menu("RayTracingQualitySwitchReplace", "Switches", "Utility",
              "RayTracingQualitySwitchReplace."),
    MaterialNode("Material.PathTracingQualitySwitch")
        .Pins(PathTracingQualitySwitchPins)
        .Menu("PathTracingQualitySwitchReplace", "Switches", "Utility",
              "PathTracingQualitySwitchReplace."),
    MaterialNode("Material.PathTracingRayTypeSwitch")
        .Pins(PathTracingRayTypeSwitchPins)
        .Menu("PathTracingRayTypeSwitch", "Switches", "Utility", "PathTracingRayTypeSwitch."),
    MaterialNode("Material.GlobalIlluminationPassSwitch")
        .Pins(GlobalIlluminationPassSwitchPins)
        .Menu("Global Illumination Pass Switch", "Switches", "Utility GIReplace",
              "Global Illumination Pass Switch."),
    MaterialNode("Material.LightBakePassSwitch")
        .Pins(LightBakePassSwitchPins)
        .Menu("Light Bake Pass Switch", "Switches", "Utility LightmassReplace",
              "Light Bake Pass Switch."),
    MaterialNode("Material.ProxyBakePassSwitch")
        .Pins(ProxyBakePassSwitchPins)
        .Menu("Proxy Bake Pass Switch", "Switches", "Utility MaterialProxyReplace",
              "Proxy Bake Pass Switch."),
    MaterialNode("Material.VirtualizedGeometryPassSwitch")
        .Pins(VirtualizedGeometryPassSwitchPins)
        .Menu("Virtualized Geometry Pass Switch", "Switches", "Utility NaniteReplace",
              "Virtualized Geometry Pass Switch."),
    MaterialNode("Material.DistanceFieldFeatureSwitch")
        .Pins(DistanceFieldFeatureSwitchPins)
        .Menu("Distance Field Feature Switch", "Switches",
              "DistanceFields DistanceFieldsRenderingSwitch",
              "Material output expression to switch logic according to whether distance fields "
              "renderering is supported on this project and feature level."),
    MaterialNode("Material.VirtualTextureFeatureSwitch")
        .Pins(VirtualTextureFeatureSwitchPins)
        .Menu("VirtualTextureFeatureSwitch", "Switches", "VirtualTexture",
              "Material output expression to switch logic according to whether virtual texturing "
              "is supported on this project, platform and feature level."),
    MaterialNode("Material.RuntimeVirtualTexturePassSwitch")
        .Pins(RuntimeVirtualTexturePassSwitchPins)
        .Menu("Runtime Virtual Texture Pass Switch", "Switches",
              "VirtualTexture RuntimeVirtualTextureReplace",
              "Material output expression to switch logic for a path that renders to runtime "
              "virtual texture pages."),
    MaterialNode("Material.MeshPaintTextureSwitch")
        .Pins(MeshPaintTextureSwitchPins)
        .Menu("Mesh Paint Texture Switch", "Switches", "MeshPaintTexture MeshPaintTextureReplace",
              "Mesh Paint Texture Switch."),
};

} // namespace

Span<const MaterialNodeDefinition> SwitchesNodeDefinitions() noexcept {
    return SwitchesDefinitions;
}

} // namespace rocks::material::detail
