#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/Nodes/Utility/NamedRerouteDeclarationProperties.generated.h>
#include <MaterialSchema/Nodes/Utility/NamedRerouteDeclarationProperties.h>
#include <MaterialSchema/Nodes/Utility/NamedRerouteUsageProperties.generated.h>
#include <MaterialSchema/Nodes/Utility/NamedRerouteUsageProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Utility 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPropertyTextDefault NamedRerouteDeclarationTextDefaults[] = {{"name", "Name"}};
constexpr MaterialPinDefinition NamedRerouteDeclarationPins[] = {
    MaterialInput("Input", MaterialPinType::Any, "Input", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Any, ""),
};

constexpr MaterialPinDefinition NamedRerouteUsagePins[] = {
    MaterialOutput("Result", MaterialPinType::Any, ""),
};

constexpr MaterialPinDefinition VertexInterpolatorPins[] = {
    MaterialInput("Input", MaterialPinType::Any, "Input", {}, MaterialPinFlags::HideValue),
    MaterialOutput("PS", MaterialPinType::Any, "PS"),
};

constexpr MaterialNodeDefinition UtilityDefinitions[] = {
    MaterialNode("Material.NamedRerouteDeclaration")
        .Properties(&GeneratedType<NamedRerouteDeclarationProperties>::Schema)
        .TextDefaults(NamedRerouteDeclarationTextDefaults)
        .Pins(NamedRerouteDeclarationPins)
        .Menu("Named Reroute Declaration", "Utility", "Utility", "Named Reroute Declaration."),
    MaterialNode("Material.NamedRerouteUsage")
        .Properties(&GeneratedType<NamedRerouteUsageProperties>::Schema)
        .Pins(NamedRerouteUsagePins)
        .Menu("Named Reroute Usage", "Utility", "", "Named Reroute Usage."),
    MaterialNode("Material.VertexInterpolator")
        .Pins(VertexInterpolatorPins)
        .Menu("Vertex Interpolator", "Utility", "Utility", "Vertex Interpolator."),
};

} // namespace

Span<const MaterialNodeDefinition> UtilityNodeDefinitions() noexcept {
    return UtilityDefinitions;
}

} // namespace rocks::material::detail
