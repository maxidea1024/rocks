#pragma once

#include <Meta/Annotations.h>

// Reflection Vector 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionReflectionVectorWS —
// RE/Public/Materials/MaterialExpressionReflectionVectorWS.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct ReflectionVectorProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() bool normalizeCustomWorldNormal {};
};

} // namespace rocks::material
