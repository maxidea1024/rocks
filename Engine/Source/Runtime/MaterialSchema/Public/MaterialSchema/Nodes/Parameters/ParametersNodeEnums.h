#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// Parameters 범주 노드 속성의 열거다(Docs/Design/MaterialSystem 19.13.2절). 값 번호와 뜻은
// UE 5.8 열거와 같고 이름은 UE 접두를 뗀 나머지다. UE의 숨은 값과 _MAX 표지는 두지 않는다. 저장
// 형식이라 바꾸지 않는다.
namespace rocks::material {

// ChannelMaskParameterColor 값이다(UE EChannelMaskParameterColor —
// RE/Public/Materials/MaterialExpressionChannelMaskParameterColor.h).
ROCKS_ENUM()
enum class ChannelMaskParameterColor : uint8 {
    Red = 0,
    Green = 1,
    Blue = 2,
    Alpha = 3
};

// CollectionTransformType 값이다(UE EParameterCollectionTransformType —
// RE/Public/Materials/MaterialExpressionCollectionTransform.h).
ROCKS_ENUM()
enum class CollectionTransformType : uint8 {
    Position = 0,
    Vector = 1,
    Projection = 2,
    LocalToWorld = 3,
    WorldToLocal = 4
};

// ScalarParameterControlType 값이다(UE EMaterialScalarParameterControlType —
// RE/Public/Materials/MaterialExpressionScalarParameter.h).
ROCKS_ENUM()
enum class ScalarParameterControlType : uint8 {
    Numeric = 0,
    Enumeration = 1,
    EnumerationIndex = 2
};

} // namespace rocks::material
