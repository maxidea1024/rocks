#pragma once

#include <Platform/PlatformMath.h>

// dB ↔ 선형 진폭 변환이다(UE Audio::ConvertToLinear·ConvertToDecibels —
// SignalProcessing/Public/DSP/Dsp.h).
//
// 사용 예:
//   const float gain = DecibelsToLinear(-6.0f);           // 약 0.501
//   const float floor = DecibelsToLinear(-96.0f);         // 블록마다 한 번 계산한다
//   const float level = LinearToDecibels(0.5f, floor);    // 약 −6.02 dB, 0이면 −96 dB
namespace rocks::audio {

// 진폭 1 decade(×10)가 20 dB다.
inline constexpr float DecibelsPerAmplitudeDecade = 20.0f;

// ln 10이다(log10 = ln/ln 10).
inline constexpr float NaturalLogOfTen = 2.302585092994046f;

// 10^(dB/20)이다.
inline float DecibelsToLinear(float decibels) noexcept {
    return platform::PlatformMath::Pow(10.0f, decibels / DecibelsPerAmplitudeDecade);
}

// 20·log10(max(linear, floorLinear))이다(UE ArrayMagnitudeToDecibelInPlace). floorLinear는 0보다
// 큰 하한 진폭이고, 그 이하·NaN 진폭은 하한의 dB가 된다.
inline float LinearToDecibels(float linear, float floorLinear) noexcept {
    const float clamped = linear > floorLinear ? linear : floorLinear;
    return DecibelsPerAmplitudeDecade * platform::PlatformMath::Log(clamped) / NaturalLogOfTen;
}

} // namespace rocks::audio
