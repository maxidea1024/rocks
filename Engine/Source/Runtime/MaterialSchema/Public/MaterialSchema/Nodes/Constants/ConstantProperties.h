#pragma once

#include <Meta/Annotations.h>

// Constant 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE UMaterialExpressionConstant
// — RE/Public/Materials/MaterialExpressionConstant.h). 이름·기본값은 UE와 같다(b 접두를 떼고
// camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct ConstantProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() float r {};
};

} // namespace rocks::material
