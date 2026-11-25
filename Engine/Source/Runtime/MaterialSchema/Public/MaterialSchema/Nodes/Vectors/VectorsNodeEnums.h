#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// Vectors 범주 노드 속성의 열거다(Docs/Design/MaterialSystem 19.13.2절). 값 번호와 뜻은
// UE 5.8 열거와 같고 이름은 UE 접두를 뗀 나머지다. UE의 숨은 값과 _MAX 표지는 두지 않는다. 저장
// 형식이라 바꾸지 않는다.
namespace rocks::material {

// BoundsType 값이다(UE EMaterialExpressionBoundsType —
// RE/Public/Materials/MaterialExpressionBounds.h).
ROCKS_ENUM()
enum class BoundsType : uint8 {
    InstanceLocal = 0,
    ObjectLocal = 1,
    PreSkinnedLocal = 2
};

// PositionOrigin 값이다(UE EPositionOrigin — RE/Public/Materials/MaterialExpression.h).
ROCKS_ENUM()
enum class PositionOrigin : uint8 {
    Absolute = 0,
    CameraRelative = 1
};

// PositionTransformSpace 값이다(UE EMaterialPositionTransformSource —
// RE/Public/Materials/MaterialExpressionTransformPosition.h).
ROCKS_ENUM()
enum class PositionTransformSpace : uint8 {
    Local = 0,
    World = 1,
    PeriodicWorld = 2,
    TranslatedWorld = 3,
    FirstPersonTranslatedWorld = 4,
    View = 5,
    Camera = 6,
    Instance = 8
};

// VectorTransformDestination 값이다(UE EMaterialVectorCoordTransform —
// RE/Public/Materials/MaterialExpressionTransform.h).
ROCKS_ENUM()
enum class VectorTransformDestination : uint8 {
    Tangent = 0,
    Local = 1,
    World = 2,
    View = 3,
    Camera = 4,
    Instance = 6
};

// VectorTransformSource 값이다(UE EMaterialVectorCoordTransformSource —
// RE/Public/Materials/MaterialExpressionTransform.h).
ROCKS_ENUM()
enum class VectorTransformSource : uint8 {
    Tangent = 0,
    Local = 1,
    World = 2,
    View = 3,
    Camera = 4,
    Instance = 6
};

} // namespace rocks::material
