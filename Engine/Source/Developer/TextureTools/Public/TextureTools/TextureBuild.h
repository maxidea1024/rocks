#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Math/Color.h>
#include <Core/Result.h>
#include <GraphicsTypes/PixelFormat.h>
#include <Texture/Texture.h>

// 텍스처 빌드다(Docs/Design/AssetSystem 12.13절 — A5i, 규칙은 UE 5.8에 맞춘 12.5절·12.2절 1–5번·
// 12.4.3절): 원본 정규 픽셀을 선형화 → flipGreen → PowerOfTwoMode → 최대 크기 → mip 사슬 →
// NormalizeNormals → 쿠킹 편향 → 출력 부호화해 쿠킹 mip으로 만든다. 순수
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

// 유효 설정이다(텍스처 값 + 그룹 값 — ResolveTextureBuildSettings가 만든다, 시험은 직접 채운다).
struct TextureBuildSettings {
    TextureDimension dimension = TextureDimension::Texture2D;
    TextureSourceFormat format = TextureSourceFormat::Invalid;
    uint32 sizeX = 0;
    uint32 sizeY = 0;
    uint32 numSlices = 0;
    TextureCompressionSettings compression = TextureCompressionSettings::Default;
    bool srgb = true;
    bool useLegacyGamma = false;
    TextureSourceEncoding encodingOverride = TextureSourceEncoding::None;
    bool flipGreenChannel = false;
    TexturePowerOfTwoSetting powerOfTwoMode = TexturePowerOfTwoSetting::None;
    Color paddingColor{0, 0, 0, 255};
    bool padWithBorderColor = false;
    uint32 resizeX = 0;
    uint32 resizeY = 0;
    // SimpleAverage·NoMipmaps·Unfiltered 하나다(FromTextureGroup은 그룹 값으로 바뀐 뒤).
    TextureMipGenSettings mipGenSettings = TextureMipGenSettings::SimpleAverage;
    bool preserveBorder = false;
    // Normalmap일 때만 켠다(UE bNormalizeNormals && IsNormalMap()).
    bool normalizeNormals = false;
    // 축별 주소다(Cube·CubeArray는 Clamp — PreserveBorder Wrap 경고만 본다, 재표본은 늘 clamp).
    TextureAddress addressX = TextureAddress::Wrap;
    TextureAddress addressY = TextureAddress::Wrap;
    TextureAddress addressZ = TextureAddress::Wrap;
    // 쿠킹 편향(텍스처 + 그룹 LODBias)·최대 크기(0 = 없음)·그룹 크기 규칙이다(12.5절 4·7번).
    int32 lodBias = 0;
    int32 maxTextureSize = 0;
    int32 minLodSize = 1;
    int32 maxLodSize = 4096;
};

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

// 빌드 경고 하나다(SrgbIgnored·PreserveBorderWithWrap — 각 한 번).
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

    PixelFormat pixelFormat = PixelFormat::Unknown;
    bool srgb = false;
    TextureSwizzle swizzle[4] = {TextureSwizzle::R, TextureSwizzle::G, TextureSwizzle::B,
                                 TextureSwizzle::A};
    // Volume 1, 그 밖 원본 slice 수다.
    uint32 numSlices = 1;
    // 상위 mip부터다.
    Array<TextureBuiltMip> mips;
    Array<TextureBuildWarning> warnings;
    TextureBuildFailure failure;
};

// 12.4.3절 SRGB 열이 "무시"인 종류인지다(Normalmap·Masks·Displacementmap·VectorDisplacementmap·
// HDR·Alpha·DistanceFieldFont·HDR_Compressed·HalfFloat·SingleFloat·HDR_F32).
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

// settings와 원본 정규 픽셀(TextureSource bulk raw byte)로 쿠킹 mip을 만든다(out — 앞 결과는
// 지운다). 오류: 픽셀 byte 수·Cube 면 비정사각 InvalidArgument(InvalidSource), Resize가 Cube
// 정사각을 깸·숨김 CompressionSettings InvalidArgument(InvalidSetting), 할당 실패 OutOfMemory(진단
// None).
ROCKS_TEXTURETOOLS_API Result<void> BuildTexture(Allocator& allocator,
                                                 const TextureBuildSettings& settings,
                                                 Span<const uint8> sourcePixels,
                                                 TextureBuildResult& out) noexcept;

} // namespace rocks
