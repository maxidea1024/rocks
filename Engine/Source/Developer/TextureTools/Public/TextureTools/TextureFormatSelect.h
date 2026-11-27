#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <Core/Result.h>
#include <GraphicsTypes/GraphicsBackend.h>
#include <GraphicsTypes/GraphicsPlatform.h>
#include <GraphicsTypes/PixelFormat.h>
#include <Texture/TextureEnums.h>
#include <TextureTools/TextureBlockEncode.h>

// 쿠킹 텍스처의 출력 형식 선택이다(Docs/Design/AssetSystem 12.17.5 2 — UE 5.8
// GetDefaultTextureFormatName·TextureFormatDXT::GetEncodedPixelFormat 순서). 설정·원본 형식·상위
// mip 크기·대상 플랫폼과 backend에서 PixelFormat, 출력 sRGB, swizzle(sampler type 계약), 인코딩,
// 채널 복제, 경고를 정한다. 순수 함수다(픽셀을 보지 않는다 — AutoDXT의 알파만 호출자가 잰다).
//
// 사용 예(빌드):
//   TextureFormatRequest request;
//   request.compression = TextureCompressionSettings::Default;
//   request.sourceFormat = TextureSourceFormat::RGBA8;
//   request.topSizeX = 256;
//   request.topSizeY = 256;
//   TextureFormatTarget target;  // Windows, 검증 끔
//   const bool alpha = TextureFormatNeedsAlpha(request, target) && /* mip 0 알파 판정 */ false;
//   TextureFormatChoice choice;
//   if (auto chosen = SelectTextureFormat(request, target, alpha, choice); !chosen) { ... }
//   // choice.pixelFormat == BC1UnormSrgb, choice.encoding == TextureFormatEncoding::Bc1
namespace rocks {

// 출력 데이터를 만드는 방법이다. 비압축은 PixelFormat의 byte 배치로 쓴다.
enum class TextureFormatEncoding : uint8 {
    Uncompressed,
    Bc1,
    Bc3,
    Bc4,
    Bc5,
    // ISPC Texture Compressor(12.17.6 — 입력은 RGBA16F).
    Bc6h,
    // ISPC Texture Compressor(12.17.6 — 알파 프로필은 alphaProfile).
    Bc7,
    // 자체 EAC R11 인코더(12.17.8 4 — 입력은 R16).
    EacR11,
    // astc-encoder(12.17.8 3 — 블록 크기는 pixelFormat, 채널 배치는 astcMode).
    Astc,
};

// 출력 전 채널 복제다(12.17절 TD6 — UE bReplicateRed·bReplicateAlpha).
enum class TextureChannelReplicate : uint8 {
    None,
    // RGBA = R(Grayscale·Alpha)
    Red,
    // RGBA = A(Displacementmap·DistanceFieldFont)
    Alpha,
};

// backend 하나의 비트다(TextureFormatTarget::backends).
constexpr uint32 TextureFormatBackendBit(graphics::Backend backend) noexcept {
    return uint32{1} << static_cast<uint32>(backend);
}

// 쿠킹 대상이다(프로필에서 만든다 — 12.17.5 4).
struct TextureFormatTarget {
    graphics::GraphicsPlatform platform = graphics::GraphicsPlatform::Windows;
    // Backend 비트 집합(TextureFormatBackendBit)이다. 0이면 샘플링 보장 검증(TD12)을 끈다. 32F 선형
    // 필터는 플랫폼 기본으로 본다(Windows·Mac 됨, Android 안 됨 — UE
    // bSupportFilteredFloat32Textures).
    uint32 backends = 0;
    // 16비트 unorm 형식을 쓸 수 있는지다(못 하면 R16Unorm → R16Float — 12.14절).
    bool norm16Supported = true;
};

// 형식을 고를 텍스처 값이다.
struct TextureFormatRequest {
    TextureCompressionSettings compression = TextureCompressionSettings::Default;
    // 텍스처 srgb(막는 설정이면 무시한다 — UE bPreventSRGB).
    bool srgb = true;
    TextureSourceFormat sourceFormat = TextureSourceFormat::Invalid;
    TextureDimension dimension = TextureDimension::Texture2D;
    TextureGroup lodGroup = TextureGroup::World;
    bool compressionNone = false;
    bool compressionNoAlpha = false;
    bool compressionForceAlpha = false;
    // 쿠킹 샘플러 필터가 Point인지다(32F 대체·샘플링 검증의 필터 요구).
    bool pointFilter = false;
    // 쿠킹된 상위 mip 크기다(TextureBuildTopMipSize — UE GetBuiltTextureSize).
    uint32 topSizeX = 0;
    uint32 topSizeY = 0;
    // ASTC 블록 크기를 정하는 품질이다(12.16.2 — Android만 본다, UE GetQualityFormat).
    TextureCompressionQuality compressionQuality = TextureCompressionQuality::Default;
};

// 형식 경고 수 상한이다(EncoderUnavailable, PlatformFormatFallback — 각 한 번. TC3부터 형식 선택은
// EncoderUnavailable을 내지 않는다 — 12.17.6 9).
inline constexpr usize TextureFormatMaxWarnings = 2;

struct TextureFormatWarning {
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    // 대체 전 PixelFormat 번호다.
    uint64 detail = 0;
};

// 고른 형식이다.
struct TextureFormatChoice {
    graphics::PixelFormat pixelFormat = graphics::PixelFormat::Unknown;
    // 출력이 sRGB 형식인지다(pixelFormat의 sRGB 짝을 골랐을 때만).
    bool srgb = false;
    // srgb를 켠 텍스처인데 설정(막는 종류·ColorLookupTable)이나 float → float 출력 때문에 선형으로
    // 쓰는지다(SrgbIgnored 경고 — 12.13절).
    bool srgbIgnored = false;
    TextureSwizzle swizzle[4] = {TextureSwizzle::R, TextureSwizzle::G, TextureSwizzle::B,
                                 TextureSwizzle::A};
    TextureFormatEncoding encoding = TextureFormatEncoding::Uncompressed;
    TextureChannelReplicate replicate = TextureChannelReplicate::None;
    // G16 → R16F norm16 대체를 했는지다(빌드가 값을 0…1로 자른다 — 12.14절. 사용자가 고른 R16F인
    // HalfFloat는 자르지 않는다).
    bool norm16Remapped = false;
    // BC7 인코더가 알파 프로필을 쓰는지다(12.17.6 4 — TD5 유효 알파: ForceNoAlpha면 거짓, 아니고
    // ForceAlpha면 참, 아니면 원본 알파 판정. UE GetProfile_alpha_basic·GetProfile_basic). BC7 밖은
    // 거짓이다.
    bool alphaProfile = false;
    // ASTC 입력 채널 배치다(12.17.8 6 — 알파 없는 RGBA·RGBA HQ와 BC6H 대체는 Rgb, 노멀은 NormalLa).
    // ASTC 밖은 Rgba(보지 않는다)다.
    TextureAstcMode astcMode = TextureAstcMode::Rgba;
    usize warningCount = 0;
    TextureFormatWarning warnings[TextureFormatMaxWarnings];
};

// request·target에서 형식을 고른다. alphaTransparent는 원본 알파 판정
// 결과다(AutoDXT(Default·Masks)의 형식과 BC7 프로필만 본다 — TextureFormatNeedsAlpha). out은 앞
// 값을 지운다.
//
// 오류(InvalidArgument): 원본 형식 Invalid, 상위 mip 크기 0, 숨김 CompressionSettings(LQ·
// EncodedReflectionCapture — 12.8절), 범위 밖 enum, 대상 backend에 쿠킹 대상이 아닌 조합이 끼어
// 보장 대체가 없음(TD12).
ROCKS_TEXTURETOOLS_API Result<void> SelectTextureFormat(const TextureFormatRequest& request,
                                                        const TextureFormatTarget& target,
                                                        bool alphaTransparent,
                                                        TextureFormatChoice& out) noexcept;

// 알파 판정 결과가 출력에 영향을 주는지다: 고른 형식이 달라지거나(AutoDXT — UE
// TextureCompressorModule 4016–4036의 두 형식 비교) BC7 인코더 프로필이나 ASTC 채널 배치가 달라질
// 때(12.17.6 4·12.17.8 6) 참이다. 빌드는 참일 때만 원본 알파를 잰다. 오류 입력이면 false다.
ROCKS_TEXTURETOOLS_API bool TextureFormatNeedsAlpha(const TextureFormatRequest& request,
                                                    const TextureFormatTarget& target) noexcept;

} // namespace rocks
