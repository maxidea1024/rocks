// Monte Carlo 표본 사상과 pdf다(Docs/Design/EngineShaders 4.2절 A02·7.3절). 입력 u ∈ [0, 1)²(균일
// 표본)를 정의역 위 방향·점으로 옮긴다. 방향은 국소 좌표(z = 법선·원뿔 축, 단위 벡터)이고 pdf는
// 입체각 측도(sr⁻¹)다. 세계 좌표로는 Math/VectorMath BuildOrthonormalBasis로 옮긴다.
// 근거: Shirley, Chiu, "A Low Distortion Map Between Disk and Square", JGT 2(3) 1997(동심원 사상),
// Malley 방법(코사인 반구 = 디스크 위로 투영), Heitz, "Sampling the GGX Distribution of Visible
// Normals", JCGT 7(4) 2018(VNDF), Pharr, Jakob, Humphreys, PBRT 3판 13.6절(구·반구·원뿔 사상과 pdf).
#pragma once

#include "/Engine/Private/Math/ScalarMath.rsh"

// 정사각형 [0, 1)² → 단위 디스크(동심원 사상, 면적 보존·연속)다. 결과 pdf는 1/π(면적)다.
float2 ConcentricSquareToDisk(float2 u) {
    const float2 a = u * 2.0 - 1.0;
    float radius;
    float phi;
    if (abs(a.x) > abs(a.y)) {
        radius = a.x;
        phi = (Pi * 0.25) * (a.y / a.x);
    } else {
        radius = a.y;
        phi = HalfPi - (Pi * 0.25) * (a.x / (a.y == 0.0 ? 1.0 : a.y));
    }

    return radius * float2(cos(phi), sin(phi));
}

// 구 전체 균일 방향이다. pdf = 1/(4π).
float3 UniformSphere(float2 u) {
    const float z = 1.0 - 2.0 * u.x;
    const float r = SafeSqrt(1.0 - z * z);
    const float phi = TwoPi * u.y;
    return float3(r * cos(phi), r * sin(phi), z);
}

float UniformSpherePdf() {
    return 0.25 * InversePi;
}

// 위 반구(z ≥ 0) 균일 방향이다. pdf = 1/(2π).
float3 UniformHemisphere(float2 u) {
    const float z = u.x;
    const float r = SafeSqrt(1.0 - z * z);
    const float phi = TwoPi * u.y;
    return float3(r * cos(phi), r * sin(phi), z);
}

float UniformHemispherePdf() {
    return InverseTwoPi;
}

// 코사인 가중 반구 방향(Malley — 디스크 점을 반구로 올린다)이다. pdf = cos θ / π.
float3 CosineHemisphere(float2 u) {
    const float2 disk = ConcentricSquareToDisk(u);
    return float3(disk, SafeSqrt(1.0 - dot(disk, disk)));
}

float CosineHemispherePdf(float cosTheta) {
    return max(cosTheta, 0.0) * InversePi;
}

// z 축 둘레 반각 cos가 cosThetaMax인 원뿔 안 균일 방향이다. pdf = 1 / (2π(1 − cosThetaMax)).
// cosThetaMax = 1(점 원뿔)은 z 축 하나다(pdf 무한 — 쓰지 않는다).
float3 UniformCone(float2 u, float cosThetaMax) {
    const float cosTheta = lerp(1.0, cosThetaMax, u.x);
    const float sinTheta = SafeSqrt(1.0 - cosTheta * cosTheta);
    const float phi = TwoPi * u.y;
    return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

float UniformConePdf(float cosThetaMax) {
    return InverseTwoPi / (1.0 - cosThetaMax);
}

// GGX 가시 법선 분포(VNDF)에서 미세면 법선을 고른다(Heitz 2018 Listing 1). view는 국소 좌표의 보는
// 방향(표면에서 눈 쪽, view.z > 0), alpha = (αx, αy)다. 결과 h의 pdf는 GgxVisibleNormalPdf다.
float3 SampleGgxVisibleNormal(float3 view, float2 alpha, float2 u) {
    // 반구 배치로 늘인다(3.2절).
    const float3 stretched = normalize(float3(alpha.x * view.x, alpha.y * view.y, view.z));
    // 직교 기저(4.1절, 수직이면 x 축).
    const float lengthSquared = stretched.x * stretched.x + stretched.y * stretched.y;
    const float3 t1 = lengthSquared > 0.0 ? float3(-stretched.y, stretched.x, 0.0) * rsqrt(lengthSquared)
                                          : float3(1.0, 0.0, 0.0);
    const float3 t2 = cross(stretched, t1);
    // 투영 면적 매개화(4.2절).
    const float r = sqrt(u.x);
    const float phi = TwoPi * u.y;
    const float p1 = r * cos(phi);
    const float s = 0.5 * (1.0 + stretched.z);
    const float p2 = lerp(SafeSqrt(1.0 - p1 * p1), r * sin(phi), s);
    // 반구로 다시 투영(4.3절)하고 타원체 배치로 되돌린다(3.4절).
    const float3 hemisphereNormal = p1 * t1 + p2 * t2 + SafeSqrt(1.0 - p1 * p1 - p2 * p2) * stretched;
    return normalize(float3(alpha.x * hemisphereNormal.x, alpha.y * hemisphereNormal.y,
                            max(0.0, hemisphereNormal.z)));
}

// VNDF 표본 h의 pdf D_v(h) = G1(v) · max(0, v·h) · D(h) / v.z다(Heitz 2018 식 1). 분포 D(h)와 마스킹
// G1(v)는 호출자가 Shading/MicrofacetTerms로 계산해 넘긴다(같은 식을 두 곳에 두지 않는다). 반사 방향
// l = reflect(−v, h)의 pdf는 이 값 / (4 v·h)다.
float GgxVisibleNormalPdf(float maskingG1, float viewDotHalf, float distribution, float viewZ) {
    return maskingG1 * max(viewDotHalf, 0.0) * distribution / viewZ;
}
