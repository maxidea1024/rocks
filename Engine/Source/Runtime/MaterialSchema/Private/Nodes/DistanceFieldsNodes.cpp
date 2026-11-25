#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/Nodes/DistanceFields/DistanceFieldApproxAoProperties.generated.h>
#include <MaterialSchema/Nodes/DistanceFields/DistanceFieldApproxAoProperties.h>
#include <MaterialSchema/Nodes/DistanceFields/DistanceFieldGradientProperties.generated.h>
#include <MaterialSchema/Nodes/DistanceFields/DistanceFieldGradientProperties.h>
#include <MaterialSchema/Nodes/DistanceFields/DistanceToNearestSurfaceProperties.generated.h>
#include <MaterialSchema/Nodes/DistanceFields/DistanceToNearestSurfaceProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Distance Fields 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition DistanceToNearestSurfacePins[] = {
    MaterialInput("Position", MaterialPinType::Numeric, "Position", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition DistanceFieldGradientPins[] = {
    MaterialInput("Position", MaterialPinType::Numeric, "Position", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition DistanceFieldApproxAoPins[] = {
    MaterialInput("Position", MaterialPinType::Numeric, "Position", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Normal", MaterialPinType::Numeric, "Normal", {}, MaterialPinFlags::HideValue),
    MaterialInput("BaseDistance", MaterialPinType::Numeric, "BaseDistance",
                  MaterialPinDefault::Scalar(15.0f)),
    MaterialInput("Radius", MaterialPinType::Numeric, "Radius", MaterialPinDefault::Scalar(150.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition DistanceFieldsDefinitions[] = {
    MaterialNode("Material.DistanceToNearestSurface")
        .Properties(&GeneratedType<DistanceToNearestSurfaceProperties>::Schema)
        .Pins(DistanceToNearestSurfacePins)
        .Menu("DistanceToNearestSurface", "Distance Fields", "Utility", "DistanceToNearestSurface.")
        .Features(ToMask(MaterialContextFeature::DistanceField)),
    MaterialNode("Material.DistanceFieldGradient")
        .Properties(&GeneratedType<DistanceFieldGradientProperties>::Schema)
        .Pins(DistanceFieldGradientPins)
        .Menu("DistanceFieldGradient", "Distance Fields", "Utility", "DistanceFieldGradient.")
        .Features(ToMask(MaterialContextFeature::DistanceField)),
    MaterialNode("Material.DistanceFieldApproxAo")
        .Properties(&GeneratedType<DistanceFieldApproxAoProperties>::Schema)
        .Pins(DistanceFieldApproxAoPins)
        .Menu("Distance Field Approx Ao", "Distance Fields", "Utility DistanceFieldApproxAO",
              "Distance Field Approx Ao.")
        .Features(ToMask(MaterialContextFeature::DistanceField)),
};

} // namespace

Span<const MaterialNodeDefinition> DistanceFieldsNodeDefinitions() noexcept {
    return DistanceFieldsDefinitions;
}

} // namespace rocks::material::detail
