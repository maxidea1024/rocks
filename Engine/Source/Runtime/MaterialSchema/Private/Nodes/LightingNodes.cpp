#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/MaterialGraphSchema.h>

// Lighting 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition AtmosphericLightVectorPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition AtmosphericLightColorPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition MainDirectionalLightPins[] = {
    MaterialOutput("Illuminance", MaterialPinType::Float3, "Illuminance"),
    MaterialOutput("Direction", MaterialPinType::Float3, "Direction"),
};

constexpr MaterialNodeDefinition LightingDefinitions[] = {
    MaterialNode("Material.AtmosphericLightVector")
        .Pins(AtmosphericLightVectorPins)
        .Menu("Atmosphere Sun Light Vector", "Lighting", "Utility", "Atmosphere Sun Light Vector.")
        .Features(ToMask(MaterialContextFeature::Sky)),
    MaterialNode("Material.AtmosphericLightColor")
        .Pins(AtmosphericLightColorPins)
        .Menu("Atmosphere Sun Light Illuminance On Ground", "Lighting", "Utility",
              "Atmosphere Sun Light Illuminance On Ground.")
        .Features(ToMask(MaterialContextFeature::Sky)),
    MaterialNode("Material.MainDirectionalLight")
        .Pins(MainDirectionalLightPins)
        .Menu("Forward Selected Directional Light", "Lighting", "",
              "Forward Selected Directional Light.")
        .Features(ToMask(MaterialContextFeature::Light)),
};

} // namespace

Span<const MaterialNodeDefinition> LightingNodeDefinitions() noexcept {
    return LightingDefinitions;
}

} // namespace rocks::material::detail
