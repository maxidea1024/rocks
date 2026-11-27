#pragma once

#include <Meta/Annotations.h>

// ConstantBiasScale 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionConstantBiasScale —
// RE/Public/Materials/MaterialExpressionConstantBiasScale.h). 이름·기본값은 UE와 같다(b 접두를 떼고
// camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct ConstantBiasScaleProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() float bias = 1.0f;
    ROCKS_PROPERTY() float scale = 0.5f;
};

} // namespace rocks::material
