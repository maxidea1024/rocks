// 디버그 시각화용 색 지도다(Docs/Design/EngineShaders 4.3절 A03·14.3절 3항). 반환 값 공간을 이름에 적는다:
// *Srgb는 색 지도 정의 그대로의 sRGB 부호 값, 접미사 없는 판은 그 선형 값(sRGB 대상에 쓰는 셰이더용)이다.
// 근거:
// - Turbo: Mikhailov, "Turbo, An Improved Rainbow Colormap for Visualization", Google AI Blog 2019 — 함께
//   발표된 성분별 5차 다항 근사(Apache 2.0 공개 계수). 표 끝에서 근사 오차가 크다(Sandbox 측정 — 엔진 셰이더
//   14.9절).
// - viridis: van der Walt, Smith, matplotlib viridis(CC0) 표에 맞춘 Zucker의 6차 다항(CC0).
// - 구분 색: 정수 해시(IntegerHash.rsh HashPcg) 상위 24 bit의 색상 + 고정 채도·명도 HSV(sRGB 부호 공간).
//
// 사용 예(깊이 복잡도 시각화):
//   return float4(TurboColorMap(saturate(overdraw / maxOverdraw)), 1.0);
#pragma once

#include "/Engine/Private/Color/ColorSpaces.rsh"
#include "/Engine/Private/Color/TransferFunctions.rsh"
#include "/Engine/Private/Math/IntegerHash.rsh"

// Turbo 다항 계수(빨강·초록·파랑, 0–3차와 4–5차)다.
static const float4 TurboRedLow = float4(0.13572138, 4.61539260, -42.66032258, 132.13108234);
static const float4 TurboGreenLow = float4(0.09140261, 2.19418839, 4.84296658, -14.18503333);
static const float4 TurboBlueLow = float4(0.10667330, 12.64194608, -60.58204836, 110.36276771);
static const float2 TurboRedHigh = float2(-152.94239396, 59.28637943);
static const float2 TurboGreenHigh = float2(4.27729857, 2.82956604);
static const float2 TurboBlueHigh = float2(-89.90310912, 27.34824973);

// viridis 다항 계수(0–6차)다.
static const float3 ViridisC0 = float3(0.2777273272234177, 0.005407344544966578, 0.3340998053353061);
static const float3 ViridisC1 = float3(0.1050930431085774, 1.404613529898575, 1.384590162594685);
static const float3 ViridisC2 = float3(-0.3308618287255563, 0.214847559468213, 0.09509516302823659);
static const float3 ViridisC3 = float3(-4.634230498983486, -5.799100973351585, -19.33244095627987);
static const float3 ViridisC4 = float3(6.228269936347081, 14.17993336680509, 56.69055260068105);
static const float3 ViridisC5 = float3(4.776384997670288, -13.74514537774601, -65.35303263337234);
static const float3 ViridisC6 = float3(-5.435455855934631, 4.645852612178535, 26.3124352495832);

// 구분 색의 채도·명도(HSV, sRGB 부호 공간)와 색상 bit 배율(2^-24)이다.
static const float DistinctColorSaturation = 0.75;
static const float DistinctColorValue = 0.9;
static const uint DistinctColorHueShift = 8u;
static const float DistinctColorHueScale = 5.9604644775390625e-8;

// Turbo 색 지도(sRGB 부호 값, t는 saturate)다.
float3 TurboColorMapSrgb(float t) {
    const float x = saturate(t);
    const float4 low = float4(1.0, x, x * x, x * x * x);
    const float2 high = low.zw * low.z;
    return saturate(float3(dot(low, TurboRedLow) + dot(high, TurboRedHigh),
                           dot(low, TurboGreenLow) + dot(high, TurboGreenHigh),
                           dot(low, TurboBlueLow) + dot(high, TurboBlueHigh)));
}

// viridis 색 지도(sRGB 부호 값, t는 saturate)다.
float3 ViridisColorMapSrgb(float t) {
    const float x = saturate(t);
    return saturate(ViridisC0 +
                    x * (ViridisC1 + x * (ViridisC2 + x * (ViridisC3 + x * (ViridisC4 + x * (ViridisC5 + x * ViridisC6))))));
}

// Turbo 색 지도의 선형 값이다.
float3 TurboColorMap(float t) {
    return SrgbToLinear(TurboColorMapSrgb(t));
}

// viridis 색 지도의 선형 값이다.
float3 ViridisColorMap(float t) {
    return SrgbToLinear(ViridisColorMapSrgb(t));
}

// 정수 → 구분 색(선형 값)이다. 이웃 번호가 비슷한 색이 되지 않고 결정적이다.
float3 DistinctIndexColor(uint index) {
    const float hue = (float)(HashPcg(index) >> DistinctColorHueShift) * DistinctColorHueScale;
    return SrgbToLinear(HsvToRgb(float3(hue, DistinctColorSaturation, DistinctColorValue)));
}
