#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackLayer.h>
#include <Meta/Annotations.h>

// 노드 속성 안의 보조 값이다(Docs/Design/MaterialSystem 19.13.2절, UE FMaterialLayersFunctions).
// UE의 입력 칸 (FExpressionInput)은 그래프 간선이 대신하므로 두지 않는다.
namespace rocks::material {

ROCKS_STRUCT()
struct LayerStackDefaults {
    ROCKS_GENERATED_BODY()

    explicit LayerStackDefaults(Allocator& allocator) noexcept : layers(allocator) {
    }

    // 아래(0)부터 위 순서다.
    ROCKS_PROPERTY() Array<LayerStackLayer> layers;
};

} // namespace rocks::material
