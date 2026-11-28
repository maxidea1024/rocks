#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Math/Color.h>
#include <Core/Result.h>
#include <GraphicsTypes/PixelFormat.h>
#include <Texture/Texture.h>
#include <Texture/TextureBuildSettings.h>
#include <TextureTools/TextureFormatSelect.h>

// 텍스처 빌드다(Docs/Design/AssetSystem 12.13절 — A5i, 규칙은 UE 5.8에 맞춘 12.5절·12.2절 1–5번,
// 출력 형식은 12.17.5절): 원본 정규 픽셀을 선형화 → flipGreen → PowerOfTwoMode → 최대 크기 → mip
// 사슬 → NormalizeNormals → 채널 복제 → 알파 판정 → 형식 선택 → 쿠킹 편향 → 출력 부호화(BC 압축
// 포함)해 쿠킹 mip으로 만든다. 순수
// 함수다(패키지·Registry·프로파일을 모른다 — 쿠킹 A5j가 입력을 만들고 결과를 TexturePlatformData로
// 옮긴다). 같은 입력이면 모든 호스트에서 같은 byte다(DeterministicMath만 쓴다).
//
// 사용 예(쿠킹 처리기):
//   TextureBuildSettings settings;
//   TextureBuildResult built(allocator);
//   if (auto resolved = ResolveTextureBuildSettings(texture, dimension, group, settings, built);
//       !resolved) { /* built.failure로 진단 */ }
//   if (auto done = BuildTexture(allocator, settings, sourcePixels, built); !done) { ... }
namespace rocks {

// 빌드가 쓰는 LOD 그룹 값이다(12.4.5절 구현 행의 부분집합 — 기본값은 UE FTextureLODGroup 멤버
// 기본값, TextureLODSettings.h). 프로파일 그룹 표·필터·aniso·로드 옵션은 쿠킹(A5j)이 다룬다.
struct TextureLodGroupValues {
    int32 minLodSize = 1;
    int32 maxLodSize = 4096;
    int32 lodBias = 0;
    TextureMipGenSettings mipGenSettings = TextureMipGenSettings::SimpleAverage;
};

// 작업 단계 하나의 작업 픽셀 byte 상한이다(16384² RGBA32F — 넘으면 InvalidSource +
// CapacityExceeded, 12.13절 규모).
inline constexpr uint64 TextureBuildMaxWorkingBytes = uint64{4} << 30;

// 쿠킹 mip 하나다(slice·면을 차례로 이은 형식 texel — 행 사이 패딩 없음, 위에서 아래).
struct TextureBuiltMip {
    explicit TextureBuiltMip(Allocator& allocator) noexcept : bytes(allocator) {
    }

    uint32 sizeX = 0;
    uint32 sizeY = 0;
    // Volume 깊이, 그 밖 1이다.
    uint32 sizeZ = 1;
    Array<uint8> bytes;
};

// 빌드 경고 하나다(SrgbIgnored·PreserveBorderWithWrap·EncoderUnavailable·PlatformFormatFallback —
// 각 한 번, 형식 경고의 detail은 대체 전 PixelFormat 번호).
struct TextureBuildWarning {
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    uint64 detail = 0;
};

// 빌드 실패다(code·error·detail — 반환 오류와 같은 error).
struct TextureBuildFailure {
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    ErrorCode error = ErrorCode::None;
    uint64 detail = 0;
};

// 빌드 결과다.
struct TextureBuildResult {
    explicit TextureBuildResult(Allocator& allocator) noexcept
        : mips(allocator), warnings(allocator) {
    }

    graphics::PixelFormat pixelFormat = graphics::PixelFormat::Unknown;
    bool srgb = false;
    TextureSwizzle swizzle[4] = {TextureSwizzle::R, TextureSwizzle::G, TextureSwizzle::B,
                                 TextureSwizzle::A};
    // Volume 1, 그 밖 원본 slice 수다.
    uint32 numSlices = 1;
    // 원본 알파 판정 결과다(12.17.5 3 — AutoDXT 형식과 BC7 프로필이 쓴다, TextureFormatNeedsAlpha가
    // 거짓이면 판정하지 않고 false). 쿠킹 적중 검증은 AutoDXT만 머리 형식으로 이 값을 되살린다.
    bool alphaTransparent = false;
    // 상위 mip부터다.
    Array<TextureBuiltMip> mips;
    Array<TextureBuildWarning> warnings;
    TextureBuildFailure failure;
};

// 빌드 규칙 버전이다(12.5·12.13·12.17절 규칙이 바뀌면 올린다 — 쿠킹 너겟 키에 들어간다, 12.14절).
// 3: 압축 형식 선택·채널 복제·BC 인코딩(12.17.5 — TC2).
// 4: 빌드 설정 compressionQuality(ASTC 블록 크기 — 12.17.8 7).
// 5: mip 필터 Sharpen·Blur·alpha coverage·useNewMipFilter 두 경로, 테두리 평균 역수 곱, Volume 깊이
//    1 평균, 키의 주소 칸(12.18 TP1).
inline constexpr uint32 TextureBuildVersion = 5;

// 빌드 경고 수
// 상한이다(SrgbIgnored·PreserveBorderWithWrap·EncoderUnavailable·PlatformFormatFallback).
inline constexpr usize TextureBuildMaxWarnings = 4;

// 출력 형식이다(12.17.5 형식 선택 — BuildTexture 결과와 같다).
struct TextureBuildOutputInfo {
    graphics::PixelFormat pixelFormat = graphics::PixelFormat::Unknown;
    bool srgb = false;
    TextureSwizzle swizzle[4] = {TextureSwizzle::R, TextureSwizzle::G, TextureSwizzle::B,
                                 TextureSwizzle::A};
    // 인코딩(인코더 종류 — 쿠킹 너겟 키의 인코더 버전·신원을 고른다, 12.17.8 8)이다.
    TextureFormatEncoding encoding = TextureFormatEncoding::Uncompressed;
};

// settings와 알파 판정 결과의 빌드 경고를 out에 쓰고 수를 돌려준다(설정·알파만으로 정한다 — 너겟
// 적중은 머리의 형식이 맞는 알파로 다시 계산한다, 12.17.5 4). 순서: SrgbIgnored,
// PreserveBorderWithWrap, EncoderUnavailable, PlatformFormatFallback.
ROCKS_TEXTURETOOLS_API usize
TextureBuildWarnings(const TextureBuildSettings& settings, bool alphaTransparent,
                     TextureBuildWarning (&out)[TextureBuildMaxWarnings]) noexcept;

// settings와 알파 판정 결과의 출력 형식·sRGB·swizzle이다(BuildTexture가 내는 값 — 쿠킹 너겟 적중
// 검증이 두 알파 값으로 부른다). 형식을 고를 수 없는 설정이면 pixelFormat이 Unknown이다.
ROCKS_TEXTURETOOLS_API TextureBuildOutputInfo
TextureBuildOutput(const TextureBuildSettings& settings, bool alphaTransparent) noexcept;

// settings로 빌드했을 때의 쿠킹 상위 mip 크기다(PowerOfTwoMode·Resize·최대 크기·쿠킹 편향 뒤 —
// UE GetBuiltTextureSize, 형식 선택의 4의 배수 판정이 쓴다). 오류: 원본 형식 Invalid·크기·slice 0
// InvalidArgument.
ROCKS_TEXTURETOOLS_API Result<void> TextureBuildTopMipSize(const TextureBuildSettings& settings,
                                                           uint32& sizeX, uint32& sizeY) noexcept;

// sRGB를 막는 CompressionSettings인지다(UE bPreventSRGB — Alpha·Normalmap·Masks·HDR·HDR_F32·
// HDR_Compressed·HalfFloat·SingleFloat, 12.17.5 2).
ROCKS_TEXTURETOOLS_API bool
TextureCompressionIgnoresSrgb(TextureCompressionSettings compression) noexcept;

// texture(출력 클래스 차원 dimension)와 그룹 값에서 유효 설정을 만든다(12.13절 해석 규칙). 계약
// 설정 판정(CheckTextureSettings)은 호출자가 먼저 한다 — 여기서는 그룹 mipGen 계약만 본다. 실패하면
// result.failure에 진단을 둔다. 오류: slice 수 규칙 InvalidArgument(InvalidSource), 비 2D
// Resize·숨김 CompressionSettings InvalidArgument(InvalidSetting), 그룹 계약 mipGen
// Unsupported(UnsupportedSetting).
ROCKS_TEXTURETOOLS_API Result<void>
ResolveTextureBuildSettings(const Texture& texture, TextureDimension dimension,
                            const TextureLodGroupValues& group, TextureBuildSettings& out,
                            TextureBuildResult& result) noexcept;

// 빌드 실행 방법이다(12.17.9 4 — 설정이 아니라 결과가 같으므로 너겟 키에 넣지 않는다). scheduler가
// 있으면 블록 압축을 나눠 실행하고 scratch(동시 Allocate·Deallocate 안전 — SystemAllocator)가
// 필요하다. 둘 다 호출 동안만 쓴다.
struct TextureBuildExecution {
    WorkerScheduler* scheduler = nullptr;
    Allocator* scratch = nullptr;
};

// settings와 원본 정규 픽셀(TextureSource bulk raw byte)로 쿠킹 mip을 만든다(out — 앞 결과는
// 지운다). 결과 byte는 execution과 무관하다. 오류: 픽셀 byte 수·Cube 면 비정사각
// InvalidArgument(InvalidSource), Resize가 Cube 정사각을 깸·숨김 CompressionSettings
// InvalidArgument(InvalidSetting), scheduler가 있는데 scratch 없음 InvalidArgument(진단 None), 할당
// 실패 OutOfMemory(진단 None).
ROCKS_TEXTURETOOLS_API Result<void>
BuildTexture(Allocator& allocator, const TextureBuildSettings& settings,
             Span<const uint8> sourcePixels, TextureBuildResult& out,
             const TextureBuildExecution& execution = {}) noexcept;

} // namespace rocks
