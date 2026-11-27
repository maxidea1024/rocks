#pragma once

#include <Core/Containers/Span.h>
#include <Graphics/DeviceTypes.h>

// noise 시스템 텍스처다(Docs/Design/RenderBase 11.5절). 머티리얼 Noise 노드의 LUT 함수
// (SimplexTex·GradientTex·GradientTex3D)가 읽는 gradient 표 둘이며, UE 5.8 Renderer 시스템
// 텍스처(PerlinNoiseGradient·PerlinNoise3D)와 byte 단위로 같은 내용이다. 내장 자원 팩
// (BuiltinResources — BuiltinTexture::PerlinGradient2D·PerlinGradient3D)이 렌더 lane 시작 단계에서
// 이 byte로 만든다. View 공유 자원에 묶는 것은 Renderer RS0다.
//
// 사용 예(CPU 참조와 GPU readback 대조):
//   Span<const uint8> expected = render::PerlinGradient2DTexels();
//   device.ReadTexture(pack.Texture(render::BuiltinTexture::PerlinGradient2D), 0, 0, out);
//   // out과 expected가 byte 단위로 같아야 한다.
//
// 셰이더 복원 규칙(UE Random.ush와 같다):
// - 2D(point·wrap): texel 중심을 읽어 gradient = s.xyz * 2 - 1(A는 0, 쓰지 않는다).
// - 3D(bilinear·wrap): uvw = p / 16 + 0.5 / 16로 읽어 n = s.xyz * 255 / 127 - 1,
//   d = s.w * 255 - 127, 결과 dot(xyz, n) - d.
//
// 스레드: 모든 함수는 상수·정적 수명 데이터만 돌려주므로 어느 스레드에서나 부를 수 있다.
namespace rocks::render {

// 2D 표의 한 변 texel 수다(128 × 128).
inline constexpr uint32 PerlinGradient2DExtent = 128;
// 3D 표의 한 변 texel 수다(16 × 16 × 16 — 마지막 면은 첫 면의 복제).
inline constexpr uint32 PerlinGradient3DExtent = 16;
// 두 표의 texel 하나의 byte다(RGBA8Unorm).
inline constexpr uint32 NoiseTexelBytes = 4;
// 두 표의 전체 byte다(행 우선, 3D는 z 면 순서 — Device::CreateTexture 초기 픽셀 배치).
inline constexpr usize PerlinGradient2DBytes =
    static_cast<usize>(PerlinGradient2DExtent) * PerlinGradient2DExtent * NoiseTexelBytes;
inline constexpr usize PerlinGradient3DBytes = static_cast<usize>(PerlinGradient3DExtent) *
                                               PerlinGradient3DExtent * PerlinGradient3DExtent *
                                               NoiseTexelBytes;

// 2D gradient 표 byte다(RGBA 순서, 정적 수명). 컴파일 때 만든 CPU 참조이자 팩의 업로드 원본이다.
ROCKS_RENDERBASE_API Span<const uint8> PerlinGradient2DTexels() noexcept;

// 3D gradient·내적 표 byte다(RGBA 순서, 정적 수명). A는 그 texel 좌표와 gradient의 내적 + 127이다.
ROCKS_RENDERBASE_API Span<const uint8> PerlinGradient3DTexels() noexcept;

// 2D 표의 장치 desc다: Texture2D 128 × 128, RGBA8Unorm, mip 1, Sampled, 샘플러 point·wrap.
ROCKS_RENDERBASE_API graphics::TextureDesc PerlinGradient2DDesc() noexcept;

// 3D 표의 장치 desc다: Texture3D 16 × 16 × 16, RGBA8Unorm, mip 1, Sampled, 샘플러 bilinear·wrap.
ROCKS_RENDERBASE_API graphics::TextureDesc PerlinGradient3DDesc() noexcept;

} // namespace rocks::render
