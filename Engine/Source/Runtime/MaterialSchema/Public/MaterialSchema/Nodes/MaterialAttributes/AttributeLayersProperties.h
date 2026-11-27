#pragma once

#include <Core/Memory/Allocator.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackDefaults.h>
#include <Meta/Annotations.h>

// Attribute Layers 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionMaterialAttributeLayers —
// RE/Public/Materials/MaterialExpressionMaterialAttributeLayers.h). 이름·기본값은 UE와 같다(b
// 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct AttributeLayersProperties {
    ROCKS_GENERATED_BODY()

    explicit AttributeLayersProperties(Allocator& allocator) noexcept : defaultLayers(allocator) {
    }

    ROCKS_PROPERTY() LayerStackDefaults defaultLayers;
};

} // namespace rocks::material
