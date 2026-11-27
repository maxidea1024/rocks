#pragma once

#include <Core/Result.h>
#include <Base/Types.h>

// GPU 블록 형식의 CPU 디코더다(Docs/Design/AssetSystem 12.17.7). 쿠킹 플랫폼
// 데이터(BC1·BC3·BC4·BC5·BC6H·BC7·EAC R11)를 RGBA8 또는 RGBA16F half bit로 푼다. 기준 디코더다:
// BC1–BC5는 NVTT BlockDXT 정수식(12.17절 TD7 — 인코더 golden과 같은 식), BC7·BC6H는 D3D11 형식의
// 정수식(Microsoft DirectXTex D3DX_BC7·D3DX_BC6H::Decode와 같은 절차 — 12.17.6 9), EAC R11은
// OpenGL ES 3.0 부록 C.1.5 식이다(12.17.8 4). ASTC(12.17.8)는 형식·크기만 두고 풀지 않는다(ASTC
// 디코드는 TextureTools DecodeAstcBlocks — astcenc가 필요하다). Core에만 기대고(에디터 데이터 없음)
// 할당하지 않는다. 인코더는 TextureTools TextureBlockEncode.h다.
//
// 사용 예:
//   uint8 texels[TextureBlockRgbaBytes];
//   DecodeTextureBlock(TextureBlockFormat::Bc1, mipBytes + blockIndex *
//   TextureBlockBytes(TextureBlockFormat::Bc1),
//                      texels);  // texel (x, y)는 (y × 4 + x) × 4 byte
namespace rocks {

// 블록 형식이다. sRGB 짝(BC1·BC3·BC7·ASTC UnormSrgb)은 같은 인코딩이다(감마는 texel byte에 이미
// 들어 있다 — ASTC는 인코더 profile만 다르다).
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
    // R 한 채널 11 bit(EAC R11 unorm) — 8 byte(big-endian 64 bit: base·multiplier·표·index 16개).
    EacR11,
    // ASTC LDR 2D 블록 — 블록마다 16 byte, 블록 크기는 이름의 texel 수다.
    Astc4x4,
    Astc6x6,
    Astc8x8,
    Astc10x10,
    Astc12x12,
};

// BC·EAC 블록 한 변의 texel 수다(ASTC는 TextureBlockWidth·TextureBlockHeight).
inline constexpr uint32 TextureBlockSize = 4;
// BC·EAC 블록 하나의 texel 수다.
inline constexpr uint32 TextureBlockTexels = TextureBlockSize * TextureBlockSize;
// RGBA8 texel 하나의 byte 수다.
inline constexpr uint32 TextureBlockTexelBytes = 4;
// 블록 하나의 RGBA8 입력·디코드 byte 수다.
inline constexpr uint32 TextureBlockRgbaBytes = TextureBlockTexels * TextureBlockTexelBytes;
// RGBA16F texel 하나의 byte 수다(half 넷, little-endian).
inline constexpr uint32 TextureBlockHalfTexelBytes = 8;
// BC3 알파·BC4·BC5 채널 블록의 팔레트 값 수다.
inline constexpr uint32 TextureBlockAlphaPaletteSize = 8;

// EAC 수정값 표의 수와 표 하나의 값 수다(OpenGL ES 3.0 Table C.10).
inline constexpr uint32 TextureEacTableCount = 16;
inline constexpr uint32 TextureEacTableSize = 8;
// EAC R11 값의 최댓값(11 bit unorm)이다.
inline constexpr uint32 TextureEacR11Max = 2047;

// EAC 수정값 표다(ETC2 알파 표 — OpenGL ES 3.0 Table C.10, 행 = 표 번호, 열 = index).
inline constexpr int8 TextureEacModifiers[TextureEacTableCount][TextureEacTableSize] = {
    {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11},  {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10},  {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},  {-2, -4, -8, -10, 1, 3, 7, 9},   {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},   {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8},
};

// EAC R11 unorm 값 하나다(OpenGL ES 3.0 식 C.4·C.5): multiplier ≥ 1이면 clamp(base·8 + 4 +
// modifier·multiplier·8, 0, 2047), multiplier 0이면 clamp(base·8 + 4 + modifier, 0, 2047)다. 인코더
// 탐색(TextureTools)도 이 함수를 쓴다.
//
// 예: TextureEacR11Value(103, 2, TextureEacModifiers[13][3]) == 668(명세 예).
inline constexpr uint32 TextureEacR11Value(uint32 base, uint32 multiplier,
                                           int32 modifier) noexcept {
    const int32 scale = multiplier == 0 ? 1 : static_cast<int32>(multiplier) * 8;
    const int32 value = static_cast<int32>(base) * 8 + 4 + modifier * scale;
    return value < 0                                      ? 0u
           : value > static_cast<int32>(TextureEacR11Max) ? TextureEacR11Max
                                                          : static_cast<uint32>(value);
}

// 형식이 ASTC(블록 크기 다섯 가지)인지다.
inline constexpr bool TextureBlockIsAstc(TextureBlockFormat format) noexcept {
    return format == TextureBlockFormat::Astc4x4 || format == TextureBlockFormat::Astc6x6 ||
           format == TextureBlockFormat::Astc8x8 || format == TextureBlockFormat::Astc10x10 ||
           format == TextureBlockFormat::Astc12x12;
}

// 형식의 블록 byte 수다(Bc1·Bc4·EacR11 8, Bc3·Bc5·Bc6h·Bc7·ASTC 16).
ROCKS_TEXTURE_API uint32 TextureBlockBytes(TextureBlockFormat format) noexcept;

// 형식의 블록 가로·세로 texel 수다(BC·EAC 4, ASTC는 이름의 크기 — 예 Astc6x6은 6).
ROCKS_TEXTURE_API uint32 TextureBlockWidth(TextureBlockFormat format) noexcept;
ROCKS_TEXTURE_API uint32 TextureBlockHeight(TextureBlockFormat format) noexcept;

// sizeX × sizeY × numSlices 그림의 블록 byte 수다. 블록 수는 slice마다 ceil(sizeX / 블록 가로) ×
// ceil(sizeY / 블록 세로)다(블록보다 작은 mip도 블록 하나 — 12.17절 TD4). 크기 0은 InvalidArgument,
// 곱이 uint64를 넘으면 CapacityExceeded다.
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
// 255), Bc5는 (R, G, 0, 255), EacR11은 (round(x · 255 / 2047), 0, 0, 255)다. Bc6h는
// DecodeTextureBlockHalf를 쓰고, ASTC는 4 × 4 블록이 아니어서 받지 않는다(둘 다 0으로 채운다).
// block은 TextureBlockBytes(format) byte 이상이어야 한다.
ROCKS_TEXTURE_API void DecodeTextureBlock(TextureBlockFormat format, const uint8* block,
                                          uint8 (&texels)[TextureBlockRgbaBytes]) noexcept;

// BC6H Ufloat 블록 하나를 4 × 4 RGBA16F half bit로 푼다. texel (x, y)의 채널 c는
// halfs[(y × 4 + x) × 4 + c]이고 알파는 1.0(0x3C00)이다. 예약·무효 모드는 불투명 검정이다. block은
// 16 byte 이상이어야 한다.
ROCKS_TEXTURE_API void DecodeTextureBlockHalf(const uint8* block,
                                              uint16 (&halfs)[TextureBlockTexels * 4]) noexcept;

// EAC R11 블록 하나(8 byte)를 11 bit 값 16개로 푼다. values[y × 4 + x]는 texel (x, y)다(블록 안
// index는 열 우선이지만 출력은 다른 디코더처럼 행 우선이다). 16 bit 확장은 (x << 5) | (x >> 6)이다.
ROCKS_TEXTURE_API void DecodeEacR11Block(const uint8* block,
                                         uint16 (&values)[TextureBlockTexels]) noexcept;

} // namespace rocks
