#pragma once

#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Texture/TextureBlockDecode.h>
#include <Base/Types.h>

// GPU 블록 인코더다. BC1·BC3·BC4·BC5는 UE 5.8 TextureFormatDXT의 NVTT 2.0.8 Quality_Production
// 경로를 같은 연산 순서로 옮겼다(Docs/Design/AssetSystem 12.17절 TC1 — UE가 같은 입력으로 내는
// byte와 같다, golden Tools/NvttGolden). BC6H·BC7은 UE TextureFormatIntelISPCTexComp와 같은 ISPC
// Texture Compressor 커널과 프로필이다(12.17.6 — 고정 ISPC·아키텍처당 타깃 하나로 빌드해 호스트
// CPU와 무관한 byte, UE와는 품질로 대조). EAC R11은 자체 정확 최적 탐색(12.17.8 4), ASTC는 UE
// TextureFormatASTC와 같은 설정의 ARM astc-encoder 5.1.0 불변성 빌드다(12.17.8 2·3). 입력은
// RGBA8(BC6H는 RGBA16F, EAC R11은 R16)이고 출력은 GPU 블록 byte다. 같은 입력이면 같은 아키텍처의
// 모든 호스트·스레드 수에서 같은 byte다(병렬 압축 TextureBlockOptions::scheduler의 worker 수와도
// 무관 — 12.17.9). 직렬 BC·EAC는 할당하지 않는다. 병렬은 helper 작업을 scratch로 할당하고, ASTC는
// astcenc 컨텍스트(전역 new — 할당 실패는 종료)와 행 여백 입력의 사본·slice 포인터(scratch)를
// 할당한다. 블록 형식·크기 함수와 기준 디코더는 런타임 Texture 모듈(Texture/TextureBlockDecode.h —
// 12.17.7)에 있다.
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

class WorkerScheduler;

// BC1–BC5 인코더의 버전이다(12.17절 TD10 — 출력 byte가 바뀌면 올린다, 너겟 키에 들어간다).
inline constexpr uint32 RocksBcEncoderVersion = 1;
// BC6H·BC7 인코더(ISPC Texture Compressor 감싸기)의 버전이다(12.17.6 5). 입력 준비·프로필이
// 바뀌거나 커널 원본(ThirdParty IspcTextureCompressor의 kernel.ispc·ispc_texcomp.cpp — upstream
// 갱신·Rocks.patch)이 바뀌면 올린다(커널 원본 내용은 키에 들어가지 않는다). ISPC 판·인자는
// IspcBcEncoderIdentity가 따로 너겟 키에 넣는다.
// 2: Rocks.patch — compute_axis 주축 재시작(12.17.6 10).
inline constexpr uint32 IspcBcEncoderVersion = 2;
// EAC R11 인코더(RocksEac)의 버전이다(12.17.8 4 — 탐색·입력 변환이 바뀌면 올린다).
inline constexpr uint32 RocksEacEncoderVersion = 1;
// ASTC 감싸기(astcenc 설정·swizzle·Rocks.patch)의 버전이다(12.17.8 5). astcenc 판·preset은
// AstcEncoderIdentity가 따로 너겟 키에 넣는다.
inline constexpr uint32 AstcEncoderVersion = 1;

// 형식의 입력 texel byte 수다(Bc6h 8 — RGBA16F, EacR11 2 — R16 unorm, 그 밖 4 — RGBA8).
ROCKS_TEXTURETOOLS_API uint32 TextureBlockSourceTexelBytes(TextureBlockFormat format) noexcept;

// ASTC 인코더의 신원이다(판·preset·빌드·SIMD 경로 — 예 "astcenc 5.1.0 medium invariant rocks-patch
// 1 x64-sse2"). 쿠킹이 너겟 키에 넣는다. SIMD 경로는 x64·Arm64 byte 동일을 Mac에서 확인할 때까지
// 넣는다.
ROCKS_TEXTURETOOLS_API StringView AstcEncoderIdentity() noexcept;

// BC6H·BC7 커널을 빌드한 ISPC의 신원이다(버전·타깃·고정 인자 — 예 "ispc 1.32.0 -O2
// --target=sse4-i32x4 ..."; BuildTargets 3.12절 BD50). 쿠킹이 너겟 키에 넣는다. BC6H·BC7 인코더가
// 없는 대상(Android)은 빈 문자열이다.
ROCKS_TEXTURETOOLS_API StringView IspcBcEncoderIdentity() noexcept;

// 인코딩할 그림이다. rgba는 RGBA8(R, G, B, A byte 순서)이고, Bc6h는 RGBA16F(half 넷,
// little-endian), EacR11은 R16 unorm(little-endian)이다(TextureBlockSourceTexelBytes). rowPitch 0은
// sizeX × texel byte, slicePitch 0은 rowPitch × sizeY다. 그림 byte는 호출이 끝날 때까지 살아 있어야
// 한다(빌린다).
struct TextureBlockImage {
    Span<const uint8> rgba{};
    uint32 sizeX = 0;
    uint32 sizeY = 0;
    uint32 numSlices = 1;
    usize rowPitch = 0;
    usize slicePitch = 0;
};

// ASTC 입력 채널 배치다(12.17.8 3 — UE TextureFormatASTC swizzle).
enum class TextureAstcMode : uint8 {
    // RGB만 쓰고 알파는 1(UE ASTC_RGB, 그리고 알파 없는 RGBA·RGBA HQ).
    Rgb,
    // RGBA 그대로.
    Rgba,
    // 노멀 LA: R = G = B = X(입력 R), A = Y(입력 G), 노멀 오차 지표(ASTCENC_FLG_MAP_NORMAL).
    NormalLa,
};

// 인코딩 선택이다. alphaProfile은 BC7, astcMode·srgb는 ASTC만 보고, 실행 칸(scheduler·
// minBatchBlocks·scratch)은 모든 형식이 본다.
struct TextureBlockOptions {
    // BC7이 알파 프로필(UE GetProfile_alpha_basic)을 쓰는지다. 거짓이면 RGB 프로필(GetProfile_basic
    // — 알파를 무시하고 255로 쓴다)이다. 쿠킹은 12.17절 TD5 알파 판정 결과를 넘긴다.
    bool alphaProfile = false;
    // ASTC 채널 배치다.
    TextureAstcMode astcMode = TextureAstcMode::Rgba;
    // ASTC가 sRGB profile(ASTCENC_PRF_LDR_SRGB)인지다(…UnormSrgb 출력).
    bool srgb = false;
    // 블록을 나눠 압축할 scheduler다(12.17.9 2 — (slice, 블록 행) 단위, ASTC는 astcenc
    // thread_count). null이면 호출 스레드 혼자다. 같은 scheduler의 worker 안에서 부르거나 시작
    // 전·종료 뒤이면 호출 스레드 혼자 실행한다. 결과 byte는 scheduler 유무·worker 수와 무관하다.
    WorkerScheduler* scheduler = nullptr;
    // batch 하나의 최소 블록 수다(0이면 형식 기본 — BC1–BC5 2048, 그 밖 블록 행 하나). 결과와
    // 무관한 분배 조절값이다(시험은 작게 해 batch 경계를 본다).
    uint32 minBatchBlocks = 0;
    // 호출 동안 쓰는 할당용 allocator다: 병렬 helper 작업, ASTC 행 여백 입력의 빽빽한 사본과 slice
    // 포인터. 여러 스레드가 동시에 Allocate·Deallocate해도 안전해야 한다(SystemAllocator는 맞다).
    // scheduler가 있는데 없으면 InvalidArgument, 없으면 행 여백이 있는 ASTC 입력도
    // InvalidArgument다.
    Allocator* scratch = nullptr;
};

// image를 format 블록으로 out에 쓴다. 출력은 slice 순, 블록 행 순, 블록 열 순이다. 블록보다 작은
// 가장자리는 BC1–BC5·EAC R11이면 NVTT ColorBlock 주기 반복으로(폭 1은 0,0,0,0, 2는 0,1,0,1, 3은
// 0,1,2,0 — 12.17절 TD4, EAC는 채운 texel을 오차에 넣지 않는다), BC6H·BC7·ASTC면 마지막 열·행
// 복제로 채운다(UE PadImageToBlockSize·astcenc 좌표 clamp — 12.17.6 4·12.17.8 3). 모든 형식은
// 결정적 부동소수 환경(platform::RunWithDeterministicFloatingPoint — batch마다, 12.17.9 2)에서
// 압축하므로 호출자와 worker의 FTZ·DAZ 설정과 무관하다.
//
// 오류(InvalidArgument): 크기 0, rowPitch가 sizeX × texel byte보다 작음, slicePitch가 slice가 닿는
// byte(rowPitch × (sizeY − 1) + sizeX × texel byte)보다 작음, 입력 byte가 마지막 texel까지 닿지
// 않음, out 크기가 TextureBlockOutputBytes와 다름. 출력 크기나 pitch로 계산한 입력 범위가 uint64를
// 넘으면 CapacityExceeded다. BC6H·BC7 인코더가 없는 대상(Android)이나 x64에서 SSE4.2가 없는 CPU는
// Unsupported다. scheduler가 있는데 scratch가 없으면 InvalidArgument(그림 크기와 무관), ASTC는 행
// 여백이 있는데 scratch가 없으면 InvalidArgument, 사본 할당 실패는 OutOfMemory다. 실패하면 out
// 내용은 정하지 않는다.
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
// texel을 그대로 쓴다). Bc6h·Bc7·EacR11·ASTC는 받지 않는다(out을 TextureBlockBytes(format) byte
// 0으로 채운다 — BC6H·BC7·ASTC는 그림으로 EncodeTextureBlocks를, EAC R11은 EncodeEacR11Block을
// 부른다, 12.17.6 9·12.17.8 5).
ROCKS_TEXTURETOOLS_API void EncodeTextureBlock(TextureBlockFormat format,
                                               const uint8 (&texels)[TextureBlockRgbaBytes],
                                               uint8* out) noexcept;

// R16 unorm 16개(values[y × 4 + x] — 행 우선)를 EAC R11 블록 하나(8 byte)로 out에 쓴다. validMask의
// bit (y × 4 + x)가 켜진 texel만 오차에 넣는다(가장자리 채움 texel은 끈다 — 0이면 전부). 목표값은
// 가장 가까운 11 bit(v · 2047 / 65535 반올림)이고, (표, multiplier, base)는 오차 제곱 합이 가장
// 작은 것 가운데 사전순으로 가장 작은 것이다(12.17.8 4).
ROCKS_TEXTURETOOLS_API void EncodeEacR11Block(const uint16 (&values)[TextureBlockTexels],
                                              uint16 validMask, uint8* out) noexcept;

// ASTC 블록을 RGBA8로 푼다(astcenc decompress — 시험·품질 측정용, 12.17.8 5). blocks는
// TextureBlockOutputBytes(format, sizeX, sizeY, numSlices) byte, rgba는 sizeX × sizeY × 4 ×
// numSlices byte(slice 순, 행 사이 여백 없음)다. srgb는 인코딩 profile과 같아야 한다. 형식이 ASTC가
// 아니거나 크기가 맞지 않으면 InvalidArgument, astcenc 컨텍스트 할당 실패는 OutOfMemory다.
ROCKS_TEXTURETOOLS_API Result<void> DecodeAstcBlocks(TextureBlockFormat format,
                                                     Span<const uint8> blocks, uint32 sizeX,
                                                     uint32 sizeY, uint32 numSlices, bool srgb,
                                                     Span<uint8> rgba) noexcept;

} // namespace rocks
