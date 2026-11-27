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

// 쿠킹용 결정적 판이다(설계 2.4.5절, CurveData 5.9절 D22). 위 두 함수와 같은 식이고 수학만
// 결정적이다: 채널은 TangentCurve::EvaluateDeterministic, 조정의 거듭제곱 둘은 C powf 의미를 지키는
// 결정적 거듭제곱(DeterministicPowf)이다. HSV의 fmod는 정확한 연산이라 그대로다. 결과 성분의 NaN은
// 고정 quiet NaN이다. 쿠킹 byte(atlas 원본·머티리얼 상수)에 들어가는 곳만 쓴다 — 런타임은 위
// 함수(UE 비트).
//
// 사용 예(atlas 굽기):
//   const LinearColor baked = EvaluateAdjustedColorCurveDeterministic(r, g, b, a, adjustments, t,
//   false);
ROCKS_CURVEASSETS_API LinearColor EvaluateUnadjustedColorCurveDeterministic(const TangentCurve& r,
                                                                            const TangentCurve& g,
                                                                            const TangentCurve& b,
                                                                            const TangentCurve& a,
                                                                            float time) noexcept;
ROCKS_CURVEASSETS_API LinearColor EvaluateAdjustedColorCurveDeterministic(
    const TangentCurve& r, const TangentCurve& g, const TangentCurve& b, const TangentCurve& a,
    const ColorCurveAdjustments& adjustments, float time, bool clampAlways) noexcept;

// C powf와 같은 의미의 결정적 거듭제곱이다(설계 2.4.5절): 지수 ±0이거나 밑이 1이면 1(NaN 입력
// 포함), 그 밖의 NaN은 NaN, 밑 > 0은 deterministic::Pow, ±0 밑은 홀수 정수 지수에서 부호를 지키고
// (음수 지수면 ±Inf), −Inf 밑은 홀수 정수 지수에서 부호를 지키고 그 밖은 +Inf·+0, 유한 음수 밑은
// 정수 지수(±Inf 포함 — 짝수로 본다)면 |밑|의 거듭제곱에 홀수 지수의 부호, 아니면 NaN이다(밑 −1과
// 지수 ±Inf는 1). 결과 NaN은 고정 quiet NaN이다.
ROCKS_CURVEASSETS_API float DeterministicPowf(float base, float exponent) noexcept;

} // namespace rocks
