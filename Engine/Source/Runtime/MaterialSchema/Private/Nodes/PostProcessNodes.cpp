#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/Nodes/PostProcess/DepthOfFieldFunctionProperties.generated.h>
#include <MaterialSchema/Nodes/PostProcess/DepthOfFieldFunctionProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Post Process 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition EyeAdaptationPins[] = {
    MaterialOutput("EyeAdaptation", MaterialPinType::Numeric, "EyeAdaptation"),
};

constexpr MaterialPinDefinition EyeAdaptationInversePins[] = {
    MaterialInput("LightValueInput", MaterialPinType::Numeric, "LightValueInput", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("AlphaInput", MaterialPinType::Numeric, "AlphaInput", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("EyeAdaptationInverse", MaterialPinType::Numeric, "EyeAdaptationInverse"),
};

constexpr MaterialPinDefinition DepthOfFieldFunctionPins[] = {
    MaterialInput("Depth", MaterialPinType::Numeric, "Depth", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition PostProcessVolumeFlagTestPins[] = {
    MaterialInput("BitIndex", MaterialPinType::Numeric, "BitIndex",
                  MaterialPinDefault::Scalar(0.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition PostProcessDefinitions[] = {
    MaterialNode("Material.EyeAdaptation")
        .Pins(EyeAdaptationPins)
        .Menu("Eye Adaptation", "Post Process", "Utility",
              "Provides access to the EyeAdaptation render target.")
        .Features(ToMask(MaterialContextFeature::PostProcess)),
    MaterialNode("Material.EyeAdaptationInverse")
        .Pins(EyeAdaptationInversePins)
        .Menu("Eye Adaptation Inverse", "Post Process", "Utility",
              "Provides access to the EyeAdaptation render target.")
        .Features(ToMask(MaterialContextFeature::PostProcess)),
    MaterialNode("Material.DepthOfFieldFunction")
        .Properties(&GeneratedType<DepthOfFieldFunctionProperties>::Schema)
        .Pins(DepthOfFieldFunctionPins)
        .Menu("Depth Of Field Function", "Post Process", "Utility", "Depth Of Field Function.")
        .Features(ToMask(MaterialContextFeature::PostProcess)),
    MaterialNode("Material.PostProcessVolumeFlagTest")
        .Pins(PostProcessVolumeFlagTestPins)
        .Menu("Post Process Volume Flag Test", "Post Process",
              "Get Post Process Setting PostVolumeUserFlagTest", "Post Process Volume Flag Test.")
        .Features(ToMask(MaterialContextFeature::PostProcess)),
};

} // namespace

Span<const MaterialNodeDefinition> PostProcessNodeDefinitions() noexcept {
    return PostProcessDefinitions;
}

} // namespace rocks::material::detail
