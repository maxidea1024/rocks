#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/Nodes/SpeedTree/SpeedTreeProperties.generated.h>
#include <MaterialSchema/Nodes/SpeedTree/SpeedTreeProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// SpeedTree 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition SpeedTreePins[] = {
    MaterialInput("GeometryInput", MaterialPinType::Numeric, "GeometryInput", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("WindInput", MaterialPinType::Numeric, "WindInput", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("LODInput", MaterialPinType::Numeric, "LODInput", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("ExtraBendWS", MaterialPinType::Numeric, "ExtraBendWS", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition SpeedTreeDefinitions[] = {
    MaterialNode("Material.SpeedTree")
        .Properties(&GeneratedType<SpeedTreeProperties>::Schema)
        .Pins(SpeedTreePins)
        .Menu("SpeedTree", "SpeedTree", "SpeedTree", "SpeedTree.")
        .Features(ToMask(MaterialContextFeature::SpeedTree)),
};

} // namespace

Span<const MaterialNodeDefinition> SpeedTreeNodeDefinitions() noexcept {
    return SpeedTreeDefinitions;
}

} // namespace rocks::material::detail
