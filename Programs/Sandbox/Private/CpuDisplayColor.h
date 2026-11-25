#pragma once

#include "ScreenRenderer.h"

// 화면 경로 self-test의 CPU 기준이다(Docs/Design/RenderBase 18.5절 3·5·7항). tone map(UE 5.8 filmic
// — Engine/Shaders/Private/Color/FilmicToneCurve.rsh와 같은 식)과 텍스처 표시(TextureInspector.rsf
// — 쿠킹 swizzle → 표시 방식)를 double로 계산하고 capture view의 sRGB 부호화로 8 bit 값을 낸다.
//
// 사용 예:
//   const DisplayPixel expected = ExpectedTonemapPixel(sceneColor);
//   const DisplayPixel texel = ExpectedInspectorPixel(texture, decoded, tonemap);
namespace sandbox {

using namespace rocks;

// capture 픽셀 하나(RGB 8 bit, sRGB 부호화)다.
struct DisplayPixel {
    int rgb[3];
};

// 선형 → sRGB 8 bit(스왑체인·capture sRGB view의 부호화)다.
int EncodeSrgb8(double linear) noexcept;

// 장면 색(선형 HDR)의 tone map 결과다(노출 배율 포함, constants 기본값은 UE FPostProcessSettings).
DisplayPixel ExpectedTonemapPixel(const float (&scene)[3],
                                  const render::TonemapConstants& constants = {}) noexcept;

// 텍스처 표시 픽셀이다: texel(셰이더가 읽는 값 — TexelCodec DecodeTexel) → 쿠킹 swizzle → 정수면
// 배율 → 표시 방식(LDR 그대로·HDR 노출 + filmic·깊이 빨강 회색).
DisplayPixel ExpectedInspectorPixel(const screen::InspectorTexture& texture,
                                    const float (&texel)[4],
                                    const render::TonemapConstants& constants = {}) noexcept;

} // namespace sandbox
