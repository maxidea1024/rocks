#pragma once

#include <MaterialSchema/Nodes/Texture/TextureNodeEnums.h>
#include <Meta/Annotations.h>

// Texture Property 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionTextureProperty — RE/Public/Materials/MaterialExpressionTextureProperty.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct TexturePropertyProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() ExposedTextureProperty property = ExposedTextureProperty::TextureSize;
};

} // namespace rocks::material
