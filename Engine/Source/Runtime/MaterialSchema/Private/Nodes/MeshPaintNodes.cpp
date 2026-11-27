#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/MaterialGraphSchema.h>

// Mesh Paint 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition MeshPaintTextureObjectPins[] = {
    MaterialOutput("Result", MaterialPinType::Texture2D, ""),
};

constexpr MaterialPinDefinition MeshPaintTextureCoordinateIndexPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition MeshPaintDefinitions[] = {
    MaterialNode("Material.MeshPaintTextureObject")
        .Pins(MeshPaintTextureObjectPins)
        .Menu("Mesh Paint Texture Object", "Mesh Paint", "MeshPaintTexture",
              "Mesh Paint Texture Object.")
        .Missing(MaterialPrerequisite::VirtualTexture),
    MaterialNode("Material.MeshPaintTextureCoordinateIndex")
        .Pins(MeshPaintTextureCoordinateIndexPins)
        .Menu("Mesh Paint Texture Coordinate Index", "Mesh Paint", "MeshPaintTexture",
              "Mesh Paint Texture Coordinate Index.")
        .Features(ToMask(MaterialContextFeature::MeshPaint)),
};

} // namespace

Span<const MaterialNodeDefinition> MeshPaintNodeDefinitions() noexcept {
    return MeshPaintDefinitions;
}

} // namespace rocks::material::detail
