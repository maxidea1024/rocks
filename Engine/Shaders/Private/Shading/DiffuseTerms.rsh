// 확산 BRDF 항이다(Docs/Design/EngineShaders 4.4절 A04·7.5절). 결과는 BRDF 값(sr⁻¹ — cos 인자를 곱하기
// 전)이고 albedo를 곱한다. cos 인자는 호출자가 [0, 1]로 자른 값이다.
// 근거: Lambert 1760; Burley, "Physically-Based Shading at Disney", SIGGRAPH 2012 Course(Burley 확산);
// Lagarde, de Rousiers, "Moving Frostbite to Physically Based Rendering 3.0", SIGGRAPH 2014 Course 식
// 6(에너지 재정규화 — 반사 항과 더해 1을 넘지 않게); Chan, "Material Advances in Call of Duty: WWII",
// SIGGRAPH 2018 Course(roughness 의존 retro-reflection 확산 — 광택 g와 α² = 2 / (1 + 2^(18g)) 관계,
// 다항식 계수는 그 발표의 식); Barre-Brisebois, Bouchard, "Approximating Translucency for a Fast,
// Cheap and Convincing Subsurface Scattering Look", GDC 2011(wrap 조명).
#pragma once

#include "/Engine/Private/Common/ShaderPrelude.rsh"

// Burley 재정규화 계수다(Lagarde 2014 — roughness 1에서 1/1.51).
static const float BurleyEnergyBiasScale = 0.5;
static const float BurleyRoughEnergyFactor = 1.0 / 1.51;
// Chan 2018 광택 매개 상수다(α² = 2 / (1 + 2^(18g))).
static const float ChanGlossExponentScale = 18.0;

// Lambert: albedo / π다.
float3 DiffuseLambert(float3 albedo) {
    return albedo * InversePi;
}

// 재정규화 Burley 확산이다(Lagarde 2014): fd90 = 0.5·r + 2 LoH² r, 에너지 배율 lerp(1, 1/1.51, r).
float3 DiffuseBurley(float3 albedo, float roughness, float NoV, float NoL, float LoH) {
    const float energyBias = BurleyEnergyBiasScale * roughness;
    const float energyFactor = lerp(1.0, BurleyRoughEnergyFactor, roughness);
    const float fd90 = energyBias + 2.0 * LoH * LoH * roughness;
    const float lightScatter = 1.0 + (fd90 - 1.0) * Pow5(1.0 - NoL);
    const float viewScatter = 1.0 + (fd90 - 1.0) * Pow5(1.0 - NoV);
    return albedo * (lightScatter * viewScatter * energyFactor * InversePi);
}

// Chan 2018 확산이다. roughness가 크면 retro-reflection 항이 생기고 작으면 Burley형 가장자리 감쇠로
// 간다. 발표의 광택 g = log2(2/α² − 1) / 18(α = roughness²)로 두 응답을 섞는다.
float3 DiffuseChan(float3 albedo, float roughness, float NoV, float NoL, float VoH, float NoH) {
    const float alpha2 = max(Pow4(roughness), SmallestSafeDivisor);
    const float gloss = saturate(log2(max(2.0 / alpha2 - 1.0, 1.0)) / ChanGlossExponentScale);
    const float rough = VoH + Pow5(1.0 - VoH);
    const float smoothView = 1.0 - 0.75 * Pow5(1.0 - NoV);
    const float smoothLight = 1.0 - 0.75 * Pow5(1.0 - NoL);
    const float response = lerp(rough, smoothView * smoothLight, saturate(2.2 * gloss - 0.5));
    const float retro = ((34.5 * gloss - 59.0) * gloss + 24.5) * VoH *
                        exp2(-max(73.2 * gloss - 21.2, 8.9) * sqrt(NoH));
    return albedo * ((response + retro) * InversePi);
}

// wrap 조명 cos 인자 max(0, (n·l + w) / (1 + w)) / (1 + w)다(w ∈ [0, 1] — w = 0이면 Lambert cos). 적분이
// Lambert와 같도록 1 + w로 한 번 더 나눈다.
float WrapLighting(float NoL, float wrap) {
    return saturate((NoL + wrap) / (1.0 + wrap)) / (1.0 + wrap);
}
