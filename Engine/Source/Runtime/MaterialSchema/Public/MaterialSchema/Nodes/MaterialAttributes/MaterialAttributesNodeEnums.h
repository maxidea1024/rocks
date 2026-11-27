#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// Material Attributes 범주 노드 속성의 열거다(Docs/Design/MaterialSystem 19.13.2절). 값 번호와 뜻은
// UE 5.8 열거와 같고 이름은 UE 접두를 뗀 나머지다. UE의 숨은 값과 _MAX 표지는 두지 않는다. 저장
// 형식이라 바꾸지 않는다.
namespace rocks::material {

// AggregateKind 값이다(UE EMaterialExpressionMakeAggregateKind —
// RE/Public/Materials/MaterialExpressionAggregate.h).
ROCKS_ENUM()
enum class AggregateKind : uint8 {
    MaterialAttributes = 0,
    UserDefined = 1
};

// LayerBlendMode 값이다(UE EMaterialExpressionBlendMode —
// RE/Public/Materials/MaterialExpressionBlend.h).
ROCKS_ENUM()
enum class LayerBlendMode : uint8 {
    Blend = 0,
    UseA = 1,
    UseB = 2
};

// MaterialAttributeBlend 값이다(UE EMaterialAttributeBlend —
// RE/Public/Materials/MaterialExpressionBlendMaterialAttributes.h).
ROCKS_ENUM()
enum class MaterialAttributeBlend : uint8 {
    Blend = 0,
    UseA = 1,
    UseB = 2
};

} // namespace rocks::material
