#pragma once

#include <Meta/Annotations.h>

// Decal Mipmap Level 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionDecalMipmapLevel — RE/Public/Materials/MaterialExpressionDecalMipmapLevel.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct DecalMipmapLevelProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() float constWidth {};
    ROCKS_PROPERTY() float constHeight {};
};

} // namespace rocks::material
