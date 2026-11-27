#pragma once

#include <Meta/Annotations.h>

// BumpOffset 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionBumpOffset — RE/Public/Materials/MaterialExpressionBumpOffset.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct BumpOffsetProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() float referencePlane = 0.5f;
    ROCKS_PROPERTY() uint32 constCoordinate = 0;
};

} // namespace rocks::material
