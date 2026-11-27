// 3D 색 LUT 좌표다(Docs/Design/EngineShaders 4.3절 A03·14.3절 1항, UE TonemapCommon.ush·
// PostProcessCombineLUTs.usf의 LUT 좌표 역할). LUT 한 변 size(기본 DefaultLutSize)는 인자로 받는다.
//
// 좌표 규칙:
// - 단위 좌표 [0, 1]³은 LUT의 첫·끝 texel 중심에 맞춘다(반 texel 보정, LutTexelCenterCoordinate).
// - 선형 색은 log shaper(TransferFunctions.rsh LinearToLutLog)로 [0, 1]에 놓는다.
// - 2D 펼친 LUT(볼륨 렌더 타깃이 없는 LUT 생성 경로): 폭 size², 높이 size, x = 파랑 slice × size + 빨강,
//   y = 초록이다.
//
// 사용 예(tone map pass가 32³ LUT를 읽는다):
//   const float3 coordinate = LinearColorToLutCoordinate(sceneColor, (float)DefaultLutSize);
//   const float3 display = ColorLut.SampleLevel(ColorLutSampler, coordinate, 0.0).rgb;
#pragma once

#include "/Engine/Private/Color/TransferFunctions.rsh"

// UE r.LUT.Size 기본값과 같은 LUT 한 변이다.
static const uint DefaultLutSize = 32u;

// 단위 좌표 → 반 texel 보정 좌표(unit · (size − 1)/size + 0.5/size)다.
float3 LutTexelCenterCoordinate(float3 unitCoordinate, float size) {
    return unitCoordinate * ((size - 1.0) / size) + 0.5 / size;
}

// 선형 색 → LUT 표본 좌표(log shaper + 반 texel 보정)다.
float3 LinearColorToLutCoordinate(float3 linearColor, float size) {
    return LutTexelCenterCoordinate(LinearToLutLog(linearColor), size);
}

// LUT texel(0 – size − 1)의 중립 단위 좌표(texel/(size − 1))다. LUT 생성 pass가 texel마다 입력으로 쓴다.
float3 LutTexelToNeutralColor(uint3 texel, float size) {
    return float3(texel) / (size - 1.0);
}

// LUT texel이 나타내는 선형 색(중립 좌표의 log shaper 역)이다.
float3 LutTexelToLinearColor(uint3 texel, float size) {
    return LutLogToLinear(LutTexelToNeutralColor(texel, size));
}

// 2D 펼친 LUT 픽셀 → LUT texel(빨강·초록·파랑)이다.
uint3 UnwrappedLutPixelToTexel(uint2 pixel, uint size) {
    return uint3(pixel.x % size, pixel.y, pixel.x / size);
}

// 3D LUT를 반 texel 보정 좌표로 표본한다(mip 0).
float4 SampleLut3D(Texture3D lut, SamplerState lutSampler, float3 unitCoordinate, float size) {
    return lut.SampleLevel(lutSampler, LutTexelCenterCoordinate(unitCoordinate, size), 0.0);
}

// 2D 펼친 LUT를 3D LUT처럼 삼선형으로 표본한다: 파랑 축의 이웃 두 slice를 bilinear로 각각 표본해 선형
// 보간한다. 샘플러는 bilinear·clamp다.
float4 SampleUnwrappedLut2D(Texture2D lut, SamplerState lutSampler, float3 unitCoordinate, float size) {
    const float3 unit = saturate(unitCoordinate);
    const float blue = unit.b * (size - 1.0);
    const float firstSlice = min(floor(blue), size - 2.0);
    const float blend = blue - max(firstSlice, 0.0);
    const float slice = max(firstSlice, 0.0);
    const float u = (unit.r * (size - 1.0) + 0.5) / (size * size);
    const float v = (unit.g * (size - 1.0) + 0.5) / size;
    const float4 lower = lut.SampleLevel(lutSampler, float2(u + slice / size, v), 0.0);
    const float4 upper = lut.SampleLevel(lutSampler, float2(u + min(slice + 1.0, size - 1.0) / size, v), 0.0);
    return lerp(lower, upper, blend);
}
