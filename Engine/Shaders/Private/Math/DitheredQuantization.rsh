// 렌더 타깃 쓰기 전 dither 양자화다(Docs/Design/EngineShaders 4.2절 A02·7.3절). bits bit 대상에 값을
// 쓸 때 고른 잡음 noise ∈ [0, 1)을 더하고 내림해 양자화 오차의 기대값을 0으로 만든다(띠 무늬 제거 —
// Roberts, "Dithering in Computer Graphics" 일반 원리; 잡음은 ScreenNoise·LowDiscrepancy가 준다).
//
// 값은 대상의 부호 영역에 있어야 한다. sRGB 렌더 타깃이면 LinearToSrgb(Color/TransferFunctions)로
// 바꾼 값을 넘기고 결과를 sRGB 부호 값으로 쓴다(선형 영역에서 dither하면 어두운 쪽 단계가 sRGB 부호
// 간격보다 커진다). 결과는 [0, 1]의 격자 값 k / (2^bits − 1)이다.
//
// 사용 예:
//   float noise = InterleavedGradientNoise(floor(position.xy), frame);
//   output = QuantizeWithDither(LinearToSrgb(color), 8u, noise);
#pragma once

#include "/Engine/Private/Math/ScalarMath.rsh"

float3 QuantizeWithDither(float3 value, uint bits, float noise) {
    const float maxCode = (float)((1u << bits) - 1u);
    precise float3 scaled = saturate(value) * maxCode + noise;
    return min(floor(scaled), maxCode) / maxCode;
}

float QuantizeWithDither(float value, uint bits, float noise) {
    const float maxCode = (float)((1u << bits) - 1u);
    precise float scaled = saturate(value) * maxCode + noise;
    return min(floor(scaled), maxCode) / maxCode;
}
