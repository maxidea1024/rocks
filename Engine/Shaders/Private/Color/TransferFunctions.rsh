// 전달 함수(부호화·복호화)다(Docs/Design/EngineShaders 4.3절 A03·7.4절). 값은 채널별이고 float·float3
// overload를 둔다. 음수 입력은 0으로 본다(PQ·HLG·감마의 정의역 밖).
// 근거: IEC 61966-2-1:1999(sRGB 구간 함수), ITU-R BT.709-6(OETF), ITU-R BT.1886(EOTF — 검정 0·흰색 1의
// 감마 2.4), SMPTE ST 2084:2014(PQ — nit, 10000 nit = 1), ITU-R BT.2100-2 표 5(HLG OETF·역).
// LUT log shaper는 Rocks 정의다(아래 상수 — 18% 회색 기준 정지 수 범위를 [0, 1]로).
#pragma once

#include "/Engine/Private/Math/ScalarMath.rsh"

// sRGB 구간 상수다(IEC 61966-2-1).
static const float SrgbLinearThreshold = 0.0031308;
static const float SrgbEncodedThreshold = 0.04045;
static const float SrgbLinearSlope = 12.92;
static const float SrgbGamma = 2.4;
static const float SrgbScale = 1.055;
static const float SrgbOffset = 0.055;
// BT.709 OETF 상수다.
static const float Rec709LinearThreshold = 0.018;
static const float Rec709LinearSlope = 4.5;
static const float Rec709Scale = 1.099;
static const float Rec709Offset = 0.099;
static const float Rec709Exponent = 0.45;
// BT.1886 EOTF 감마다.
static const float Rec1886Gamma = 2.4;
// PQ 상수(ST 2084)와 최대 휘도(nit)다.
static const float PqM1 = 0.1593017578125;
static const float PqM2 = 78.84375;
static const float PqC1 = 0.8359375;
static const float PqC2 = 18.8515625;
static const float PqC3 = 18.6875;
static const float PqMaxNits = 10000.0;
// HLG 상수(BT.2100)다.
static const float HlgA = 0.17883277;
static const float HlgB = 0.28466892;
static const float HlgC = 0.55991073;
static const float HlgLinearThreshold = 1.0 / 12.0;
static const float HlgEncodedThreshold = 0.5;
// LUT log shaper: 18% 회색 기준 [-10, +6.5] 정지(0.18·2^-10 – 0.18·2^6.5 ≈ 16.3)를 [0, 1]로 둔다.
static const float LutLogMiddleGrey = 0.18;
static const float LutLogMinStops = -10.0;
static const float LutLogMaxStops = 6.5;

// 선형 → sRGB 부호(구간 함수 정확)다.
float LinearToSrgb(float x) {
    const float v = max(x, 0.0);
    return v <= SrgbLinearThreshold ? v * SrgbLinearSlope : SrgbScale * PowPositive(v, 1.0 / SrgbGamma) - SrgbOffset;
}

float3 LinearToSrgb(float3 x) {
    return float3(LinearToSrgb(x.x), LinearToSrgb(x.y), LinearToSrgb(x.z));
}

// sRGB 부호 → 선형이다.
float SrgbToLinear(float x) {
    const float v = max(x, 0.0);
    return v <= SrgbEncodedThreshold ? v / SrgbLinearSlope : PowPositive((v + SrgbOffset) / SrgbScale, SrgbGamma);
}

float3 SrgbToLinear(float3 x) {
    return float3(SrgbToLinear(x.x), SrgbToLinear(x.y), SrgbToLinear(x.z));
}

// 순수 감마 부호화 x^(1/gamma)·복호화 x^gamma다.
float3 LinearToGamma(float3 x, float gamma) {
    return PowPositive(x, 1.0 / gamma);
}

float3 GammaToLinear(float3 x, float gamma) {
    return PowPositive(x, gamma);
}

// BT.709 OETF(장면 선형 → 신호)다.
float LinearToRec709(float x) {
    const float v = max(x, 0.0);
    return v < Rec709LinearThreshold ? v * Rec709LinearSlope : Rec709Scale * PowPositive(v, Rec709Exponent) - Rec709Offset;
}

float3 LinearToRec709(float3 x) {
    return float3(LinearToRec709(x.x), LinearToRec709(x.y), LinearToRec709(x.z));
}

// BT.1886 EOTF(신호 → 표시 선형, 검정 0·흰색 1)다.
float3 Rec1886ToLinear(float3 x) {
    return PowPositive(x, Rec1886Gamma);
}

// 휘도(nit) → PQ 신호 [0, 1]다(10000 nit 위는 1).
float LinearToPq(float nits) {
    const float y = PowPositive(saturate(nits / PqMaxNits), PqM1);
    return PowPositive((PqC1 + PqC2 * y) / (1.0 + PqC3 * y), PqM2);
}

float3 LinearToPq(float3 nits) {
    return float3(LinearToPq(nits.x), LinearToPq(nits.y), LinearToPq(nits.z));
}

// PQ 신호 → 휘도(nit)다.
float PqToLinear(float signal) {
    const float e = PowPositive(saturate(signal), 1.0 / PqM2);
    return PqMaxNits * PowPositive(max(e - PqC1, 0.0) / (PqC2 - PqC3 * e), 1.0 / PqM1);
}

float3 PqToLinear(float3 signal) {
    return float3(PqToLinear(signal.x), PqToLinear(signal.y), PqToLinear(signal.z));
}

// HLG OETF(장면 선형 [0, 1] → 신호 [0, 1])다.
float LinearToHlg(float x) {
    const float v = max(x, 0.0);
    return v <= HlgLinearThreshold ? sqrt(3.0 * v) : HlgA * log(12.0 * v - HlgB) + HlgC;
}

float3 LinearToHlg(float3 x) {
    return float3(LinearToHlg(x.x), LinearToHlg(x.y), LinearToHlg(x.z));
}

// HLG 역 OETF다.
float HlgToLinear(float signal) {
    const float v = max(signal, 0.0);
    return v <= HlgEncodedThreshold ? v * v / 3.0 : (exp((v - HlgC) / HlgA) + HlgB) / 12.0;
}

float3 HlgToLinear(float3 signal) {
    return float3(HlgToLinear(signal.x), HlgToLinear(signal.y), HlgToLinear(signal.z));
}

// 3D LUT용 log2 shaper(선형 → [0, 1])다. LutLogMiddleGrey · 2^LutLogMinStops 아래는 0, 2^LutLogMaxStops
// 위는 1이다.
float3 LinearToLutLog(float3 x) {
    const float3 v = max(x, LutLogMiddleGrey * exp2(LutLogMinStops)) / LutLogMiddleGrey;
    return saturate((log2(v) - LutLogMinStops) / (LutLogMaxStops - LutLogMinStops));
}

float3 LutLogToLinear(float3 x) {
    return LutLogMiddleGrey * exp2(lerp(LutLogMinStops, LutLogMaxStops, saturate(x)));
}
