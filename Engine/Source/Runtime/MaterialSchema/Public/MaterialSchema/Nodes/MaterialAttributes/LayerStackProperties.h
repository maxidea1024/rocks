#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <MaterialSchema/MaterialAssetReference.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackDefaults.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackInput.h>
#include <Meta/Annotations.h>

// Layer Stack 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionLayerStack — RE/Public/Materials/MaterialExpressionLayerStack.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct LayerStackProperties {
    ROCKS_GENERATED_BODY()

    explicit LayerStackProperties(Allocator& allocator) noexcept
        : defaultLayers(allocator), availableLayers(allocator), availableBlends(allocator),
          layerInputs(allocator) {
    }

    ROCKS_PROPERTY() LayerStackDefaults defaultLayers;
    // 기대 클래스는 MaterialFunctionBase다(19.4.3절).
    ROCKS_PROPERTY() Array<MaterialAssetReference> availableLayers;
    // 기대 클래스는 MaterialFunctionBase다(19.4.3절).
    ROCKS_PROPERTY() Array<MaterialAssetReference> availableBlends;
    // 레이어 함수 입력의 사본이다(UE LayerInputs). 레이어가 바뀌면 에디터가 다시 채운다.
    ROCKS_PROPERTY() Array<LayerStackInput> layerInputs;
};

} // namespace rocks::material
