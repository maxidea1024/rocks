#pragma once

#include <MaterialSchema/Nodes/MaterialAttributes/MaterialAttributesNodeEnums.h>
#include <Meta/Annotations.h>

// Blend 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE UMaterialExpressionBlend —
// RE/Public/Materials/MaterialExpressionBlend.h). 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct BlendProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() LayerBlendMode pixelAttributesBlendMode = LayerBlendMode::Blend;
    ROCKS_PROPERTY() LayerBlendMode vertexAttributesBlendMode = LayerBlendMode::Blend;
};

} // namespace rocks::material
