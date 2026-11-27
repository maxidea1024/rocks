#pragma once

#include <MaterialSchema/Nodes/Math/MathNodeEnums.h>
#include <Meta/Annotations.h>

// FloatToUInt 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionFloatToUInt — RE/Public/Materials/MaterialExpressionFloatToUInt.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct FloatToUIntProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() FloatToIntMode mode = FloatToIntMode::Truncate;
};

} // namespace rocks::material
