// 스칼라·벡터 기본 보조다(Docs/Design/EngineShaders 4.2절 A02·7.3절). HLSL 2018이라 벡터 폭마다
// 손으로 쓴 overload를 둔다(템플릿 없음 — 7.1절). NaN·Inf 입력: 판정 함수(IsFiniteValue)와
// 치환(ReplaceNonFinite)은 bit 패턴으로 판정하므로 fast-math 최적화에도 결과가 같다. 다른 함수의
// NaN·Inf 결과는 정의하지 않는다(시험하지 않는다).
#pragma once

#include "/Engine/Private/Math/MathConstants.rsh"

// float 지수 비트 mask다(모두 1이면 Inf·NaN).
static const uint FloatExponentMask = 0x7f800000u;

// x².
float Square(float x) {
    return x * x;
}

float2 Square(float2 x) {
    return x * x;
}

float3 Square(float3 x) {
    return x * x;
}

float4 Square(float4 x) {
    return x * x;
}

// x^2(곱셈만 — pow의 exp2·log2 오차가 없다).
float Pow2(float x) {
    return x * x;
}

float2 Pow2(float2 x) {
    return x * x;
}

float3 Pow2(float3 x) {
    return x * x;
}

float4 Pow2(float4 x) {
    return x * x;
}

// x^3(곱셈만 — pow의 exp2·log2 오차가 없다).
float Pow3(float x) {
    return x * x * x;
}

float2 Pow3(float2 x) {
    return x * x * x;
}

float3 Pow3(float3 x) {
    return x * x * x;
}

float4 Pow3(float4 x) {
    return x * x * x;
}

// x^4(곱셈만 — pow의 exp2·log2 오차가 없다).
float Pow4(float x) {
    return Square(x * x);
}

float2 Pow4(float2 x) {
    return Square(x * x);
}

float3 Pow4(float3 x) {
    return Square(x * x);
}

float4 Pow4(float4 x) {
    return Square(x * x);
}

// x^5(곱셈만 — pow의 exp2·log2 오차가 없다).
float Pow5(float x) {
    return Square(x * x) * x;
}

float2 Pow5(float2 x) {
    return Square(x * x) * x;
}

float3 Pow5(float3 x) {
    return Square(x * x) * x;
}

float4 Pow5(float4 x) {
    return Square(x * x) * x;
}

// 셋 가운데 최대·최소다(성분별).
float Max3(float a, float b, float c) {
    return max(a, max(b, c));
}

float2 Max3(float2 a, float2 b, float2 c) {
    return max(a, max(b, c));
}

float3 Max3(float3 a, float3 b, float3 c) {
    return max(a, max(b, c));
}

float4 Max3(float4 a, float4 b, float4 c) {
    return max(a, max(b, c));
}

float Min3(float a, float b, float c) {
    return min(a, min(b, c));
}

float2 Min3(float2 a, float2 b, float2 c) {
    return min(a, min(b, c));
}

float3 Min3(float3 a, float3 b, float3 c) {
    return min(a, min(b, c));
}

float4 Min3(float4 a, float4 b, float4 c) {
    return min(a, min(b, c));
}

// 0이 아닌 쪽 부호(±1)다. 0·-0은 +1이다(팔면체 인코딩 등 접기 규칙).
float SignNotZero(float x) {
    return x >= 0.0 ? 1.0 : -1.0;
}

float2 SignNotZero(float2 x) {
    return float2(SignNotZero(x.x), SignNotZero(x.y));
}

float3 SignNotZero(float3 x) {
    return float3(SignNotZero(x.x), SignNotZero(x.y), SignNotZero(x.z));
}

// 1/x를 0 나눗셈 없이 구한다. |x| < SmallestSafeDivisor면 부호를 지킨 ±1/SmallestSafeDivisor(유한
// 1e30)다(0·-0은 +). 그 밖은 1/x(사양 2.5 ulp — 8.3절).
float SafeRcp(float x) {
    return 1.0 / (SignNotZero(x) * max(abs(x), SmallestSafeDivisor));
}

float2 SafeRcp(float2 x) {
    return 1.0 / (SignNotZero(x) * max(abs(x), SmallestSafeDivisor));
}

float3 SafeRcp(float3 x) {
    return 1.0 / (SignNotZero(x) * max(abs(x), SmallestSafeDivisor));
}

float4 SafeRcp(float4 x) {
    return float4(SafeRcp(x.xyz), SafeRcp(x.w));
}

// 음수를 0으로 본 제곱근이다(음수 입력 → 0).
float SafeSqrt(float x) {
    return sqrt(max(x, 0.0));
}

float2 SafeSqrt(float2 x) {
    return sqrt(max(x, 0.0));
}

float3 SafeSqrt(float3 x) {
    return sqrt(max(x, 0.0));
}

float4 SafeSqrt(float4 x) {
    return sqrt(max(x, 0.0));
}

// v를 정규화한다. 길이 제곱이 SmallestSafeSquaredLength보다 작으면(0 벡터 포함) fallback을
// 그대로 돌려준다(fallback은 호출자가 정한 단위 벡터).
float2 SafeNormalize(float2 v, float2 fallback) {
    const float lengthSquared = dot(v, v);
    return lengthSquared >= SmallestSafeSquaredLength ? v * rsqrt(lengthSquared) : fallback;
}

float3 SafeNormalize(float3 v, float3 fallback) {
    const float lengthSquared = dot(v, v);
    return lengthSquared >= SmallestSafeSquaredLength ? v * rsqrt(lengthSquared) : fallback;
}

float4 SafeNormalize(float4 v, float4 fallback) {
    const float lengthSquared = dot(v, v);
    return lengthSquared >= SmallestSafeSquaredLength ? v * rsqrt(lengthSquared) : fallback;
}

// [-1, 1]로 자른다.
float SaturateSigned(float x) {
    return clamp(x, -1.0, 1.0);
}

float2 SaturateSigned(float2 x) {
    return clamp(x, -1.0, 1.0);
}

float3 SaturateSigned(float3 x) {
    return clamp(x, -1.0, 1.0);
}

float4 SaturateSigned(float4 x) {
    return clamp(x, -1.0, 1.0);
}

// (x - edge0) / (edge1 - edge0)를 [0, 1]로 자른 선형 계단이다. edge0 == edge1이면 x >= edge1일 때
// 1, 아니면 0이다(0 나눗셈 없음).
float LinearStep(float edge0, float edge1, float x) {
    const float range = edge1 - edge0;
    const float stepValue = x >= edge1 ? 1.0 : 0.0;
    return range == 0.0 ? stepValue : saturate((x - edge0) / (range == 0.0 ? 1.0 : range));
}

// Perlin 5차 계단 t³(6t² − 15t + 10)이다(Perlin 2002 — 1·2차 도함수가 양 끝에서 0). smoothstep의
// 3차 계단보다 경계가 부드럽다. 인자 규칙은 LinearStep과 같다.
float SmootherStep(float edge0, float edge1, float x) {
    const float t = LinearStep(edge0, edge1, x);
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

// 유한수(NaN·±Inf가 아님)인가(bit 판정).
bool IsFiniteValue(float x) {
    return (asuint(x) & FloatExponentMask) != FloatExponentMask;
}

bool IsFiniteValue(float2 x) {
    return IsFiniteValue(x.x) && IsFiniteValue(x.y);
}

bool IsFiniteValue(float3 x) {
    return IsFiniteValue(x.x) && IsFiniteValue(x.y) && IsFiniteValue(x.z);
}

bool IsFiniteValue(float4 x) {
    return IsFiniteValue(x.x) && IsFiniteValue(x.y) && IsFiniteValue(x.z) && IsFiniteValue(x.w);
}

// NaN·±Inf 성분을 replacement로 바꾼다(성분별).
float ReplaceNonFinite(float x, float replacement) {
    return IsFiniteValue(x) ? x : replacement;
}

float2 ReplaceNonFinite(float2 x, float2 replacement) {
    return float2(ReplaceNonFinite(x.x, replacement.x), ReplaceNonFinite(x.y, replacement.y));
}

float3 ReplaceNonFinite(float3 x, float3 replacement) {
    return float3(ReplaceNonFinite(x.x, replacement.x), ReplaceNonFinite(x.y, replacement.y), ReplaceNonFinite(x.z, replacement.z));
}

float4 ReplaceNonFinite(float4 x, float4 replacement) {
    return float4(ReplaceNonFinite(x.x, replacement.x), ReplaceNonFinite(x.y, replacement.y), ReplaceNonFinite(x.z, replacement.z), ReplaceNonFinite(x.w, replacement.w));
}

// x^y(x > 0)다. x ≤ 0이면 0이다(pow의 음수 정의역 경고와 GLSL log2(0) 미정의를 피한다). 정밀도는
// exp2(y·log2 x) 사양 상한이다(8.3절).
float PowPositive(float x, float y) {
    return x > 0.0 ? exp2(y * log2(x)) : 0.0;
}

float3 PowPositive(float3 x, float y) {
    return float3(PowPositive(x.x, y), PowPositive(x.y, y), PowPositive(x.z, y));
}
