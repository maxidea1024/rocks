#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/Nodes/Constants/Constant2Properties.generated.h>
#include <MaterialSchema/Nodes/Constants/Constant2Properties.h>
#include <MaterialSchema/Nodes/Constants/Constant3Properties.generated.h>
#include <MaterialSchema/Nodes/Constants/Constant3Properties.h>
#include <MaterialSchema/Nodes/Constants/Constant4Properties.generated.h>
#include <MaterialSchema/Nodes/Constants/Constant4Properties.h>
#include <MaterialSchema/Nodes/Constants/ConstantDoubleProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/ConstantDoubleProperties.h>
#include <MaterialSchema/Nodes/Constants/ConstantProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/ConstantProperties.h>
#include <MaterialSchema/Nodes/Constants/StaticBoolProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/StaticBoolProperties.h>
#include <MaterialSchema/Nodes/Constants/TimeProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/TimeProperties.h>
#include <MaterialSchema/Nodes/Constants/ViewPropertyProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/ViewPropertyProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Constants 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition ConstantPins[] = {
    MaterialOutput("Result", MaterialPinType::Float1, ""),
};

constexpr MaterialPinDefinition Constant2Pins[] = {
    MaterialOutput("Result", MaterialPinType::Float2, ""),
    MaterialOutput("R", MaterialPinType::Float1, ""),
    MaterialOutput("G", MaterialPinType::Float1, ""),
};

constexpr MaterialPinDefinition Constant3Pins[] = {
    MaterialOutput("Result", MaterialPinType::Float3, ""),
    MaterialOutput("R", MaterialPinType::Float1, ""),
    MaterialOutput("G", MaterialPinType::Float1, ""),
    MaterialOutput("B", MaterialPinType::Float1, ""),
};

constexpr MaterialPinDefinition Constant4Pins[] = {
    MaterialOutput("RGBA", MaterialPinType::Float4, "RGBA"),
    MaterialOutput("R", MaterialPinType::Float1, "R"),
    MaterialOutput("G", MaterialPinType::Float1, "G"),
    MaterialOutput("B", MaterialPinType::Float1, "B"),
    MaterialOutput("A", MaterialPinType::Float1, "A"),
    MaterialOutput("RGB", MaterialPinType::Float3, "RGB"),
};

constexpr MaterialPinDefinition ConstantDoublePins[] = {
    MaterialOutput("Result", MaterialPinType::Double1, ""),
};

constexpr MaterialPinDefinition StaticBoolPins[] = {
    MaterialOutput("Result", MaterialPinType::StaticBool, ""),
};

constexpr MaterialPinDefinition TimePins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition DeltaTimePins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ViewPropertyPins[] = {
    MaterialOutput("Property", MaterialPinType::Numeric, "Property"),
    MaterialOutput("InvProperty", MaterialPinType::Numeric, "InvProperty"),
};

constexpr MaterialPinDefinition IsOrthographicPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition TwoSidedSignPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition VertexColorPins[] = {
    MaterialOutput("Result", MaterialPinType::Float3, ""),
    MaterialOutput("R", MaterialPinType::Float1, ""),
    MaterialOutput("G", MaterialPinType::Float1, ""),
    MaterialOutput("B", MaterialPinType::Float1, ""),
    MaterialOutput("A", MaterialPinType::Float1, ""),
};

constexpr MaterialPinDefinition PerInstanceRandomPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition PerInstanceFadeAmountPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition DistanceCullFadePins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition PrecomputedAoMaskPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition IsFirstPersonPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition ConstantsDefinitions[] = {
    MaterialNode("Material.Constant")
        .Properties(&GeneratedType<ConstantProperties>::Schema)
        .Pins(ConstantPins)
        .Menu("Constant", "Constants", "Constants", "Constant."),
    MaterialNode("Material.Constant2")
        .Properties(&GeneratedType<Constant2Properties>::Schema)
        .Pins(Constant2Pins)
        .Menu("Constant2", "Constants", "Constants Vectors Constant2Vector", "Constant2."),
    MaterialNode("Material.Constant3")
        .Properties(&GeneratedType<Constant3Properties>::Schema)
        .Pins(Constant3Pins)
        .Menu("Constant3", "Constants", "Constants Vectors Constant3Vector", "Constant3."),
    MaterialNode("Material.Constant4")
        .Properties(&GeneratedType<Constant4Properties>::Schema)
        .Pins(Constant4Pins)
        .Menu("Constant4", "Constants", "Constants Vectors Constant4Vector", "Constant4."),
    MaterialNode("Material.ConstantDouble")
        .Properties(&GeneratedType<ConstantDoubleProperties>::Schema)
        .Pins(ConstantDoublePins)
        .Menu("Constant Double", "Constants", "Constants", "Constant Double."),
    MaterialNode("Material.StaticBool")
        .Properties(&GeneratedType<StaticBoolProperties>::Schema)
        .Pins(StaticBoolPins)
        .Menu("Static Bool", "Constants", "Functions", "Static Bool."),
    MaterialNode("Material.Time")
        .Properties(&GeneratedType<TimeProperties>::Schema)
        .Pins(TimePins)
        .Menu("Time (Stopped)", "Constants", "Constants", "Time (Stopped).")
        .Features(ToMask(MaterialContextFeature::View)),
    MaterialNode("Material.DeltaTime")
        .Pins(DeltaTimePins)
        .Menu("DeltaTime", "Constants", "Constants", "DeltaTime.")
        .Features(ToMask(MaterialContextFeature::View)),
    MaterialNode("Material.ViewProperty")
        .Properties(&GeneratedType<ViewPropertyProperties>::Schema)
        .Pins(ViewPropertyPins)
        .Menu("View Property", "Constants", "Constants", "View Property.")
        .Features(ToMask(MaterialContextFeature::View)),
    MaterialNode("Material.IsOrthographic")
        .Pins(IsOrthographicPins)
        .Menu("IsOrthographic", "Constants", "Constants", "IsOrthographic.")
        .Features(ToMask(MaterialContextFeature::View)),
    MaterialNode("Material.TwoSidedSign")
        .Pins(TwoSidedSignPins)
        .Menu("TwoSidedSign", "Constants", "Constants", "TwoSidedSign.")
        .Features(ToMask(MaterialContextFeature::Vertex)),
    MaterialNode("Material.VertexColor")
        .Pins(VertexColorPins)
        .Menu("Vertex Color", "Constants", "Constants", "Vertex Color.")
        .Features(ToMask(MaterialContextFeature::Vertex)),
    MaterialNode("Material.PerInstanceRandom")
        .Pins(PerInstanceRandomPins)
        .Menu("PerInstanceRandom", "Constants", "Constants", "PerInstanceRandom.")
        .Features(ToMask(MaterialContextFeature::Primitive)),
    MaterialNode("Material.PerInstanceFadeAmount")
        .Pins(PerInstanceFadeAmountPins)
        .Menu("PerInstanceFadeAmount", "Constants", "Constants", "PerInstanceFadeAmount.")
        .Features(ToMask(MaterialContextFeature::Primitive)),
    MaterialNode("Material.DistanceCullFade")
        .Pins(DistanceCullFadePins)
        .Menu("Distance Cull Fade", "Constants", "Constants", "Distance Cull Fade.")
        .Features(ToMask(MaterialContextFeature::Primitive)),
    MaterialNode("Material.PrecomputedAoMask")
        .Pins(PrecomputedAoMaskPins)
        .Menu("PrecomputedAOMask", "Constants", "Constants PrecomputedAOMask",
              "A material expression that allows access to the precomputed AO mask generated when "
              "bGenerateAmbientOcclusionMaterialMask is enabled in LightBake World settings.")
        .Features(ToMask(MaterialContextFeature::Lightmap)),
    MaterialNode("Material.IsFirstPerson")
        .Pins(IsFirstPersonPins)
        .Menu("IsFirstPerson", "Constants", "Constants First Person", "IsFirstPerson.")
        .Features(ToMask(MaterialContextFeature::FirstPerson)),
};

} // namespace

Span<const MaterialNodeDefinition> ConstantsNodeDefinitions() noexcept {
    return ConstantsDefinitions;
}

} // namespace rocks::material::detail
