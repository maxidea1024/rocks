#pragma once

#include <Core/Containers/Span.h>
#include <GraphicsTypes/PixelFormat.h>

// 메모리 장면용 단순 블록 인코더다(Sandbox 텍스처 시험 — Docs/Design/RenderBase 18.5절 7항). 품질이
// 아니라 유효한 블록이 목적이다: 기준(TexelCodec DecodeImageTexel — 공용 디코더 Texture
// TextureBlockDecode.h)이 만든 byte를 그대로 해석하므로 인코딩 오차는 대조와 무관하다.
// - BC1·BC3 색: 끝점 = 가장 먼 두 texel(565), 4색 모드(c0 > c1), 가장 가까운 index.
// - BC3 알파·BC4·BC5: 끝점 = 최대·최소(8값 모드), 가장 가까운 index.
// - BC7: mode 6 하나(1 subset, RGBA 7 bit + 끝점마다 p-bit, 4 bit index) — 끝점 = 가장 먼 두
//   texel(RGBA).
// - BC6H(Ufloat): mode 11 하나(1 subset, 10 bit 끝점, 변환 없음, 4 bit index) — 끝점 = 가장 먼 두
//   texel(RGB), 음수는 0.
// 팔레트(가장 가까운 index 고르기)는 공용 디코더로 계산한다(끝점과 index 0…15를 담은 블록을 풀어
// 얻는다 — 해석 식을 여기 다시 두지 않는다). 에셋 빌드의 TextureTools 인코더는 쓰지 못한다:
// TextureTools는 에디터 데이터(ROCKS_WITH_EDITOR_DATA)가 있는 타깃에서만 빌드되고 Sandbox(Game
// 타깃)에는 없다(18.5절 7항 "TC2 뒤 쿠킹 형식").
//
// 사용 예:
//   const graphics::PixelFormat staging = BlockStagingFormat(graphics::PixelFormat::BC7Unorm);
//   // staging(RGBA8Unorm) texel 16개를 rgba에 쓴 뒤
//   uint8 block[16];
//   EncodeBlockImage(graphics::PixelFormat::BC7Unorm, rgba, 4, 4, 1, Span<uint8>(block, 16));
namespace sandbox {

using namespace rocks;

// 다루는 블록 압축 형식인가(BC1·BC3(sRGB 짝 포함)·BC4·BC5·BC6HUfloat·BC7(sRGB 짝 포함)).
bool IsBlockCodecFormat(graphics::PixelFormat format) noexcept;

// 블록 형식의 부호화 입력 형식이다: BC6HUfloat는 RGBA16Float, sRGB 짝은 RGBA8UnormSrgb(입력 byte에
// 감마가 들어 있다), 그 밖은 RGBA8Unorm. 블록 형식이 아니면 format 그대로다.
graphics::PixelFormat BlockStagingFormat(graphics::PixelFormat format) noexcept;

// rgba(BlockStagingFormat 형식 texel — plane 다음 행 다음 열, planes 장)를 블록으로 out에 쓴다.
// out은 PixelFormatImageBytes(format, width, height) × planes byte다. 블록보다 작은 mip도 블록
// 하나다(블록 밖 자리는 가장자리 texel을 복제해 채운다 — 장치는 읽지 않는다). 다루지 않는
// 형식·크기 불일치면 false다.
bool EncodeBlockImage(graphics::PixelFormat format, Span<const uint8> rgba, uint32 width,
                      uint32 height, uint32 planes, Span<uint8> out) noexcept;

} // namespace sandbox
