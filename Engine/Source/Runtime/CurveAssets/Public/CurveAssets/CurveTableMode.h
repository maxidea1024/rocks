#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 커브 표의 행 종류다(Docs/Design/CurveAssets 3.2.1절, UE ECurveTableMode와 같은 번호).
namespace rocks {

// - Empty: 행이 없다(두 맵 모두 쓰지 않는다).
// - Plain: 행이 PlainCurve다(UE SimpleCurves — 커브 하나에 보간 하나).
// - Tangent: 행이 TangentCurve다(UE RichCurves — 키마다 보간·접선).
ROCKS_ENUM()
enum class CurveTableMode : uint8 {
    Empty = 0,
    Plain = 1,
    Tangent = 2,
};

} // namespace rocks
