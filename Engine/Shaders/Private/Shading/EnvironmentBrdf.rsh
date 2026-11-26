// split-sum 환경 BRDF다(Docs/Design/EngineShaders 4.4절 A04·7.5절). SL0에는 해석 근사만 둔다(LUT 표본은
// 내장 자원 팩이 들어오는 SL3). 결과 (scale, bias)로 반사 = F0 · scale + F90 · bias다.
// 근거: Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013 Course(split sum); Karis, "Physically
// Based Shading on Mobile", Unreal Engine Blog 2014(Lazarov의 Call of Duty 근사를 고친 해석 식 —
// 계수는 그 글). 수치 적분(GGX·Smith) 대비 오차는 시험이 재서 기록한다(7.5절).
#pragma once

#include "/Engine/Private/Common/ShaderPrelude.rsh"

static const float4 EnvironmentBrdfRoughnessScale = float4(-1.0, -0.0275, -0.572, 0.022);
static const float4 EnvironmentBrdfRoughnessBias = float4(1.0, 0.0425, 1.04, -0.04);
static const float EnvironmentBrdfFalloff = -9.28;
static const float2 EnvironmentBrdfSlope = float2(-1.04, 1.04);

// (scale, bias)다. NoV ∈ [0, 1], roughness ∈ [0, 1].
float2 EnvironmentBrdfApprox(float NoV, float roughness) {
    const float4 r = roughness * EnvironmentBrdfRoughnessScale + EnvironmentBrdfRoughnessBias;
    const float a004 = min(r.x * r.x, exp2(EnvironmentBrdfFalloff * NoV)) * r.x + r.y;
    return EnvironmentBrdfSlope * a004 + r.zw;
}

// 환경 반사 배율 F0 · scale + F90 · bias다.
float3 EnvironmentBrdfApproxSpecular(float3 f0, float f90, float NoV, float roughness) {
    const float2 scaleBias = EnvironmentBrdfApprox(NoV, roughness);
    return f0 * scaleBias.x + f90 * scaleBias.y;
}
