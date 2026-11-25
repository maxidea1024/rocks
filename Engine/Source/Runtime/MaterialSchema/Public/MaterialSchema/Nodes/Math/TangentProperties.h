#pragma once

#include <Meta/Annotations.h>

// Tangent 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE UMaterialExpressionTangent —
// RE/Public/Materials/MaterialExpressionTangent.h). 이름·기본값은 UE와 같다(b 접두를 떼고
// camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct TangentProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() float period = 1.0f;
};

} // namespace rocks::material
