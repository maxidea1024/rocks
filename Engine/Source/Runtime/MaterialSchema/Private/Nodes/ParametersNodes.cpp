#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/Nodes/Parameters/ChannelMaskParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/ChannelMaskParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/CurveAtlasRowParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/CurveAtlasRowParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/DoubleVectorParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/DoubleVectorParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/ParameterCollectionTransformProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/ParameterCollectionTransformProperties.h>
#include <MaterialSchema/Nodes/Parameters/ParameterCollectionValueProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/ParameterCollectionValueProperties.h>
#include <MaterialSchema/Nodes/Parameters/ScalarParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/ScalarParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/StaticBoolParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/StaticBoolParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/StaticComponentMaskParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/StaticComponentMaskParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/StaticSwitchParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/StaticSwitchParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/VectorParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/VectorParameterProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Parameters 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr StringView ScalarParameterNonSemantic[] = {
    "group",     "sortPriority", "defaultValue", "controlType",
    "sliderMin", "sliderMax",    "enumeration",  "enumerationIndex"};
constexpr MaterialPropertyTextDefault ScalarParameterTextDefaults[] = {{"parameterName", "Param"}};
constexpr MaterialPinDefinition ScalarParameterPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr StringView VectorParameterNonSemantic[] = {"group", "sortPriority", "defaultValue",
                                                     "channelNames"};
constexpr MaterialPropertyTextDefault VectorParameterTextDefaults[] = {{"parameterName", "Param"}};
constexpr MaterialPinDefinition VectorParameterPins[] = {
    MaterialOutput("RGB", MaterialPinType::Float3, "RGB"),
    MaterialOutput("R", MaterialPinType::Float1, "R"),
    MaterialOutput("G", MaterialPinType::Float1, "G"),
    MaterialOutput("B", MaterialPinType::Float1, "B"),
    MaterialOutput("A", MaterialPinType::Float1, "A"),
    MaterialOutput("RGBA", MaterialPinType::Float4, "RGBA"),
};

constexpr StringView DoubleVectorParameterNonSemantic[] = {"group", "sortPriority", "defaultValue"};
constexpr MaterialPropertyTextDefault DoubleVectorParameterTextDefaults[] = {
    {"parameterName", "Param"}};
constexpr MaterialPinDefinition DoubleVectorParameterPins[] = {
    MaterialOutput("XYZ", MaterialPinType::Float3, "XYZ"),
    MaterialOutput("X", MaterialPinType::Float1, "X"),
    MaterialOutput("Y", MaterialPinType::Float1, "Y"),
    MaterialOutput("Z", MaterialPinType::Float1, "Z"),
    MaterialOutput("W", MaterialPinType::Float1, "W"),
};

constexpr StringView ChannelMaskParameterNonSemantic[] = {"group", "sortPriority", "defaultValue",
                                                          "channelNames"};
constexpr MaterialPropertyTextDefault ChannelMaskParameterTextDefaults[] = {
    {"parameterName", "Param"}};
constexpr MaterialPinDefinition ChannelMaskParameterPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Float1, ""),
};

constexpr StringView StaticBoolParameterNonSemantic[] = {"group", "sortPriority"};
constexpr MaterialPropertyTextDefault StaticBoolParameterTextDefaults[] = {
    {"parameterName", "Param"}};
constexpr MaterialPinDefinition StaticBoolParameterPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr StringView StaticSwitchParameterNonSemantic[] = {"group", "sortPriority"};
constexpr MaterialPropertyTextDefault StaticSwitchParameterTextDefaults[] = {
    {"parameterName", "Param"}};
constexpr MaterialPinDefinition StaticSwitchParameterPins[] = {
    MaterialInput("A", MaterialPinType::Any, "True", {}, MaterialPinFlags::HideValue),
    MaterialInput("B", MaterialPinType::Any, "False", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Any, ""),
};

constexpr StringView StaticComponentMaskParameterNonSemantic[] = {"group", "sortPriority"};
constexpr MaterialPropertyTextDefault StaticComponentMaskParameterTextDefaults[] = {
    {"parameterName", "Param"}};
constexpr MaterialPinDefinition StaticComponentMaskParameterPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ParameterCollectionValuePins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ParameterCollectionTransformPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr StringView CurveAtlasRowParameterNonSemantic[] = {
    "group",     "sortPriority", "defaultValue", "controlType",
    "sliderMin", "sliderMax",    "enumeration",  "enumerationIndex"};
constexpr MaterialPropertyTextDefault CurveAtlasRowParameterTextDefaults[] = {
    {"parameterName", "Param"}};
constexpr MaterialPinDefinition CurveAtlasRowParameterPins[] = {
    MaterialInput("InputTime", MaterialPinType::Numeric, "InputTime", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Float3, ""),
    MaterialOutput("R", MaterialPinType::Float1, ""),
    MaterialOutput("G", MaterialPinType::Float1, ""),
    MaterialOutput("B", MaterialPinType::Float1, ""),
    MaterialOutput("A", MaterialPinType::Float1, ""),
};

constexpr MaterialNodeDefinition ParametersDefinitions[] = {
    MaterialNode("Material.ScalarParameter")
        .Properties(&GeneratedType<ScalarParameterProperties>::Schema, ScalarParameterNonSemantic)
        .TextDefaults(ScalarParameterTextDefaults)
        .Pins(ScalarParameterPins)
        .Menu("Scalar Parameter", "Parameters", "Parameters", "Scalar Parameter.")
        .Flags(MaterialNodeFlags::Parameter),
    MaterialNode("Material.VectorParameter")
        .Properties(&GeneratedType<VectorParameterProperties>::Schema, VectorParameterNonSemantic)
        .TextDefaults(VectorParameterTextDefaults)
        .Pins(VectorParameterPins)
        .Menu("Vector Parameter", "Parameters", "Parameters", "Vector Parameter.")
        .Flags(MaterialNodeFlags::Parameter),
    MaterialNode("Material.DoubleVectorParameter")
        .Properties(&GeneratedType<DoubleVectorParameterProperties>::Schema,
                    DoubleVectorParameterNonSemantic)
        .TextDefaults(DoubleVectorParameterTextDefaults)
        .Pins(DoubleVectorParameterPins)
        .Menu("Double Vector Parameter", "Parameters", "Parameters", "Double Vector Parameter.")
        .Flags(MaterialNodeFlags::Parameter),
    MaterialNode("Material.ChannelMaskParameter")
        .Properties(&GeneratedType<ChannelMaskParameterProperties>::Schema,
                    ChannelMaskParameterNonSemantic)
        .TextDefaults(ChannelMaskParameterTextDefaults)
        .Pins(ChannelMaskParameterPins)
        .Menu("Channel Mask Parameter", "Parameters", "Parameters Utility",
              "Channel Mask Parameter.")
        .Flags(MaterialNodeFlags::Parameter),
    MaterialNode("Material.StaticBoolParameter")
        .Properties(&GeneratedType<StaticBoolParameterProperties>::Schema,
                    StaticBoolParameterNonSemantic)
        .TextDefaults(StaticBoolParameterTextDefaults)
        .Pins(StaticBoolParameterPins)
        .Menu("Static Bool Parameter", "Parameters", "Parameters", "Static Bool Parameter.")
        .Flags(MaterialNodeFlags::Parameter),
    MaterialNode("Material.StaticSwitchParameter")
        .Properties(&GeneratedType<StaticSwitchParameterProperties>::Schema,
                    StaticSwitchParameterNonSemantic)
        .TextDefaults(StaticSwitchParameterTextDefaults)
        .Pins(StaticSwitchParameterPins)
        .Menu("Static Switch Parameter", "Parameters", "Parameters", "Static Switch Parameter.")
        .Flags(MaterialNodeFlags::Parameter),
    MaterialNode("Material.StaticComponentMaskParameter")
        .Properties(&GeneratedType<StaticComponentMaskParameterProperties>::Schema,
                    StaticComponentMaskParameterNonSemantic)
        .TextDefaults(StaticComponentMaskParameterTextDefaults)
        .Pins(StaticComponentMaskParameterPins)
        .Menu("Static Component Mask Parameter", "Parameters", "Parameters",
              "Static Component Mask Parameter.")
        .Flags(MaterialNodeFlags::Parameter),
    MaterialNode("Material.ParameterCollectionValue")
        .Properties(&GeneratedType<ParameterCollectionValueProperties>::Schema)
        .Pins(ParameterCollectionValuePins)
        .Menu("Parameter Collection Value", "Parameters", "Parameters CollectionParameter",
              "Parameter Collection Value."),
    MaterialNode("Material.ParameterCollectionTransform")
        .Properties(&GeneratedType<ParameterCollectionTransformProperties>::Schema)
        .Pins(ParameterCollectionTransformPins)
        .Menu("Parameter Collection Transform", "Parameters", "Parameters CollectionTransform",
              "Parameter Collection Transform."),
    MaterialNode("Material.CurveAtlasRowParameter")
        .Properties(&GeneratedType<CurveAtlasRowParameterProperties>::Schema,
                    CurveAtlasRowParameterNonSemantic)
        .TextDefaults(CurveAtlasRowParameterTextDefaults)
        .Pins(CurveAtlasRowParameterPins)
        .Menu("Curve Atlas Row Parameter", "Parameters", "Parameters", "Curve Atlas Row Parameter.")
        .Flags(MaterialNodeFlags::Parameter)
        .Missing(MaterialPrerequisite::CurveAsset),
};

} // namespace

Span<const MaterialNodeDefinition> ParametersNodeDefinitions() noexcept {
    return ParametersDefinitions;
}

} // namespace rocks::material::detail
