#pragma once

#include <MaterialSchema/Nodes/MaterialAttributes/MaterialAttributesNodeEnums.h>
#include <Meta/Annotations.h>

// Blend Attributes 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionBlendMaterialAttributes —
// RE/Public/Materials/MaterialExpressionBlendMaterialAttributes.h). 이름·기본값은 UE와 같다(b
// 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct BlendAttributesProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() MaterialAttributeBlend pixelAttributeBlendType = MaterialAttributeBlend::Blend;
    ROCKS_PROPERTY()
    MaterialAttributeBlend vertexAttributeBlendType = MaterialAttributeBlend::Blend;
};

} // namespace rocks::material
