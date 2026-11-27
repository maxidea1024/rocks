#pragma once

#include <Meta/Annotations.h>

// Texture Coordinate 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionTextureCoordinate —
// RE/Public/Materials/MaterialExpressionTextureCoordinate.h). 이름·기본값은 UE와 같다(b 접두를 떼고
// camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct TextureCoordinateProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 coordinateIndex {};
    ROCKS_PROPERTY() float uTiling = 1.0f;
    ROCKS_PROPERTY() float vTiling = 1.0f;
    ROCKS_PROPERTY() bool unMirrorU {};
    ROCKS_PROPERTY() bool unMirrorV {};
};

} // namespace rocks::material
