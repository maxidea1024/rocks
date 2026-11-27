#include "MaterialBuiltinNodes.h"

#include "MaterialDynamicPins.h"
#include <MaterialSchema/Nodes/Functions/FunctionCallProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallProperties.h>
#include <MaterialSchema/Nodes/Functions/FunctionInputProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionInputProperties.h>
#include <MaterialSchema/Nodes/Functions/FunctionOutputProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionOutputProperties.h>
#include <MaterialSchema/Nodes/Functions/LayerOutputProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/LayerOutputProperties.h>
#include <MaterialSchema/Nodes/Functions/MaterialSampleProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/MaterialSampleProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Functions 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPropertyTextDefault FunctionInputTextDefaults[] = {{"inputName", "In"}};
constexpr MaterialPropertyTextDefault FunctionOutputTextDefaults[] = {{"outputName", "Result"}};
constexpr MaterialPinDefinition FunctionOutputPins[] = {
    MaterialInput("A", MaterialPinType::Any, "A", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
};

constexpr MaterialPropertyTextDefault LayerOutputTextDefaults[] = {
    {"outputName", "Material Attributes"}};
constexpr MaterialPinDefinition LayerOutputPins[] = {
    MaterialInput("A", MaterialPinType::MaterialAttributes, "A", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
};

constexpr MaterialPinDefinition MaterialSamplePins[] = {
    MaterialOutput("Front Material", MaterialPinType::Surface, "Front Material"),
};

constexpr MaterialNodeDefinition FunctionsDefinitions[] = {
    MaterialNode("Material.FunctionCall")
        .Properties(&GeneratedType<FunctionCallProperties>::Schema)
        .DynamicPins(&FunctionCallDynamicPins)
        .Menu("Function Call", "Functions", "Functions MaterialFunctionCall", "Function Call."),
    MaterialNode("Material.FunctionInput")
        .Properties(&GeneratedType<FunctionInputProperties>::Schema)
        .TextDefaults(FunctionInputTextDefaults)
        .DynamicPins(&FunctionInputDynamicPins)
        .Menu("Function Input", "Functions", "Functions", "Function Input.")
        .GraphKinds(ToMask(MaterialGraphKind::Function)),
    MaterialNode("Material.FunctionOutput")
        .Properties(&GeneratedType<FunctionOutputProperties>::Schema)
        .TextDefaults(FunctionOutputTextDefaults)
        .Pins(FunctionOutputPins)
        .Menu("Function Output", "Functions", "Functions", "Function Output.")
        .GraphKinds(ToMask(MaterialGraphKind::Function)),
    MaterialNode("Material.LayerOutput")
        .Properties(&GeneratedType<LayerOutputProperties>::Schema)
        .TextDefaults(LayerOutputTextDefaults)
        .Pins(LayerOutputPins)
        .Menu("Layer Output", "Functions", "MaterialLayerOutput", "Layer Output.")
        .GraphKinds(ToMask(MaterialGraphKind::Function)),
    MaterialNode("Material.MaterialSample")
        .Properties(&GeneratedType<MaterialSampleProperties>::Schema)
        .Pins(MaterialSamplePins)
        .Menu("Material Sample", "Functions", "", "Material Sample."),
};

} // namespace

Span<const MaterialNodeDefinition> FunctionsNodeDefinitions() noexcept {
    return FunctionsDefinitions;
}

} // namespace rocks::material::detail
