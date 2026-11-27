#pragma once

// 커브 에셋 층의 이름 있는 상수다(Docs/Design/CurveAssets 2.4.2절).
namespace rocks {

// 색 조정의 "거의 같음" 허용치다(UE UE_KINDA_SMALL_NUMBER). 밝기 곡선 1·활기 0 판정과 원본 RGB가
// LDR인지 판정(최댓값 < 1 + 허용치)에 쓴다.
inline constexpr float ColorAdjustTolerance = 1.0e-4f;

// 활기 조정의 계수와 채도 지수다(UE AdjustColorsOld — S += clamp(활기) × 계수 × (1 − S)^지수).
inline constexpr float VibranceScale = 0.5f;
inline constexpr float VibranceSaturationExponent = 5.0f;

// 색상(도)의 한 바퀴다(HSV 색상 감기).
inline constexpr float HueDegreesPerTurn = 360.0f;

} // namespace rocks
