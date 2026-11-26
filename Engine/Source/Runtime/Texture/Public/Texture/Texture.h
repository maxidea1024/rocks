#pragma once

#include <Base/Compiler.h>
#include <AssetRuntime/PerPlatformFloat.h>
#include <AssetRuntime/StreamableRenderAsset.h>
#include <Core/Math/Color.h>
#include <Core/Math/Vector4.h>
#include <Object/ObjectRefs.h>
#include <Texture/TextureSource.h>

// 텍스처 에셋 클래스다(Docs/Design/AssetSystem 12.1·12.4·12.8절, UE 5.8 UTexture와 차원 클래스).
// 필드 이름(도구 이름도)은 Rocks 규칙대로 camelCase이고 bool에 b 접두를 붙이지 않는다(UE 이름은
// 12.4절 대응표 — LODGroup → lodGroup, bFlipGreenChannel → flipGreenChannel). 기본값은 UE
// 생성자 값이다(Texture.cpp 148행 — 그 밖은 0). EditorOnly 구분은 UE WITH_EDITORONLY_DATA 구간과
// 같다. 계약 설정(12.4.1절)은 스키마에 있고 CheckTextureSettings(Texture/TextureSettingsCheck.h)가
// 판정한다.
namespace rocks {

// 텍스처 공통 설정이다(UE UTexture — 추상).
ROCKS_CLASS(Abstract)
class ROCKS_TEXTURE_API Texture : public StreamableRenderAsset {
    ROCKS_GENERATED_BODY()

public:
    explicit Texture(Allocator& allocator) noexcept
        :
#if ROCKS_WITH_EDITOR_DATA
          layerFormatSettings(allocator),
#endif
          downscale(allocator), platformData(allocator) {
    }

    // ---- 에디터 데이터(UE WITH_EDITORONLY_DATA) ----
#if ROCKS_WITH_EDITOR_DATA
    ROCKS_PROPERTY(EditorOnly) TextureSource source;
    // 보정(계약).
    ROCKS_PROPERTY(EditorOnly) float adjustBrightness = 1.0f;
    ROCKS_PROPERTY(EditorOnly) float adjustBrightnessCurve = 1.0f;
    ROCKS_PROPERTY(EditorOnly) float adjustVibrance = 0.0f;
    ROCKS_PROPERTY(EditorOnly) float adjustSaturation = 1.0f;
    ROCKS_PROPERTY(EditorOnly) float adjustRGBCurve = 1.0f;
    ROCKS_PROPERTY(EditorOnly) float adjustHue = 0.0f;
    ROCKS_PROPERTY(EditorOnly) float adjustMinAlpha = 0.0f;
    ROCKS_PROPERTY(EditorOnly) float adjustMaxAlpha = 1.0f;
    // 압축(비압축 쿠킹에서는 결과를 바꾸지 않는다).
    ROCKS_PROPERTY(EditorOnly) uint32 compressionNoAlpha : 1 = 0;
    ROCKS_PROPERTY(EditorOnly) uint32 compressionForceAlpha : 1 = 0;
    ROCKS_PROPERTY(EditorOnly) uint32 compressionNone : 1 = 0;
    ROCKS_PROPERTY(EditorOnly)
    TextureLossyCompressionAmount lossyCompressionAmount = TextureLossyCompressionAmount::Default;
    // Rocks는 Oodle을 쓰지 않는다 — 기록만(UE가 새 텍스처에 최신 버전을 쓴다).
    ROCKS_PROPERTY(EditorOnly) Name oodleTextureSdkVersion;
    ROCKS_PROPERTY(EditorOnly) int32 maxTextureSize = 0;
    ROCKS_PROPERTY(EditorOnly)
    TextureCompressionQuality compressionQuality = TextureCompressionQuality::Default;
    ROCKS_PROPERTY(EditorOnly) Guid compressionCacheId;
    ROCKS_PROPERTY(EditorOnly)
    bool doScaleMipsForAlphaCoverage = false;
    ROCKS_PROPERTY(EditorOnly)
    Vec4d alphaCoverageThresholds{0.0, 0.0, 0.0, 0.0};
    // 기록만(빌드 결과에 영향 없음 — 12.4.1절).
    ROCKS_PROPERTY(EditorOnly) bool useNewMipFilter = false;
    ROCKS_PROPERTY(EditorOnly) uint32 preserveBorder : 1 = 0;
    ROCKS_PROPERTY(EditorOnly) uint32 flipGreenChannel : 1 = 0;
    ROCKS_PROPERTY(EditorOnly)
    TexturePowerOfTwoSetting powerOfTwoMode = TexturePowerOfTwoSetting::None;
    ROCKS_PROPERTY(EditorOnly) Color paddingColor { 0, 0, 0, 255 };
    ROCKS_PROPERTY(EditorOnly) bool padWithBorderColor = false;
    ROCKS_PROPERTY(EditorOnly) int32 resizeDuringBuildX = 0;
    ROCKS_PROPERTY(EditorOnly) int32 resizeDuringBuildY = 0;
    ROCKS_PROPERTY(EditorOnly) bool chromaKeyTexture = false;
    ROCKS_PROPERTY(EditorOnly)
    float chromaKeyThreshold = 1.0f / 255.0f;
    ROCKS_PROPERTY(EditorOnly) Color chromaKeyColor { 255, 0, 255, 255 };
    ROCKS_PROPERTY(EditorOnly)
    TextureMipGenSettings mipGenSettings = TextureMipGenSettings::FromTextureGroup;
    ROCKS_PROPERTY(EditorOnly) ObjectRef<Texture> compositeTexture;
    ROCKS_PROPERTY(EditorOnly)
    TextureCompositeMode compositeTextureMode = TextureCompositeMode::NormalRoughnessToAlpha;
    ROCKS_PROPERTY(EditorOnly) float compositePower = 1.0f;
    ROCKS_PROPERTY(EditorOnly)
    Array<TextureFormatSettings> layerFormatSettings;
    ROCKS_PROPERTY(EditorOnly) uint8 normalizeNormals : 1 = 0;
    ROCKS_PROPERTY(EditorOnly) uint8 useLegacyGamma : 1 = 0;
    ROCKS_PROPERTY(EditorOnly)
    TextureSourceColorSettings sourceColorSettings;
#endif

    // ---- 저장(런타임) ----
    ROCKS_PROPERTY() int32 lodBias = 0;
    ROCKS_PROPERTY()
    TextureCompressionSettings compressionSettings = TextureCompressionSettings::Default;
    ROCKS_PROPERTY() TextureFilter filter = TextureFilter::Default;
    ROCKS_PROPERTY()
    TextureMipLoadOptions mipLoadOptions = TextureMipLoadOptions::Default;
    ROCKS_PROPERTY()
    VirtualTextureProducerPriority virtualTextureStreamingPriority =
        VirtualTextureProducerPriority::Normal;
    ROCKS_PROPERTY() uint8 virtualTexturePrefetchMips = 0;
    ROCKS_PROPERTY()
    TextureCookPlatformTilingSettings cookPlatformTilingSettings =
        TextureCookPlatformTilingSettings::FromTextureGroup;
    ROCKS_PROPERTY() bool oodlePreserveExtremes = false;
    ROCKS_PROPERTY() TextureGroup lodGroup = TextureGroup::World;
    // 플랫폼별 축소 비율이다(UE FPerPlatformFloat Downscale — 계약, 키는 TargetProfile id).
    ROCKS_PROPERTY() PerPlatformFloat downscale;
    ROCKS_PROPERTY()
    TextureDownscaleOptions downscaleOptions = TextureDownscaleOptions::Default;
    ROCKS_PROPERTY()
    TextureAvailability availability = TextureAvailability::GPU;
    ROCKS_PROPERTY() uint8 srgb : 1 = 1;
    ROCKS_PROPERTY() uint8 noTiling : 1 = 0;
    ROCKS_PROPERTY() uint8 virtualTextureStreaming : 1 = 0;
    ROCKS_PROPERTY()
    uint32 useVirtualTextureStreamingPriority : 1 = 0;
    ROCKS_PROPERTY() uint8 compressionYCoCg : 1 = 0;
    // 쿠킹 데이터(쿠킹 처리기가 채운다 — A5j).
    ROCKS_PROPERTY() TexturePlatformData platformData;
};

// 2D 텍스처다(UE UTexture2D — 주소 X·Y, Rocks Border 색).
ROCKS_CLASS()
class ROCKS_TEXTURE_API Texture2D final : public Texture {
    ROCKS_GENERATED_BODY()

public:
    using Texture::Texture;

    ROCKS_PROPERTY() TextureAddress addressX = TextureAddress::Wrap;
    ROCKS_PROPERTY() TextureAddress addressY = TextureAddress::Wrap;
    ROCKS_PROPERTY()
    TextureBorderColor borderColor = TextureBorderColor::TransparentBlack;
};

// Cube 텍스처다(UE UTextureCube — 고유 설정 없음, 샘플러는 Clamp).
ROCKS_CLASS()
class ROCKS_TEXTURE_API TextureCube final : public Texture {
    ROCKS_GENERATED_BODY()

public:
    using Texture::Texture;
};

// 2D 배열 텍스처다(UE UTexture2DArray — 주소 X·Y·Z, 원본 Texture2D 목록).
ROCKS_CLASS()
class ROCKS_TEXTURE_API Texture2DArray final : public Texture {
    ROCKS_GENERATED_BODY()

public:
    explicit Texture2DArray(Allocator& allocator) noexcept
        : Texture(allocator)
#if ROCKS_WITH_EDITOR_DATA
          ,
          sourceTextures(allocator)
#endif
    {
    }

    ROCKS_PROPERTY() TextureAddress addressX = TextureAddress::Wrap;
    ROCKS_PROPERTY() TextureAddress addressY = TextureAddress::Wrap;
    ROCKS_PROPERTY() TextureAddress addressZ = TextureAddress::Wrap;
    ROCKS_PROPERTY()
    TextureBorderColor borderColor = TextureBorderColor::TransparentBlack;
#if ROCKS_WITH_EDITOR_DATA
    ROCKS_PROPERTY(EditorOnly) ObjectRefArray<Texture2D> sourceTextures;
#endif
};

// Cube 배열 텍스처다(UE UTextureCubeArray — 원본 TextureCube 목록).
ROCKS_CLASS()
class ROCKS_TEXTURE_API TextureCubeArray final : public Texture {
    ROCKS_GENERATED_BODY()

public:
    explicit TextureCubeArray(Allocator& allocator) noexcept
        : Texture(allocator)
#if ROCKS_WITH_EDITOR_DATA
          ,
          sourceTextures(allocator)
#endif
    {
    }

#if ROCKS_WITH_EDITOR_DATA
    ROCKS_PROPERTY(EditorOnly) ObjectRefArray<TextureCube> sourceTextures;
#endif
};

// 볼륨 텍스처다(UE UVolumeTexture — 세 축 공통 주소, 원본 2D 텍스처와 타일 크기).
ROCKS_CLASS()
class ROCKS_TEXTURE_API VolumeTexture final : public Texture {
    ROCKS_GENERATED_BODY()

public:
    using Texture::Texture;

    ROCKS_PROPERTY() TextureAddress addressMode = TextureAddress::Wrap;
    ROCKS_PROPERTY()
    TextureBorderColor borderColor = TextureBorderColor::TransparentBlack;
#if ROCKS_WITH_EDITOR_DATA
    ROCKS_PROPERTY(EditorOnly) ObjectRef<Texture2D> source2DTexture;
    ROCKS_PROPERTY(EditorOnly) int32 source2DTileSizeX = 0;
    ROCKS_PROPERTY(EditorOnly) int32 source2DTileSizeY = 0;
#endif
};

// Texture 모듈 리플렉션 타입을 등록한다(생성 집계 함수 generated::Texture::RegisterModuleTypes —
// 의존 모듈 타입도 함께). 도구·시험은 직접, 엔진은 모듈 시작이 부른다.
ROCKS_TEXTURE_API Result<void> RegisterTextureTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks
