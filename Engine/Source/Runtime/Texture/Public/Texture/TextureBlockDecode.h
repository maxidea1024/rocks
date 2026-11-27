#pragma once

#include <Core/Result.h>
#include <Base/Types.h>

// GPU 블록 형식의 CPU 디코더다(Docs/Design/AssetSystem 12.17.7). 쿠킹 플랫폼
// 데이터(BC1·BC3·BC4·BC5·BC6H· BC7)를 RGBA8 또는 RGBA16F half bit로 푼다. 기준 디코더다: BC1–BC5는
// NVTT BlockDXT 정수식(12.17절 TD7 — 인코더 golden과 같은 식), BC7·BC6H는 D3D11 형식의
// 정수식(Microsoft DirectXTex D3DX_BC7·D3DX_BC6H::Decode와 같은 절차 — 12.17.6 9)이다. Core에만
// 기대고(에디터 데이터 없음) 할당하지 않는다. 인코더는 TextureTools TextureBlockEncode.h다.
//
// 사용 예:
//   uint8 texels[TextureBlockRgbaBytes];
//   DecodeTextureBlock(TextureBlockFormat::Bc1, mipBytes + blockIndex *
//   TextureBlockBytes(TextureBlockFormat::Bc1),
//                      texels);  // texel (x, y)는 (y × 4 + x) × 4 byte
namespace rocks {

// 블록 형식이다. sRGB 짝(BC1·BC3·BC7 UnormSrgb)은 같은 인코딩이다(감마는 texel byte에 이미 들어
// 있다).
enum class TextureBlockFormat : uint8 {
    // RGB 4색 또는 3색 + 투명 검정 — 8 byte.
    Bc1,
    // 알파 8값·6값 + RGB 4색 — 16 byte(알파 블록 다음 색 블록).
    Bc3,
    // R 한 채널 8값·6값 — 8 byte.
    Bc4,
    // R·G 두 채널 — 16 byte(R 블록 다음 G 블록).
    Bc5,
    // RGB 부호 없는 half(Ufloat) — 16 byte. 알파는 없다.
    Bc6h,
    // RGBA 8비트 — 16 byte.
    Bc7,
};

// 블록 한 변의 texel 수다.
inline constexpr uint32 TextureBlockSize = 4;
// 블록 하나의 texel 수다.
inline constexpr uint32 TextureBlockTexels = TextureBlockSize * TextureBlockSize;
// RGBA8 texel 하나의 byte 수다.
inline constexpr uint32 TextureBlockTexelBytes = 4;
// 블록 하나의 RGBA8 입력·디코드 byte 수다.
inline constexpr uint32 TextureBlockRgbaBytes = TextureBlockTexels * TextureBlockTexelBytes;
// RGBA16F texel 하나의 byte 수다(half 넷, little-endian).
inline constexpr uint32 TextureBlockHalfTexelBytes = 8;
// BC3 알파·BC4·BC5 채널 블록의 팔레트 값 수다.
inline constexpr uint32 TextureBlockAlphaPaletteSize = 8;

// 형식의 블록 byte 수다(Bc1·Bc4 8, Bc3·Bc5·Bc6h·Bc7 16).
ROCKS_TEXTURE_API uint32 TextureBlockBytes(TextureBlockFormat format) noexcept;

// sizeX × sizeY × numSlices 그림의 블록 byte 수다. 블록 수는 slice마다 ceil(sizeX / 4) ×
// ceil(sizeY / 4)다(블록보다 작은 mip도 블록 하나 — 12.17절 TD4). 크기 0은 InvalidArgument, 곱이
// uint64를 넘으면 CapacityExceeded다.
ROCKS_TEXTURE_API Result<uint64> TextureBlockOutputBytes(TextureBlockFormat format, uint32 sizeX,
                                                         uint32 sizeY, uint32 numSlices) noexcept;

// BC3 알파·BC4·BC5 채널 블록의 팔레트다(NVTT AlphaBlockDXT5::evaluatePalette — 바닥 나눗셈). alpha0
// > alpha1이면 8값((7 − k)·a0 + k·a1) / 7, 아니면 6값((5 − k)·a0 + k·a1) / 5와 0·255다. 인코더의
// index 선택(TextureTools)도 이 함수를 쓴다.
ROCKS_TEXTURE_API void
TextureBlockAlphaPalette(uint32 alpha0, uint32 alpha1,
                         uint8 (&palette)[TextureBlockAlphaPaletteSize]) noexcept;

// format 블록 하나를 4 × 4 RGBA8로 푼다. Bc1·Bc3·Bc7은 RGBA(Bc1 3색 모드의 index 3은 투명 검정, Bc3
// 색 블록은 끝점 순서와 무관하게 늘 4색 — D3D 명세, Bc7 예약 모드는 투명 검정), Bc4는 (R, 0, 0,
// 255), Bc5는 (R, G, 0, 255)다. Bc6h는 DecodeTextureBlockHalf를 쓴다 (여기서는 0으로 채운다).
// block은 TextureBlockBytes(format) byte 이상이어야 한다.
ROCKS_TEXTURE_API void DecodeTextureBlock(TextureBlockFormat format, const uint8* block,
                                          uint8 (&texels)[TextureBlockRgbaBytes]) noexcept;

// BC6H Ufloat 블록 하나를 4 × 4 RGBA16F half bit로 푼다. texel (x, y)의 채널 c는
// halfs[(y × 4 + x) × 4 + c]이고 알파는 1.0(0x3C00)이다. 예약·무효 모드는 불투명 검정이다. block은
// 16 byte 이상이어야 한다.
ROCKS_TEXTURE_API void DecodeTextureBlockHalf(const uint8* block,
                                              uint16 (&halfs)[TextureBlockTexels * 4]) noexcept;

} // namespace rocks
