#pragma once

#include <GraphicsTypes/PixelFormat.h>

// 비압축 texel 하나의 부호화·해석이다(Sandbox 텍스처 시험 — Docs/Design/RenderBase 18.5절 7항).
// 메모리 플랫폼 데이터 장면이 texel을 쓰고, CPU 기준이 GPU가 읽을 값을 계산한다. 성분이 모두 같은
// 크기인 색 형식(8·16·32 bit, unorm·uint·int·float, RGBA·BGRA 순서, sRGB)만
// 다룬다(packed·압축·깊이는 false).
//
// 사용 예:
//   const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
//   uint8 bytes[16];
//   EncodeTexel(PixelFormat::RGBA16Float, red, bytes);
//   float value[4];
//   DecodeTexel(PixelFormat::RGBA16Float, bytes, value);   // (1, 0, 0, 1)
namespace sandbox {

using namespace rocks;

// 다루는 형식인가.
bool IsTexelCodecFormat(graphics::PixelFormat format) noexcept;

// value(선형 값 — 정수 형식은 정수 값, unorm은 [0, 1]로 자름)를 out(형식 texel byte 수)에 쓴다.
// sRGB 형식은 색 성분을 sRGB로 부호화한다(알파는 선형). 형식에 없는 성분은 버린다.
bool EncodeTexel(graphics::PixelFormat format, const float (&value)[4], uint8* out) noexcept;

// in의 texel을 셰이더가 읽는 값으로 해석한다(형식에 없는 G·B는 0, A는 1 — 그래픽 API 규칙, sRGB는
// 선형으로 풀고 정수는 정수 값 그대로).
bool DecodeTexel(graphics::PixelFormat format, const uint8* in, float (&out)[4]) noexcept;

// 선형 값 [0, 1]의 sRGB 부호화·해석이다(IEC 61966-2-1).
double LinearToSrgb(double linear) noexcept;
double SrgbToLinear(double encoded) noexcept;

} // namespace sandbox
