#pragma once

#include <AssetRuntime/BulkData.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Math/Vector2.h>
#include <Core/Result.h>
#include <Core/Text/Name.h>
#include <Meta/Annotations.h>
#include <Texture/TextureEnums.h>

// 텍스처 값 struct다(Docs/Design/AssetSystem 12.1·12.4·12.8절): 원본(TextureSource — 에디터
// 데이터), 쿠킹 데이터(TexturePlatformData와 mip·샘플러), 설정 묶음(원본 색 설정·층 형식
// 설정·축소). 필드 이름은 camelCase다(UE 이름은 AssetSystem 12.4절 대응표).
namespace rocks {

// 원본 픽셀이다(UE FTextureSource 대응 — 12.4.4절). 원본 파일 byte가 아니라 디코딩한 정규 픽셀을
// Bulk(Zstd, raw SHA-256 = 원본 정체성)로 담는다. Texture가 EditorOnly 필드로 가진다. NumMips·
// NumLayers는 1만 구현이다(다른 값은 계약 — CheckTextureSettings). 검증 hook은 Format이 Invalid가
// 아니면 크기 > 0·NumSlices ≥ 1을 본다(차원별 slice 규칙은 빌드 판정 — A5i).
ROCKS_STRUCT()
struct TextureSource {
    ROCKS_GENERATED_BODY()

    // 형식·크기 규칙을 본다(위 설명 — 아니면 InvalidArgument).
    ROCKS_TEXTURE_API static Result<void> Validate(const TextureSource& source) noexcept;

    ROCKS_PROPERTY() TextureSourceFormat format = TextureSourceFormat::Invalid;
    ROCKS_PROPERTY() uint32 sizeX = 0;
    ROCKS_PROPERTY() uint32 sizeY = 0;
    ROCKS_PROPERTY() uint32 numSlices = 0;
    ROCKS_PROPERTY() uint32 numMips = 1;
    ROCKS_PROPERTY() uint32 numLayers = 1;
    ROCKS_PROPERTY() BulkDataRef bulk;
};

// 원본 색 해석 설정이다(UE FTextureSourceColorSettings — 12.2절). 색도 좌표 기본은 (0, 0)이다(UE
// 생성자 — ColorSpace에 따라 UE는 UpdateColorSpaceChromaticities가 채운다, Texture.cpp 262–270).
// 구현은 EncodingOverride None·Linear·sRGB와 ColorSpace None·sRGB이고 나머지는 계약이다.
ROCKS_STRUCT()
struct TextureSourceColorSettings {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY()
    TextureSourceEncoding encodingOverride = TextureSourceEncoding::None;
    ROCKS_PROPERTY() TextureColorSpace colorSpace = TextureColorSpace::None;
    ROCKS_PROPERTY() Vec2d redChromaticityCoordinate { 0.0, 0.0 };
    ROCKS_PROPERTY()
    Vec2d greenChromaticityCoordinate{0.0, 0.0};
    ROCKS_PROPERTY()
    Vec2d blueChromaticityCoordinate{0.0, 0.0};
    ROCKS_PROPERTY()
    Vec2d whiteChromaticityCoordinate{0.0, 0.0};
    ROCKS_PROPERTY()
    TextureChromaticAdaptationMethod chromaticAdaptationMethod =
        TextureChromaticAdaptationMethod::Bradford;
};

// 층별 형식 설정이다(UE FTextureFormatSettings — 다층 원본, 계약).
ROCKS_STRUCT()
struct TextureFormatSettings {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY()
    TextureCompressionSettings compressionSettings = TextureCompressionSettings::Default;
    ROCKS_PROPERTY() uint8 compressionNoAlpha : 1 = 0;
    ROCKS_PROPERTY() uint8 compressionForceAlpha : 1 = 0;
    ROCKS_PROPERTY() uint8 compressionNone : 1 = 0;
    ROCKS_PROPERTY() uint8 compressionYCoCg : 1 = 0;
    ROCKS_PROPERTY() uint8 srgb : 1 = 0;
};

// 플랫폼별 축소 비율이다(UE FPerPlatformFloat 대응 — 계약). PerPlatform 키는 플랫폼 이름이다.
ROCKS_STRUCT()
struct TextureDownscale {
    ROCKS_GENERATED_BODY()

    explicit TextureDownscale(Allocator& allocator) noexcept : perPlatform(allocator) {
    }

    ROCKS_PROPERTY() float defaultValue = 0.0f;
    ROCKS_PROPERTY() OrderedMap<Name, float> perPlatform;
};

// 쿠킹 mip 하나다(12.1절): 크기, 패딩 제외 byte 수, 위치(스트리밍 mip은 Bulk, 상주 꼬리 mip은
// TexturePlatformData::TailBulk 안 오프셋 — 한 mip의 모든 slice·면·깊이를 함께 담는다).
ROCKS_STRUCT()
struct TextureMipData {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() uint32 sizeX = 0;
    ROCKS_PROPERTY() uint32 sizeY = 0;
    ROCKS_PROPERTY() uint32 sizeZ = 0;
    ROCKS_PROPERTY() uint64 byteSize = 0;
    ROCKS_PROPERTY() BulkDataRef bulk;
    ROCKS_PROPERTY() uint64 tailOffset = 0;
};

// 쿠킹 샘플러다(텍스처·LOD 그룹에서 정한 값 — 렌더러가 그대로 쓴다).
ROCKS_STRUCT()
struct TextureSamplerSettings {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() TextureSamplerFilter filter = TextureSamplerFilter::Bilinear;
    ROCKS_PROPERTY() uint32 maxAniso = 0;
    ROCKS_PROPERTY() TextureAddress addressU = TextureAddress::Wrap;
    ROCKS_PROPERTY() TextureAddress addressV = TextureAddress::Wrap;
    ROCKS_PROPERTY() TextureAddress addressW = TextureAddress::Wrap;
    ROCKS_PROPERTY()
    TextureBorderColor borderColor = TextureBorderColor::TransparentBlack;
};

// 쿠킹 데이터다(UE FTexturePlatformData 대응 — 12.1절). 쿠킹 처리기(A5j)가 채우고 에디터
// 패키지에서는 기본값이라 차이 저장이 생략한다. PixelFormat은 GraphicsTypes PixelFormat 번호다(저장
// 형식 — 번호를 바꾸지 않는다, 12.8절). 검증 hook은 PixelFormat이 0(아직 없음)이거나 알려진 비압축
// 색 형식인지 본다. mip 크기 사슬·꼬리 범위의 일관성은 쿠킹 처리기와 렌더 업로드가 본다.
ROCKS_STRUCT()
struct TexturePlatformData {
    ROCKS_GENERATED_BODY()

    explicit TexturePlatformData(Allocator& allocator) noexcept : mips(allocator) {
    }

    // PixelFormat 번호를 본다(위 설명 — 아니면 InvalidArgument).
    ROCKS_TEXTURE_API static Result<void> Validate(const TexturePlatformData& data) noexcept;

    ROCKS_PROPERTY() TextureDimension dimension = TextureDimension::Texture2D;
    ROCKS_PROPERTY() uint8 pixelFormat = 0;
    ROCKS_PROPERTY() bool srgb = false;
    ROCKS_PROPERTY() TextureSwizzle swizzleR = TextureSwizzle::R;
    ROCKS_PROPERTY() TextureSwizzle swizzleG = TextureSwizzle::G;
    ROCKS_PROPERTY() TextureSwizzle swizzleB = TextureSwizzle::B;
    ROCKS_PROPERTY() TextureSwizzle swizzleA = TextureSwizzle::A;
    ROCKS_PROPERTY() uint32 sizeX = 0;
    ROCKS_PROPERTY() uint32 sizeY = 0;
    ROCKS_PROPERTY() uint32 sizeZ = 0;
    ROCKS_PROPERTY() uint32 numMips = 0;
    ROCKS_PROPERTY() uint32 numSlices = 0;
    ROCKS_PROPERTY() Array<TextureMipData> mips;
    ROCKS_PROPERTY() BulkDataRef tailBulk;
    ROCKS_PROPERTY() uint32 residentTailMips = 0;
    ROCKS_PROPERTY() TextureSamplerSettings sampler;
};

} // namespace rocks
