// 실수 구면 조화(SH)다(Docs/Design/EngineShaders 4.2절 A02·7.3절). 2차(띠 0–1, 계수 4)·3차(띠 0–2,
// 계수 9), 채널 하나. 기저 순서는 (l, m) = (0,0), (1,-1), (1,0), (1,1), (2,-2), (2,-1), (2,0), (2,1),
// (2,2)이고 정규화는 ∫Y_i Y_j dω = δ_ij(Sloan, "Stupid Spherical Harmonics (SH) Tricks", GDC 2008 부록
// 식)다. 방향은 단위 벡터이고 축 이름은 world 축 그대로다(z가 극 — Z-up world에서 극이 위).
// 조도: Ramamoorthi, Hanrahan, "An Efficient Representation for Irradiance Environment Maps",
// SIGGRAPH 2001(코사인 엽 계수 Â₀ = π, Â₁ = 2π/3, Â₂ = π/4). 창: Sloan 2008 "windowing"(Hanning).
//
// 사용 예(방향 표본으로 radiance를 투영하고 법선의 조도를 읽는다):
//   SphericalHarmonics3 sh = ZeroSH3();
//   sh = AddSH3(sh, ProjectDirectionSH3(direction, radiance * weight));
//   float irradiance = EvaluateIrradianceSH3(sh, normal);
#pragma once

#include "/Engine/Private/Math/MathConstants.rsh"

// 기저 상수(√(1/4π), √(3/4π), √(15/4π), √(5/16π), √(15/16π))다.
static const float SHBand0 = 0.28209479177387814;
static const float SHBand1 = 0.48860251190291992;
static const float SHBand2Product = 1.09254843059207907;
static const float SHBand2Zonal = 0.31539156525252001;
static const float SHBand2Difference = 0.54627421529603954;
// 코사인 엽 convolution 계수(Ramamoorthi 2001)다.
static const float SHCosineLobeBand0 = 3.14159265358979323846;
static const float SHCosineLobeBand1 = 2.09439510239319549231;
static const float SHCosineLobeBand2 = 0.78539816339744830962;

struct SphericalHarmonics2 {
    // (0,0), (1,-1), (1,0), (1,1)
    float4 coefficients;
};

struct SphericalHarmonics3 {
    // (0,0), (1,-1), (1,0), (1,1)
    float4 band01;
    // (2,-2), (2,-1), (2,0), (2,1)
    float4 band2;
    // (2,2)
    float band2Last;
};

SphericalHarmonics2 ZeroSH2() {
    SphericalHarmonics2 sh;
    sh.coefficients = float4(0.0, 0.0, 0.0, 0.0);
    return sh;
}

SphericalHarmonics3 ZeroSH3() {
    SphericalHarmonics3 sh;
    sh.band01 = float4(0.0, 0.0, 0.0, 0.0);
    sh.band2 = float4(0.0, 0.0, 0.0, 0.0);
    sh.band2Last = 0.0;
    return sh;
}

// 단위 방향의 기저 값이다.
SphericalHarmonics2 BasisSH2(float3 d) {
    SphericalHarmonics2 sh;
    sh.coefficients = float4(SHBand0, SHBand1 * d.y, SHBand1 * d.z, SHBand1 * d.x);
    return sh;
}

SphericalHarmonics3 BasisSH3(float3 d) {
    SphericalHarmonics3 sh;
    sh.band01 = float4(SHBand0, SHBand1 * d.y, SHBand1 * d.z, SHBand1 * d.x);
    sh.band2 = float4(SHBand2Product * d.x * d.y, SHBand2Product * d.y * d.z,
                      SHBand2Zonal * (3.0 * d.z * d.z - 1.0), SHBand2Product * d.x * d.z);
    sh.band2Last = SHBand2Difference * (d.x * d.x - d.y * d.y);
    return sh;
}

SphericalHarmonics2 AddSH2(SphericalHarmonics2 a, SphericalHarmonics2 b) {
    a.coefficients += b.coefficients;
    return a;
}

SphericalHarmonics3 AddSH3(SphericalHarmonics3 a, SphericalHarmonics3 b) {
    a.band01 += b.band01;
    a.band2 += b.band2;
    a.band2Last += b.band2Last;
    return a;
}

SphericalHarmonics2 ScaleSH2(SphericalHarmonics2 a, float s) {
    a.coefficients *= s;
    return a;
}

SphericalHarmonics3 ScaleSH3(SphericalHarmonics3 a, float s) {
    a.band01 *= s;
    a.band2 *= s;
    a.band2Last *= s;
    return a;
}

// ∫a·b dω(계수 내적)다.
float DotSH2(SphericalHarmonics2 a, SphericalHarmonics2 b) {
    return dot(a.coefficients, b.coefficients);
}

float DotSH3(SphericalHarmonics3 a, SphericalHarmonics3 b) {
    return dot(a.band01, b.band01) + dot(a.band2, b.band2) + a.band2Last * b.band2Last;
}

// 방향 d에서 온 값 value(델타 표본 하나)의 투영이다. 균일 표본 N개를 투영하면 각 표본에 4π/N을
// 곱해 더한다.
SphericalHarmonics2 ProjectDirectionSH2(float3 d, float value) {
    return ScaleSH2(BasisSH2(d), value);
}

SphericalHarmonics3 ProjectDirectionSH3(float3 d, float value) {
    return ScaleSH3(BasisSH3(d), value);
}

// 함수의 SH와 클램프 코사인 엽 max(0, n·ω)의 convolution이다(띠 l에 Â_l를 곱한다). radiance SH를
// 넣으면 결과를 법선 방향에서 평가한 값이 조도(irradiance)다.
SphericalHarmonics2 ConvolveCosineLobeSH2(SphericalHarmonics2 sh) {
    sh.coefficients *= float4(SHCosineLobeBand0, SHCosineLobeBand1, SHCosineLobeBand1, SHCosineLobeBand1);
    return sh;
}

SphericalHarmonics3 ConvolveCosineLobeSH3(SphericalHarmonics3 sh) {
    sh.band01 *= float4(SHCosineLobeBand0, SHCosineLobeBand1, SHCosineLobeBand1, SHCosineLobeBand1);
    sh.band2 *= SHCosineLobeBand2;
    sh.band2Last *= SHCosineLobeBand2;
    return sh;
}

// 방향 d에서 SH 함수 값이다.
float EvaluateSH2(SphericalHarmonics2 sh, float3 d) {
    return DotSH2(sh, BasisSH2(d));
}

float EvaluateSH3(SphericalHarmonics3 sh, float3 d) {
    return DotSH3(sh, BasisSH3(d));
}

// radiance SH와 단위 법선에서 조도 E(n) = ∫L(ω) max(0, n·ω) dω다(Lambert 확산 radiance는 albedo·E/π).
float EvaluateIrradianceSH2(SphericalHarmonics2 radiance, float3 normal) {
    return EvaluateSH2(ConvolveCosineLobeSH2(radiance), normal);
}

float EvaluateIrradianceSH3(SphericalHarmonics3 radiance, float3 normal) {
    return EvaluateSH3(ConvolveCosineLobeSH3(radiance), normal);
}

// 띠 l의 Hanning 창 가중 (1 + cos(πl / width)) / 2다(width > 0, l ≥ width면 0). 잘린 SH의 ringing
// (음수 엽)을 줄인다. width = 3이면 3차 SH 띠 2를 0.25배 한다.
float HanningWindowWeight(float band, float width) {
    return band >= width ? 0.0 : 0.5 * (1.0 + cos(Pi * band / width));
}

SphericalHarmonics3 WindowSH3(SphericalHarmonics3 sh, float width) {
    const float w1 = HanningWindowWeight(1.0, width);
    const float w2 = HanningWindowWeight(2.0, width);
    sh.band01 *= float4(1.0, w1, w1, w1);
    sh.band2 *= w2;
    sh.band2Last *= w2;
    return sh;
}
