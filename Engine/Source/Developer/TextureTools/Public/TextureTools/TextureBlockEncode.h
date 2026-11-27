#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Texture/TextureBlockDecode.h>
#include <Base/Types.h>

// GPU 블록 인코더다. BC1·BC3·BC4·BC5는 UE 5.8 TextureFormatDXT의 NVTT 2.0.8 Quality_Production
// 경로를 같은 연산 순서로 옮겼다(Docs/Design/AssetSystem 12.17절 TC1 — UE가 같은 입력으로 내는
// byte와 같다, golden Tools/NvttGolden). BC6H·BC7은 UE TextureFormatIntelISPCTexComp와 같은 ISPC
// Texture Compressor 커널과 프로필이다(12.17.6 — 고정 ISPC·아키텍처당 타깃 하나로 빌드해 호스트
// CPU와 무관한 byte, UE와는 품질로 대조). 입력은 RGBA8(BC6H는 RGBA16F)이고 출력은 GPU 블록 byte다.
// 같은 입력이면 같은 아키텍처의 모든 호스트·스레드 수에서 같은 byte다. 할당하지 않는다. 블록
// 형식·크기 함수와 기준 디코더는 런타임 Texture 모듈(Texture/TextureBlockDecode.h — 12.17.7)에
// 있다.
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

// BC1–BC5 인코더의 버전이다(12.17절 TD10 — 출력 byte가 바뀌면 올린다, 너겟 키에 들어간다).
inline constexpr uint32 RocksBcEncoderVersion = 1;
// BC6H·BC7 인코더(ISPC Texture Compressor 감싸기)의 버전이다(12.17.6 5). 입력 준비·프로필이
// 바뀌거나 커널 원본(ThirdParty IspcTextureCompressor의 kernel.ispc·ispc_texcomp.cpp — upstream
// 갱신·Rocks.patch)이 바뀌면 올린다(커널 원본 내용은 키에 들어가지 않는다). ISPC 판·인자는
// IspcBcEncoderIdentity가 따로 너겟 키에 넣는다.
inline constexpr uint32 IspcBcEncoderVersion = 1;

// 형식의 입력 texel byte 수다(Bc6h 8 — RGBA16F, 그 밖 4 — RGBA8).
ROCKS_TEXTURETOOLS_API uint32 TextureBlockSourceTexelBytes(TextureBlockFormat format) noexcept;

// BC6H·BC7 커널을 빌드한 ISPC의 신원이다(버전·타깃·고정 인자 — 예 "ispc 1.32.0 -O2
// --target=sse4-i32x4 ..."; BuildTargets 3.12절 BD50). 쿠킹이 너겟 키에 넣는다. BC6H·BC7 인코더가
// 없는 대상(Android)은 빈 문자열이다.
ROCKS_TEXTURETOOLS_API StringView IspcBcEncoderIdentity() noexcept;

// 인코딩할 그림이다. rgba는 RGBA8(R, G, B, A byte 순서)이고, Bc6h는 RGBA16F(half 넷, little-endian
// — TextureBlockSourceTexelBytes)다. rowPitch 0은 sizeX × texel byte, slicePitch 0은 rowPitch ×
// sizeY다. 그림 byte는 호출이 끝날 때까지 살아 있어야 한다(빌린다).
struct TextureBlockImage {
    Span<const uint8> rgba{};
    uint32 sizeX = 0;
    uint32 sizeY = 0;
    uint32 numSlices = 1;
    usize rowPitch = 0;
    usize slicePitch = 0;
};

// BC6H·BC7 인코딩 선택이다(BC1–BC5는 보지 않는다).
struct TextureBlockOptions {
    // BC7이 알파 프로필(UE GetProfile_alpha_basic)을 쓰는지다. 거짓이면 RGB 프로필(GetProfile_basic
    // — 알파를 무시하고 255로 쓴다)이다. 쿠킹은 12.17절 TD5 알파 판정 결과를 넘긴다.
    bool alphaProfile = false;
};

// image를 format 블록으로 out에 쓴다. 출력은 slice 순, 블록 행 순, 블록 열 순이다. 블록보다 작은
// 가장자리는 BC1–BC5면 NVTT ColorBlock 주기 반복으로(폭 1은 0,0,0,0, 2는 0,1,0,1, 3은 0,1,2,0 —
// 12.17절 TD4), BC6H·BC7이면 마지막 열·행 복제로 채운다(UE PadImageToBlockSize — 12.17.6 4).
// BC6H·BC7은 결정적 부동소수 환경(platform::RunWithDeterministicFloatingPoint)에서 압축하므로
// 호출자의 FTZ·DAZ 설정과 무관하다.
//
// 오류(InvalidArgument): 크기 0, rowPitch가 sizeX × texel byte보다 작음, slicePitch가 slice가 닿는
// byte(rowPitch × (sizeY − 1) + sizeX × texel byte)보다 작음, 입력 byte가 마지막 texel까지 닿지
// 않음, out 크기가 TextureBlockOutputBytes와 다름. 출력 크기나 pitch로 계산한 입력 범위가 uint64를
// 넘으면 CapacityExceeded다. BC6H·BC7 인코더가 없는 대상(Android)이나 x64에서 SSE4.2가 없는 CPU는
// Unsupported다. 실패하면 out 내용은 정하지 않는다.
ROCKS_TEXTURETOOLS_API Result<void> EncodeTextureBlocks(TextureBlockFormat format,
                                                        const TextureBlockImage& image,
                                                        const TextureBlockOptions& options,
                                                        Span<uint8> out) noexcept;

// 기본 선택(BC7 RGB 프로필)으로 EncodeTextureBlocks를 부른다.
ROCKS_TEXTURETOOLS_API Result<void> EncodeTextureBlocks(TextureBlockFormat format,
                                                        const TextureBlockImage& image,
                                                        Span<uint8> out) noexcept;

// 4 × 4 RGBA8 texel 한 블록(행 우선 — texel (x, y)는 (y × 4 + x) × 4 byte)을 BC1–BC5 블록으로 out에
// 쓴다. out은 TextureBlockBytes(format) byte 이상이어야 한다. 가장자리 채움은 하지 않는다(받은 16
// texel을 그대로 쓴다). Bc6h·Bc7은 받지 않는다(out을 0으로 채운다 — 4 × 4 그림으로
// EncodeTextureBlocks를 부른다, 12.17.6 9).
ROCKS_TEXTURETOOLS_API void EncodeTextureBlock(TextureBlockFormat format,
                                               const uint8 (&texels)[TextureBlockRgbaBytes],
                                               uint8* out) noexcept;

} // namespace rocks
