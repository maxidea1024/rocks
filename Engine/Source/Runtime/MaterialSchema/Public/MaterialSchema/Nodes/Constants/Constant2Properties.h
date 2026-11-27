#pragma once

#include <Meta/Annotations.h>

// Constant2 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionConstant2Vector — RE/Public/Materials/MaterialExpressionConstant2Vector.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct Constant2Properties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() float r {};
    ROCKS_PROPERTY() float g {};
};

} // namespace rocks::material
