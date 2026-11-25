#pragma once

#include <Meta/Annotations.h>

// Panner 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE UMaterialExpressionPanner —
// RE/Public/Materials/MaterialExpressionPanner.h). 이름·기본값은 UE와 같다(b 접두를 떼고
// camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct PannerProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() float speedX {};
    ROCKS_PROPERTY() uint32 constCoordinate = 0;
    ROCKS_PROPERTY() bool fractionalPart {};
};

} // namespace rocks::material
