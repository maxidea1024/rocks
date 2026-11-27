#pragma once

#include <MaterialSchema/Nodes/Vectors/VectorsNodeEnums.h>
#include <Meta/Annotations.h>

// Transform Position 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionTransformPosition —
// RE/Public/Materials/MaterialExpressionTransformPosition.h). 이름·기본값은 UE와 같다(b 접두를 떼고
// camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct TransformPositionProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() PositionTransformSpace transformSourceType = PositionTransformSpace::Local;
    ROCKS_PROPERTY() PositionTransformSpace transformType = PositionTransformSpace::Local;
    // 동적 입력 PeriodicWorldTileSize가 연결되지 않았을 때의 값이다.
    ROCKS_PROPERTY() float constPeriodicWorldTileSize = 32.0f;
    // 동적 입력 FirstPersonInterpolationAlpha가 연결되지 않았을 때의 값이다.
    ROCKS_PROPERTY() float constFirstPersonInterpolationAlpha = 1.0f;
};

} // namespace rocks::material
