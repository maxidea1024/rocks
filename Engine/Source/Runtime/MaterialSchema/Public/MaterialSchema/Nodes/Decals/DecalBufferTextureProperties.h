#pragma once

#include <MaterialSchema/Nodes/Decals/DecalsNodeEnums.h>
#include <Meta/Annotations.h>

// Decal Buffer Texture 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionDBufferTexture — RE/Public/Materials/MaterialExpressionDBufferTexture.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct DecalBufferTextureProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() DecalBufferTextureId dBufferTextureId = DecalBufferTextureId::A;
};

} // namespace rocks::material
