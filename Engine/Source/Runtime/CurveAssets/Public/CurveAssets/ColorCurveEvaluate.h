#pragma once

#include <Core/Math/Color.h>
#include <CurveAssets/ColorCurveAdjustments.h>
#include <Curves/TangentCurve.h>

// 색 커브 평가의 공통 식이다(Docs/Design/CurveAssets 2.4절, UE UCurveLinearColor::
// GetUnadjustedLinearColorValue·GetAdjustedColorValue). ColorCurveAsset·ColorCurveValue·
// ColorCurveSource가 같은 식을 쓴다. 할당하지 않고 스레드 제약이 없다(커브를 읽기만 한다).
namespace rocks {

// 조정 없는 색이다: R·G·B는 각 커브의 Evaluate(time)(fallback 0), A는 a에 키가 없으면 1(a의
// defaultValue도 보지 않는다 — UE와 같다), 있으면 Evaluate(time). 범위 밖 값도 자르지 않는다.
//
// 사용 예:
//   const LinearColor raw = EvaluateUnadjustedColorCurve(r, g, b, a, age);
ROCKS_CURVEASSETS_API LinearColor EvaluateUnadjustedColorCurve(const TangentCurve& r,
                                                               const TangentCurve& g,
                                                               const TangentCurve& b,
                                                               const TangentCurve& a,
                                                               float time) noexcept;

// 조정한 색이다(설계 2.4.2절 열한 단계 — UE GetAdjustedColorValue와 같은 float 식·순서·비교 형태,
// HSV는 UE float 식이고 Core LinearColor의 HSV는 쓰지 않는다). clampAlways면 명도를 늘 [0, 1]로
// 자르고(UE GetClampedLinearColorValue), 아니면 원본 RGB가 LDR일 때만 자른다(UE
// GetLinearColorValue).
//
// 사용 예:
//   const LinearColor tint = EvaluateAdjustedColorCurve(r, g, b, a, adjustments, age, false);
ROCKS_CURVEASSETS_API LinearColor EvaluateAdjustedColorCurve(
    const TangentCurve& r, const TangentCurve& g, const TangentCurve& b, const TangentCurve& a,
    const ColorCurveAdjustments& adjustments, float time, bool clampAlways) noexcept;

} // namespace rocks
