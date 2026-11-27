#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// Post Process 범주 노드 속성의 열거다(Docs/Design/MaterialSystem 19.13.2절). 값 번호와 뜻은
// UE 5.8 열거와 같고 이름은 UE 접두를 뗀 나머지다. UE의 숨은 값과 _MAX 표지는 두지 않는다. 저장
// 형식이라 바꾸지 않는다.
namespace rocks::material {

// DepthOfFieldFunctionValue 값이다(UE EDepthOfFieldFunctionValue —
// RE/Public/Materials/MaterialExpressionDepthOfFieldFunction.h).
ROCKS_ENUM()
enum class DepthOfFieldFunctionValue : uint8 {
    NearAndFarMask = 0,
    NearMask = 1,
    FarMask = 2,
    CircleOfConfusionRadius = 3
};

} // namespace rocks::material
