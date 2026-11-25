// 미세면 BRDF 항이다(Docs/Design/EngineShaders 4.4절 A04·7.5절, 결정 SLD3). 각 cos 인자는 호출자가 [0, 1]로
// 자른 값이다(NoH·NoV·NoL·VoH). α는 GGX 폭(= roughness² — RoughnessToAlpha)이다. BRDF 반사 항은
// D · Vis · F이고 Vis = G / (4 NoL NoV)다(분모를 Vis에 넣은 형태).
// 근거: Walter, Marschner, Li, Torrance, "Microfacet Models for Refraction through Rough Surfaces", EGSR
// 2007(GGX D, 굴절 Fresnel); Heitz, "Understanding the Masking-Shadowing Function in Microfacet-Based
// BRDFs", JCGT 3(2) 2014(높이 상관 Smith G2, Λ); Hammon, "PBR Diffuse Lighting for GGX+Smith
// Microsurfaces", GDC 2017(Vis 선형 근사); Schlick, "An Inexpensive BRDF Model for Physically-based
// Rendering", CGF 13(3) 1994; Estevez, Kulla, "Production Friendly Microfacet Sheen BRDF", Imageworks 2017
// (Charlie); Ashikhmin, Premoze, "Distribution-based BRDFs", 2007, Neubelt, Pettineo, "Crafting a Next-Gen
// Material Pipeline for The Order: 1886", SIGGRAPH 2013 Course(inverse GGX — 뒤집힌 Gaussian 옷감 분포);
// Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013 Course(α = roughness²).
#pragma once

#include "/Engine/Private/Common/ShaderPrelude.rsh"

// roughness 하한이다(α = 0.02² — 점 광원 하이라이트가 float에서 무한히 좁아지지 않게 한다).
static const float MinimumGgxRoughness = 0.02;
// Charlie·inverse GGX 폭 하한이다(0 나눗셈 방지).
static const float MinimumClothRoughness = 0.0078125;
// inverse GGX에서 θ = 0 근처 sin⁴ 하한이다.
static const float MinimumSinSquared = 1.0e-7;

// roughness(지각 roughness, [0, 1]) → GGX α(= roughness²)다. MinimumGgxRoughness 아래는 하한이다.
float RoughnessToAlpha(float roughness) {
    return Square(max(roughness, MinimumGgxRoughness));
}

// 등방 GGX(Trowbridge–Reitz) 분포 D(h) = α² / (π ((n·h)²(α² − 1) + 1)²)다. ∫D(h)(n·h)dω = 1.
float DistributionGgx(float alpha, float NoH) {
    const float a2 = alpha * alpha;
    const float d = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (Pi * d * d);
}

// 이방 GGX 분포다. XoH·YoH는 tangent·bitangent와 h의 내적, alphaX·alphaY는 두 축 폭이다.
float DistributionGgxAniso(float alphaX, float alphaY, float NoH, float XoH, float YoH) {
    const float3 v = float3(alphaY * XoH, alphaX * YoH, alphaX * alphaY * NoH);
    const float a2 = alphaX * alphaY;
    const float w = a2 / dot(v, v);
    return a2 * w * w * InversePi;
}

// GGX Smith Λ(등방)다: Λ(v) = (−1 + √(1 + α² tan²θ)) / 2.
float SmithLambdaGgx(float alpha, float NoV) {
    const float cos2 = max(NoV * NoV, SmallestSafeSquaredLength);
    const float tan2 = (1.0 - cos2) / cos2;
    return 0.5 * (-1.0 + sqrt(1.0 + alpha * alpha * tan2));
}

// GGX Smith 마스킹 G1(v) = 1 / (1 + Λ(v))다(VNDF pdf에 쓴다).
float SmithG1Ggx(float alpha, float NoV) {
    return 2.0 * NoV / (NoV + sqrt(alpha * alpha + (1.0 - alpha * alpha) * NoV * NoV));
}

// 이방 GGX Smith G1이다. view는 국소 좌표(z = 법선) 방향이다.
float SmithG1GgxAniso(float3 view, float2 alpha) {
    const float z2 = max(view.z * view.z, SmallestSafeSquaredLength);
    const float lambda = 0.5 * (-1.0 + sqrt(1.0 + (Square(alpha.x * view.x) + Square(alpha.y * view.y)) / z2));
    return 1.0 / (1.0 + lambda);
}

// 높이 상관 Smith 가시성 Vis = G2 / (4 NoL NoV), G2 = 1 / (1 + Λ(l) + Λ(v))다(Heitz 2014 식 99).
float VisibilitySmithJointGgx(float alpha, float NoV, float NoL) {
    const float a2 = alpha * alpha;
    const float lambdaV = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    const float lambdaL = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(lambdaV + lambdaL, SmallestSafeDivisor);
}

// 높이 상관 Smith 가시성의 선형 근사다(Hammon 2017 — sqrt 없음). 정확한 식과의 차이는 시험이 잰다.
float VisibilitySmithJointGgxApprox(float alpha, float NoV, float NoL) {
    return 0.5 / max(lerp(2.0 * NoL * NoV, NoL + NoV, alpha), SmallestSafeDivisor);
}

// Schlick Fresnel F0 + (F90 − F0)(1 − VoH)⁵다.
float3 FresnelSchlick(float3 f0, float f90, float VoH) {
    return f0 + (f90 - f0) * Pow5(1.0 - VoH);
}

float FresnelSchlick(float f0, float f90, float VoH) {
    return f0 + (f90 - f0) * Pow5(1.0 - VoH);
}

// 유전체 정확 Fresnel(비편광 평균)이다. eta = 나가는 쪽 굴절률 / 들어오는 쪽 굴절률, cosIncident ∈ [0,
// 1]이다. 전반사면 1이다(Walter 2007 식 22).
float FresnelDielectric(float eta, float cosIncident) {
    const float g2 = eta * eta - 1.0 + cosIncident * cosIncident;
    const float g = sqrt(max(g2, 0.0));
    const float a = (g - cosIncident) / (g + cosIncident);
    const float b = (cosIncident * (g + cosIncident) - 1.0) / (cosIncident * (g - cosIncident) + 1.0);
    return g2 < 0.0 ? 1.0 : 0.5 * a * a * (1.0 + b * b);
}

// Charlie sheen 분포 D = (2 + 1/r) sin^(1/r)θ / (2π)다(Estevez, Kulla 2017, r = 옷감 roughness).
float DistributionCharlie(float roughness, float NoH) {
    const float inverse = 1.0 / max(roughness, MinimumClothRoughness);
    const float sin2 = max(1.0 - NoH * NoH, 0.0);
    return (2.0 + inverse) * PowPositive(sin2, inverse * 0.5) * InverseTwoPi;
}

// 뒤집힌 Gaussian(inverse GGX) 옷감 분포 D = (1 + 4 exp(−cot²θ / α²) / sin⁴θ) / (π(1 + 4α²))다
// (Ashikhmin, Premoze 2007 분포에 Neubelt, Pettineo 2013 정규화 — ∫D cosθ dω = 1).
float DistributionInverseGgx(float alpha, float NoH) {
    const float a2 = max(alpha * alpha, MinimumClothRoughness * MinimumClothRoughness);
    const float cos2 = NoH * NoH;
    const float sin2 = max(1.0 - cos2, MinimumSinSquared);
    return (1.0 + 4.0 * exp(-cos2 / (sin2 * a2)) / (sin2 * sin2)) / (Pi * (1.0 + 4.0 * a2));
}
