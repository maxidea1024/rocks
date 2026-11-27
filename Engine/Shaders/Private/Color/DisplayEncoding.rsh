// 출력 장치 부호화다(Docs/Design/EngineShaders 4.3절 A03·14.3절 2항). 선형 표시 색(tone map 뒤)을 출력 장치
// 신호로 바꾼다. 지금은 SDR sRGB(IEC 61966-2-1 구간 함수)만이다 — HDR10(Rec.2020 + PQ)·scRGB는 후속이고
// 그 값은 #error로 막는다(장치 쪽은 HDR 스왑체인을 요청하지 않는다).
//
// include 전에 DISPLAY_OUTPUT_DEVICE(출력 장치 번호 — pass 퍼뮤테이션)를 정의한다. 스왑체인 view가
// sRGB인 경로(RenderBase 18.5절 1항)는 부호화를 view가 하므로 이 파일을 쓰지 않는다.
//
// 사용 예(UNORM 대상에 쓰는 tone map):
//   #define DISPLAY_OUTPUT_DEVICE 0
//   #include "/Engine/Private/Color/DisplayEncoding.rsh"
//   return float4(EncodeDisplayColor(displayColor), 1.0);
#pragma once

#include "/Engine/Private/Color/TransferFunctions.rsh"

// 출력 장치 번호다.
static const uint DisplayOutputSdrSrgb = 0u;

#if !defined(DISPLAY_OUTPUT_DEVICE)
#error "define DISPLAY_OUTPUT_DEVICE (0 = SDR sRGB) before including DisplayEncoding.rsh"
#endif

#if DISPLAY_OUTPUT_DEVICE != 0
#error "only DISPLAY_OUTPUT_DEVICE 0 (SDR sRGB) is supported; HDR outputs are a later stage"
#endif

// 선형 표시 색 → SDR sRGB 신호다. [0, 1] 밖은 자른다(SDR 범위).
float3 EncodeSdrSrgbDisplay(float3 linearDisplayColor) {
    return LinearToSrgb(saturate(linearDisplayColor));
}

// DISPLAY_OUTPUT_DEVICE가 고른 부호화다.
float3 EncodeDisplayColor(float3 linearDisplayColor) {
    return EncodeSdrSrgbDisplay(linearDisplayColor);
}
