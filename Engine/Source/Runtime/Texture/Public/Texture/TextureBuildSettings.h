#pragma once

#include <Base/Types.h>
#include <Core/Math/Color.h>
#include <Meta/Annotations.h>
#include <Texture/TextureEnums.h>

// 텍스처 빌드의 유효 설정이다(Docs/Design/AssetSystem 12.13·12.14절 — 텍스처 값 + LOD 그룹 값을
// 합친 결과, TextureTools ResolveTextureBuildSettings가 만든다). 리플렉션 struct인 까닭은 쿠킹
// 너겟 키가 이것을 WriteReflected로 쓰기 때문이다(9.1절 결정 2 — 칸을 더하면 키가 저절로 바뀐다).
// 에셋 명령 세션이 이미 등록하는 Texture 모듈에 둔다(임포트 옵션과 같은 이유).
namespace rocks {

ROCKS_STRUCT()
struct TextureBuildSettings {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() TextureDimension dimension = TextureDimension::Texture2D;
    ROCKS_PROPERTY() TextureSourceFormat format = TextureSourceFormat::Invalid;
    ROCKS_PROPERTY() uint32 sizeX = 0;
    ROCKS_PROPERTY() uint32 sizeY = 0;
    ROCKS_PROPERTY() uint32 numSlices = 0;
    ROCKS_PROPERTY()
    TextureCompressionSettings compression = TextureCompressionSettings::Default;
    ROCKS_PROPERTY() bool srgb = true;
    ROCKS_PROPERTY() bool useLegacyGamma = false;
    ROCKS_PROPERTY()
    TextureSourceEncoding encodingOverride = TextureSourceEncoding::None;
    ROCKS_PROPERTY() bool flipGreenChannel = false;
    ROCKS_PROPERTY()
    TexturePowerOfTwoSetting powerOfTwoMode = TexturePowerOfTwoSetting::None;
    ROCKS_PROPERTY() Color paddingColor { 0, 0, 0, 255 };
    ROCKS_PROPERTY() bool padWithBorderColor = false;
    ROCKS_PROPERTY() uint32 resizeX = 0;
    ROCKS_PROPERTY() uint32 resizeY = 0;
    // SimpleAverage·NoMipmaps·Unfiltered 하나다(FromTextureGroup은 그룹 값으로 바뀐 뒤).
    ROCKS_PROPERTY()
    TextureMipGenSettings mipGenSettings = TextureMipGenSettings::SimpleAverage;
    ROCKS_PROPERTY() bool preserveBorder = false;
    // Normalmap일 때만 켠다(UE bNormalizeNormals && IsNormalMap()).
    ROCKS_PROPERTY() bool normalizeNormals = false;
    // 축별 주소다(Cube·CubeArray는 Clamp — PreserveBorder Wrap 경고만 본다, 재표본은 늘 clamp).
    ROCKS_PROPERTY() TextureAddress addressX = TextureAddress::Wrap;
    ROCKS_PROPERTY() TextureAddress addressY = TextureAddress::Wrap;
    ROCKS_PROPERTY() TextureAddress addressZ = TextureAddress::Wrap;
    // 쿠킹 편향(텍스처 + 그룹 LODBias)·최대 크기(0 = 없음)·그룹 크기 규칙이다(12.5절 4·7번).
    ROCKS_PROPERTY() int32 lodBias = 0;
    ROCKS_PROPERTY() int32 maxTextureSize = 0;
    ROCKS_PROPERTY() int32 minLodSize = 1;
    ROCKS_PROPERTY() int32 maxLodSize = 4096;
    // 대상이 R16 Unorm을 표본할 수 있는지다(false면 R16F — 12.14절).
    ROCKS_PROPERTY() bool norm16Supported = true;
    // 출력 형식 선택 입력이다(AssetSystem 12.17.5 3 — TextureTools SelectTextureFormat). 텍스처의
    // 압축 플래그와 LOD 그룹, 쿠킹 샘플러 필터가 Point인지, 쿠킹 대상 플랫폼(GraphicsPlatform 값)과
    // backend 비트 집합(1 << Backend — 0이면 샘플링 보장 검증을 끈다)이다.
    ROCKS_PROPERTY() bool compressionNone = false;
    ROCKS_PROPERTY() bool compressionNoAlpha = false;
    ROCKS_PROPERTY() bool compressionForceAlpha = false;
    ROCKS_PROPERTY() TextureGroup lodGroup = TextureGroup::World;
    ROCKS_PROPERTY() bool samplerPointFilter = false;
    ROCKS_PROPERTY() uint8 cookPlatform = 0;
    ROCKS_PROPERTY() uint32 cookBackends = 0;
};

} // namespace rocks
