#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/Nodes/Font/FontSampleParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Font/FontSampleParameterProperties.h>
#include <MaterialSchema/Nodes/Font/FontSampleProperties.generated.h>
#include <MaterialSchema/Nodes/Font/FontSampleProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Font 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition FontSignedDistancePins[] = {
    MaterialOutput("Signed Distance", MaterialPinType::Float1, "Signed Distance"),
    MaterialOutput("Smooth Signed Distance", MaterialPinType::Float1, "Smooth Signed Distance"),
    MaterialOutput("Pixel Distance Factor", MaterialPinType::Float1, "Pixel Distance Factor"),
    MaterialOutput("Implicit Opacity", MaterialPinType::Float1, "Implicit Opacity"),
};

constexpr MaterialPinDefinition FontSamplePins[] = {
    MaterialOutput("Result", MaterialPinType::Float3, ""),
    MaterialOutput("R", MaterialPinType::Float1, ""),
    MaterialOutput("G", MaterialPinType::Float1, ""),
    MaterialOutput("B", MaterialPinType::Float1, ""),
    MaterialOutput("A", MaterialPinType::Float1, ""),
};

constexpr StringView FontSampleParameterNonSemantic[] = {"font", "fontTexturePage", "group",
                                                         "sortPriority"};
constexpr MaterialPinDefinition FontSampleParameterPins[] = {
    MaterialOutput("Result", MaterialPinType::Float3, ""),
    MaterialOutput("R", MaterialPinType::Float1, ""),
    MaterialOutput("G", MaterialPinType::Float1, ""),
    MaterialOutput("B", MaterialPinType::Float1, ""),
    MaterialOutput("A", MaterialPinType::Float1, ""),
};

constexpr MaterialNodeDefinition FontDefinitions[] = {
    MaterialNode("Material.FontSignedDistance")
        .Pins(FontSignedDistancePins)
        .Menu("Font Signed Distance", "Font", "Constants", "Font Signed Distance.")
        .Features(ToMask(MaterialContextFeature::UserInterface)),
    MaterialNode("Material.FontSample")
        .Properties(&GeneratedType<FontSampleProperties>::Schema)
        .Pins(FontSamplePins)
        .Menu("Font Sample", "Font", "Font Texture", "Font Sample.")
        .Missing(MaterialPrerequisite::Font),
    MaterialNode("Material.FontSampleParameter")
        .Properties(&GeneratedType<FontSampleParameterProperties>::Schema,
                    FontSampleParameterNonSemantic)
        .Pins(FontSampleParameterPins)
        .Menu("Font Param", "Font", "Font Texture Parameters", "Font Param.")
        .Flags(MaterialNodeFlags::Parameter)
        .Missing(MaterialPrerequisite::Font),
};

} // namespace

Span<const MaterialNodeDefinition> FontNodeDefinitions() noexcept {
    return FontDefinitions;
}

} // namespace rocks::material::detail
