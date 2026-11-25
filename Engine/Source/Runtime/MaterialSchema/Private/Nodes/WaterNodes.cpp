#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/Nodes/Water/SceneDepthWithoutWaterProperties.generated.h>
#include <MaterialSchema/Nodes/Water/SceneDepthWithoutWaterProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Water 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition SceneDepthWithoutWaterPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input",
                  MaterialPinDefault::Vector2(0.0f, 0.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition WaterDefinitions[] = {
    MaterialNode("Material.SceneDepthWithoutWater")
        .Properties(&GeneratedType<SceneDepthWithoutWaterProperties>::Schema)
        .Pins(SceneDepthWithoutWaterPins)
        .Menu("Scene Depth Without Water", "Water", "Water", "Scene Depth Without Water.")
        .Features(ToMask(MaterialContextFeature::Water)),
};

} // namespace

Span<const MaterialNodeDefinition> WaterNodeDefinitions() noexcept {
    return WaterDefinitions;
}

} // namespace rocks::material::detail
