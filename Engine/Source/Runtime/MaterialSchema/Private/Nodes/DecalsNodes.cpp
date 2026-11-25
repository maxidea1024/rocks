#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/Nodes/Decals/DecalBufferTextureProperties.generated.h>
#include <MaterialSchema/Nodes/Decals/DecalBufferTextureProperties.h>
#include <MaterialSchema/Nodes/Decals/DecalMipmapLevelProperties.generated.h>
#include <MaterialSchema/Nodes/Decals/DecalMipmapLevelProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Decals 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition DecalBufferTexturePins[] = {
    MaterialInput("Coordinates", MaterialPinType::Numeric, "Coordinates", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("RGBA", MaterialPinType::Float4, "RGBA"),
    MaterialOutput("RGB", MaterialPinType::Float3, "RGB"),
    MaterialOutput("A", MaterialPinType::Float1, "A"),
};

constexpr MaterialPinDefinition DecalColorPins[] = {
    MaterialOutput("RGB", MaterialPinType::Float3, "RGB"),
    MaterialOutput("R", MaterialPinType::Float1, "R"),
    MaterialOutput("G", MaterialPinType::Float1, "G"),
    MaterialOutput("B", MaterialPinType::Float1, "B"),
    MaterialOutput("A", MaterialPinType::Float1, "A"),
    MaterialOutput("RGBA", MaterialPinType::Float4, "RGBA"),
};

constexpr MaterialPinDefinition DecalDerivativePins[] = {
    MaterialOutput("DDX", MaterialPinType::Numeric, "DDX"),
    MaterialOutput("DDY", MaterialPinType::Numeric, "DDY"),
};

constexpr MaterialPinDefinition DecalLifetimeOpacityPins[] = {
    MaterialOutput("Opacity", MaterialPinType::Numeric, "Opacity"),
};

constexpr MaterialPinDefinition DecalMipmapLevelPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition DecalsDefinitions[] = {
    MaterialNode("Material.DecalBufferTexture")
        .Properties(&GeneratedType<DecalBufferTextureProperties>::Schema)
        .Pins(DecalBufferTexturePins)
        .Menu("Decal Buffer Texture", "Decals", "Decals DBufferTexture", "Decal Buffer Texture.")
        .Features(ToMask(MaterialContextFeature::Decal)),
    MaterialNode("Material.DecalColor")
        .Pins(DecalColorPins)
        .Menu("Decal Color", "Decals", "Decals", "Decal Color.")
        .Features(ToMask(MaterialContextFeature::Decal)),
    MaterialNode("Material.DecalDerivative")
        .Pins(DecalDerivativePins)
        .Menu("Decal Derivative", "Decals", "Decals", "Decal Derivative.")
        .Features(ToMask(MaterialContextFeature::Decal)),
    MaterialNode("Material.DecalLifetimeOpacity")
        .Pins(DecalLifetimeOpacityPins)
        .Menu("Decal Lifetime Opacity", "Decals", "Decals", "Decal Lifetime Opacity.")
        .Features(ToMask(MaterialContextFeature::Decal)),
    MaterialNode("Material.DecalMipmapLevel")
        .Properties(&GeneratedType<DecalMipmapLevelProperties>::Schema)
        .Pins(DecalMipmapLevelPins)
        .Menu("Decal Mipmap Level", "Decals", "Decals", "Decal Mipmap Level.")
        .Features(ToMask(MaterialContextFeature::Decal)),
};

} // namespace

Span<const MaterialNodeDefinition> DecalsNodeDefinitions() noexcept {
    return DecalsDefinitions;
}

} // namespace rocks::material::detail
