#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Base/Types.h>

// BC1·BC3·BC4·BC5 블록 인코더다(Docs/Design/AssetSystem 12.17절 TC1 — UE 5.8 TextureFormatDXT의
// NVTT 2.0.8 Quality_Production 경로를 같은 연산 순서로 옮겼다). 입력은 RGBA8이고 출력은 GPU 블록
// byte다. 같은 입력이면 모든 호스트·스레드 수에서 같은 byte이고, UE가 같은 입력으로 내는 byte와
// 같다(golden — Tools/NvttGolden). 할당하지 않는다.
//
// 사용 예:
//   auto bytes = TextureBlockOutputBytes(TextureBlockFormat::Bc1, 256, 256, 1);
//   if (!bytes) { /* 크기 오류 */ }
//   Array<uint8> out(allocator);
//   if (!out.TryResize(static_cast<usize>(bytes.Value()))) { /* OutOfMemory */ }
//   TextureBlockImage image;
//   image.rgba = pixels;  // 256 × 256 × 4 byte
//   image.sizeX = 256;
//   image.sizeY = 256;
//   if (auto done = EncodeTextureBlocks(TextureBlockFormat::Bc1, image, out.View()); !done) { ... }
namespace rocks {

// 블록 형식이다. sRGB 짝(BC1·BC3 UnormSrgb)은 같은 인코딩이다(감마는 입력 byte에 이미 들어 있다 —
// UE도 같다). 쿠킹이 PixelFormat을 이 값으로 바꾼다(12.17절 TC2).
enum class TextureBlockFormat : uint8 {
    // RGB 4색(3색 모드는 쓰지만 투명 index 3은 쓰지 않는다), 알파 없음 — 8 byte.
    Bc1,
    // 알파 8값 + RGB 4색 — 16 byte(알파 블록 다음 색 블록).
    Bc3,
    // R 한 채널 8값 — 8 byte.
    Bc4,
    // R·G 두 채널 8값 — 16 byte(R 블록 다음 G 블록).
    Bc5,
};

// 블록 한 변의 texel 수다.
inline constexpr uint32 TextureBlockSize = 4;
// 블록 하나의 texel 수다.
inline constexpr uint32 TextureBlockTexels = TextureBlockSize * TextureBlockSize;
// RGBA8 texel 하나의 byte 수다.
inline constexpr uint32 TextureBlockTexelBytes = 4;
// 블록 하나의 RGBA8 입력·디코드 byte 수다.
inline constexpr uint32 TextureBlockRgbaBytes = TextureBlockTexels * TextureBlockTexelBytes;

// 이 인코더의 버전이다(12.17절 TD10 — 출력 byte가 바뀌면 올린다, 너겟 키 연결은 TC2).
inline constexpr uint32 RocksBcEncoderVersion = 1;

// 형식의 블록 byte 수다(Bc1·Bc4 8, Bc3·Bc5 16).
ROCKS_TEXTURETOOLS_API uint32 TextureBlockBytes(TextureBlockFormat format) noexcept;

// sizeX × sizeY × numSlices 그림의 출력 byte 수다. 블록 수는 slice마다 ceil(sizeX / 4) ×
// ceil(sizeY / 4)다(블록보다 작은 mip도 블록 하나 — 12.17절 TD4). 크기 0은 InvalidArgument, 곱이
// uint64를 넘으면 CapacityExceeded다.
ROCKS_TEXTURETOOLS_API Result<uint64> TextureBlockOutputBytes(TextureBlockFormat format,
                                                              uint32 sizeX, uint32 sizeY,
                                                              uint32 numSlices) noexcept;

// 인코딩할 RGBA8 그림이다(R, G, B, A byte 순서). rowPitch 0은 sizeX × 4, slicePitch 0은 rowPitch ×
// sizeY다. 그림 byte는 호출이 끝날 때까지 살아 있어야 한다(빌린다).
struct TextureBlockImage {
    Span<const uint8> rgba{};
    uint32 sizeX = 0;
    uint32 sizeY = 0;
    uint32 numSlices = 1;
    usize rowPitch = 0;
    usize slicePitch = 0;
};

// image를 format 블록으로 out에 쓴다. 출력은 slice 순, 블록 행 순, 블록 열 순이다. 블록보다 작은
// 가장자리 블록은 NVTT ColorBlock 주기 반복으로 채운다(폭 1은 0,0,0,0, 2는 0,1,0,1, 3은 0,1,2,0 —
// 12.17절 TD4).
//
// 오류(InvalidArgument): 크기 0, rowPitch가 sizeX × 4보다 작음, slicePitch가 slice가 닿는 byte
// (rowPitch × (sizeY − 1) + sizeX × 4)보다 작음, 입력 byte가 마지막 texel까지 닿지 않음, out 크기가
// TextureBlockOutputBytes와 다름. 출력 크기나 pitch로 계산한 입력 범위가 uint64를 넘으면
// CapacityExceeded다. 실패하면 out 내용은 정하지 않는다.
ROCKS_TEXTURETOOLS_API Result<void> EncodeTextureBlocks(TextureBlockFormat format,
                                                        const TextureBlockImage& image,
                                                        Span<uint8> out) noexcept;

// 4 × 4 RGBA8 texel 한 블록(행 우선 — texel (x, y)는 (y × 4 + x) × 4 byte)을 format 블록으로 out에
// 쓴다. out은 TextureBlockBytes(format) byte 이상이어야 한다. 가장자리 채움은 하지 않는다(받은 16
// texel을 그대로 쓴다).
ROCKS_TEXTURETOOLS_API void EncodeTextureBlock(TextureBlockFormat format,
                                               const uint8 (&texels)[TextureBlockRgbaBytes],
                                               uint8* out) noexcept;

// format 블록 하나를 4 × 4 RGBA8로 푼다(12.17절 TD7 기준 디코더 — NVTT BlockDXT 정수식). Bc1·Bc3은
// RGBA(Bc1 3색 모드의 index 3은 투명 검정), Bc4는 (R, 0, 0, 255), Bc5는 (R, G, 0, 255)다. 시험과
// 품질 측정이 쓴다. block은 TextureBlockBytes(format) byte 이상이어야 한다.
ROCKS_TEXTURETOOLS_API void DecodeTextureBlock(TextureBlockFormat format, const uint8* block,
                                               uint8 (&texels)[TextureBlockRgbaBytes]) noexcept;

} // namespace rocks
