// 머티리얼 공통 원문이다(Docs/Design/MaterialSystem 7.2·7.3·7.7절). 문맥 구조체(pass 원문과 정점 입력
// 타입이 채운다), 생성 원문이 부르는 보조 함수, 표면 속성 정의를 담고 끝에 job마다 연결되는 생성 파일
// /Engine/Generated/Material.rsh를 include한다. 머티리얼 pass 셰이더만 include한다(엔진 전역
// 셰이더가 include하면 생성 파일 연결이 없어 전처리 오류다 — 7.2절).
//
// 사용 예(pass 원문):
//   #include "/Engine/Private/Material/MaterialCommon.rsh"
//   MaterialInputs inputs;
//   inputs.svPosition = position;
//   SurfaceAttributes surface = EvaluateMaterialSurface(inputs);
#pragma once

#include "/Engine/Private/Common/ShaderPrelude.rsh"
#include "/Engine/Private/Shading/SurfaceAttributes.rsh"

// 정점 단계 문맥이다(7.3절). MS2a는 첫 칸만 있고 MS2b가 문맥 값 표(19.3.2절)대로 채운다.
struct MaterialVertexInputs {
    // 카메라 상대 world 위치(cm)다.
    float3 translatedWorldPosition;
};

// 정점 함수의 결과다.
struct MaterialVertexResult {
    // world 공간 위치 오프셋(cm)이다(UE WorldPositionOffset).
    float3 worldPositionOffset;
};

// 픽셀 단계 문맥이다(Renderer 14.1절 이름). MS2a는 첫 칸만 있다.
struct MaterialInputs {
    // 화면 위치(SV_Position)다.
    float4 svPosition;
};

// 벡터 조건 선택·논리곱·논리합이다(7.7절). HLSL 2021은 벡터 ?:·&&·||를 막고 select·and·or를 준다.
#if ROCKS_HLSL2021
#define MaterialSelect(condition, whenTrue, whenFalse) select(condition, whenTrue, whenFalse)
#define MaterialAnd(left, right) and(left, right)
#define MaterialOr(left, right) or(left, right)
#else
#define MaterialSelect(condition, whenTrue, whenFalse) ((condition) ? (whenTrue) : (whenFalse))
#define MaterialAnd(left, right) ((left) && (right))
#define MaterialOr(left, right) ((left) || (right))
#endif

// float 비트에서 부호를 뺀 마스크와 지수가 모두 1인 값이다(NaN·Inf 판정).
static const uint MaterialFloatMagnitudeMask = 0x7fffffffu;
static const uint MaterialFloatInfinityBits = 0x7f800000u;

// NaN·Inf·유한 판정이다. fast-math가 isnan·isinf를 지울 수 있어 비트로 본다(6.2절).
#define MATERIAL_FLOAT_CLASS(Name, Compare)                                                           \
    bool Name(float x) {                                                                          \
        return (asuint(x) & MaterialFloatMagnitudeMask) Compare MaterialFloatInfinityBits;        \
    }                                                                                             \
    bool2 Name(float2 x) {                                                                        \
        return bool2(Name(x.x), Name(x.y));                                                       \
    }                                                                                             \
    bool3 Name(float3 x) {                                                                        \
        return bool3(Name(x.x), Name(x.y), Name(x.z));                                            \
    }                                                                                             \
    bool4 Name(float4 x) {                                                                        \
        return bool4(Name(x.x), Name(x.y), Name(x.z), Name(x.w));                                 \
    }

MATERIAL_FLOAT_CLASS(MaterialIsNan, >)
MATERIAL_FLOAT_CLASS(MaterialIsInf, ==)
MATERIAL_FLOAT_CLASS(MaterialIsFinite, <)

#undef MATERIAL_FLOAT_CLASS

// 성분마다 같은 스칼라 함수를 부르는 벡터 판이다.
#define MATERIAL_COMPONENTWISE_UNARY(Name)                                                            \
    float2 Name(float2 x) {                                                                       \
        return float2(Name(x.x), Name(x.y));                                                      \
    }                                                                                             \
    float3 Name(float3 x) {                                                                       \
        return float3(Name(x.x), Name(x.y), Name(x.z));                                           \
    }                                                                                             \
    float4 Name(float4 x) {                                                                       \
        return float4(Name(x.x), Name(x.y), Name(x.z), Name(x.w));                                \
    }

// UE acosFast(SH/Private/FastMathThirdParty.ush — Eberly 1차 다항식, 최대 절대 오차 9.0e-3)와 같은
// 식이다. 입력 [-1, 1], 출력 [0, π].
static const float MaterialAcosFastSlope = -0.156583;

float MaterialAcosFast(float inX) {
    const float x = abs(inX);
    float result = MaterialAcosFastSlope * x + HalfPi;
    result *= sqrt(1.0 - x);
    return inX >= 0.0 ? result : Pi - result;
}

MATERIAL_COMPONENTWISE_UNARY(MaterialAcosFast)

// UE asinFast: π/2 − acosFast(x).
float MaterialAsinFast(float x) {
    return HalfPi - MaterialAcosFast(x);
}

MATERIAL_COMPONENTWISE_UNARY(MaterialAsinFast)

// UE atanFastPos·atanFast(Eberly 5차 홀수 다항식, 최대 절대 오차 1.3e-3)의 계수다.
static const float MaterialAtanFastC5 = 0.0872929;
static const float MaterialAtanFastC3 = -0.301895;

// 입력 [0, ∞), 출력 [0, π/2].
float MaterialAtanFastPositive(float x) {
    const float t0 = x < 1.0 ? x : 1.0 / x;
    const float t1 = t0 * t0;
    float poly = MaterialAtanFastC5;
    poly = MaterialAtanFastC3 + poly * t1;
    poly = 1.0 + poly * t1;
    poly = poly * t0;
    return x < 1.0 ? poly : HalfPi - poly;
}

float MaterialAtanFast(float x) {
    const float t0 = MaterialAtanFastPositive(abs(x));
    return x < 0.0 ? -t0 : t0;
}

MATERIAL_COMPONENTWISE_UNARY(MaterialAtanFast)

// UE atan2Fast와 같은 식이다.
float MaterialAtan2Fast(float y, float x) {
    float t0 = max(abs(x), abs(y));
    const float t1 = min(abs(x), abs(y));
    float t3 = t1 / t0;
    const float t4 = t3 * t3;
    t0 = MaterialAtanFastC5;
    t0 = t0 * t4 + MaterialAtanFastC3;
    t0 = t0 * t4 + 1.0;
    t3 = t0 * t3;
    t3 = abs(y) > abs(x) ? HalfPi - t3 : t3;
    t3 = x < 0.0 ? Pi - t3 : t3;
    t3 = y < 0.0 ? -t3 : t3;
    return t3;
}

float2 MaterialAtan2Fast(float2 y, float2 x) {
    return float2(MaterialAtan2Fast(y.x, x.x), MaterialAtan2Fast(y.y, x.y));
}

float3 MaterialAtan2Fast(float3 y, float3 x) {
    return float3(MaterialAtan2Fast(y.x, x.x), MaterialAtan2Fast(y.y, x.y),
                  MaterialAtan2Fast(y.z, x.z));
}

float4 MaterialAtan2Fast(float4 y, float4 x) {
    return float4(MaterialAtan2Fast(y.x, x.x), MaterialAtan2Fast(y.y, x.y),
                  MaterialAtan2Fast(y.z, x.z), MaterialAtan2Fast(y.w, x.w));
}

// HLSL에 없는 역쌍곡 함수다(로그 식 — UE IR 연산 ACosh·ASinh·ATanh와 같은 뜻).
float MaterialAcosh(float x) {
    return log(x + sqrt((x - 1.0) * (x + 1.0)));
}

MATERIAL_COMPONENTWISE_UNARY(MaterialAcosh)

// 음수에서 x + sqrt(x² + 1)의 상쇄(큰 음수면 log(0))를 피하려고 |x|로 계산하고 부호를 붙인다.
float MaterialAsinh(float x) {
    const float magnitude = abs(x);
    return sign(x) * log(magnitude + sqrt(magnitude * magnitude + 1.0));
}

MATERIAL_COMPONENTWISE_UNARY(MaterialAsinh)

float MaterialAtanh(float x) {
    return 0.5 * log((1.0 + x) / (1.0 - x));
}

MATERIAL_COMPONENTWISE_UNARY(MaterialAtanh)

#undef MATERIAL_COMPONENTWISE_UNARY

// 번역 결과다(job마다 메모리 텍스트로 연결된다 — 7.2절).
#include "/Engine/Generated/Material.rsh"
