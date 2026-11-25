#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/Nodes/Depth/SceneDepthProperties.generated.h>
#include <MaterialSchema/Nodes/Depth/SceneDepthProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Depth 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition SceneDepthPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input",
                  MaterialPinDefault::Vector2(2.0f, 0.0f)),
    MaterialOutput("Result", MaterialPinType::Float1, ""),
};

constexpr MaterialPinDefinition DepthFadePins[] = {
    MaterialInput("InOpacity", MaterialPinType::Numeric, "InOpacity",
                  MaterialPinDefault::Scalar(1.0f)),
    MaterialInput("FadeDistance", MaterialPinType::Numeric, "FadeDistance",
                  MaterialPinDefault::Scalar(100.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition DepthDefinitions[] = {
    MaterialNode("Material.SceneDepth")
        .Properties(&GeneratedType<SceneDepthProperties>::Schema)
        .Pins(SceneDepthPins)
        .Menu("Scene Depth", "Depth", "Depth", "Scene Depth.")
        .Features(ToMask(MaterialContextFeature::Scene)),
    MaterialNode("Material.DepthFade")
        .Pins(DepthFadePins)
        .Menu("Depth Fade", "Depth", "Depth Utility",
              "Scales Opacity by a Linear fade based on SceneDepth, from 0 at PixelDepth to 1 at "
              "FadeDistance")
        .Features(ToMask(MaterialContextFeature::Scene)),
};

} // namespace

Span<const MaterialNodeDefinition> DepthNodeDefinitions() noexcept {
    return DepthDefinitions;
}

} // namespace rocks::material::detail
