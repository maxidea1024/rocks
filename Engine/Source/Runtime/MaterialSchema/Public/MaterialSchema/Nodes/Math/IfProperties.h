#pragma once

#include <Meta/Annotations.h>

// If 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE UMaterialExpressionIf —
// RE/Public/Materials/MaterialExpressionIf.h). 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct IfProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() float equalsThreshold = 0.00001f;
};

} // namespace rocks::material
