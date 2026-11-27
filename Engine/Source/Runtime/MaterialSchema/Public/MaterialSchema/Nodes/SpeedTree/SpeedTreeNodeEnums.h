#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// SpeedTree 범주 노드 속성의 열거다(Docs/Design/MaterialSystem 19.13.2절). 값 번호와 뜻은
// UE 5.8 열거와 같고 이름은 UE 접두를 뗀 나머지다. UE의 숨은 값과 _MAX 표지는 두지 않는다. 저장
// 형식이라 바꾸지 않는다.
namespace rocks::material {

// SpeedTreeGeometryType 값이다(UE ESpeedTreeGeometryType —
// RE/Public/Materials/MaterialExpressionSpeedTree.h).
ROCKS_ENUM()
enum class SpeedTreeGeometryType : uint8 {
    Branch = 0,
    Frond = 1,
    Leaf = 2,
    FacingLeaf = 3,
    Billboard = 4
};

// SpeedTreeLodType 값이다(UE ESpeedTreeLODType —
// RE/Public/Materials/MaterialExpressionSpeedTree.h).
ROCKS_ENUM()
enum class SpeedTreeLodType : uint8 {
    Pop = 0,
    Smooth = 1
};

// SpeedTreeWindType 값이다(UE ESpeedTreeWindType —
// RE/Public/Materials/MaterialExpressionSpeedTree.h).
ROCKS_ENUM()
enum class SpeedTreeWindType : uint8 {
    None = 0,
    Fastest = 1,
    Fast = 2,
    Better = 3,
    Best = 4,
    Palm = 5,
    BestPlus = 6
};

} // namespace rocks::material
