#include "MaterialBuiltinNodes.h"

#include "MaterialDynamicPins.h"
#include <MaterialSchema/Nodes/Custom/CustomHlslProperties.generated.h>
#include <MaterialSchema/Nodes/Custom/CustomHlslProperties.h>
#include <MaterialSchema/Nodes/Custom/PerInstanceCustomData3Properties.generated.h>
#include <MaterialSchema/Nodes/Custom/PerInstanceCustomData3Properties.h>
#include <MaterialSchema/Nodes/Custom/PerInstanceCustomDataProperties.generated.h>
#include <MaterialSchema/Nodes/Custom/PerInstanceCustomDataProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Custom 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPropertyTextDefault CustomHlslTextDefaults[] = {
    {"code",
     "// The below expression will get compiled\n// into the output of this node\nfloat3(1, 1, 1)"},
    {"description", "Custom"}};
constexpr MaterialPinDefinition PerInstanceCustomDataPins[] = {
    MaterialInput("DefaultValue", MaterialPinType::Numeric, "DefaultValue",
                  MaterialPinDefault::Scalar(0.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition PerInstanceCustomData3Pins[] = {
    MaterialInput("DefaultValue", MaterialPinType::Numeric, "DefaultValue",
                  MaterialPinDefault::Vector4(0.0f, 0.0f, 0.0f, 0.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition CustomDefinitions[] = {
    MaterialNode("Material.CustomHlsl")
        .Properties(&GeneratedType<CustomHlslProperties>::Schema)
        .TextDefaults(CustomHlslTextDefaults)
        .DynamicPins(&CustomDynamicPins)
        .Menu("Custom Hlsl", "Custom", "Custom", "Custom Hlsl."),
    MaterialNode("Material.PerInstanceCustomData")
        .Properties(&GeneratedType<PerInstanceCustomDataProperties>::Schema)
        .Pins(PerInstanceCustomDataPins)
        .Menu("Per Instance Custom Data", "Custom", "Custom", "Per Instance Custom Data.")
        .Features(ToMask(MaterialContextFeature::Primitive)),
    MaterialNode("Material.PerInstanceCustomData3")
        .Properties(&GeneratedType<PerInstanceCustomData3Properties>::Schema)
        .Pins(PerInstanceCustomData3Pins)
        .Menu("Per Instance Custom Data3", "Custom", "Custom PerInstanceCustomData3Vector",
              "Per Instance Custom Data3.")
        .Features(ToMask(MaterialContextFeature::Primitive)),
};

} // namespace

Span<const MaterialNodeDefinition> CustomNodeDefinitions() noexcept {
    return CustomDefinitions;
}

} // namespace rocks::material::detail
