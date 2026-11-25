#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 텍스처 설정 enum이다(Docs/Design/AssetSystem 12.4·12.8절). 값은 UE 5.8과 같고(TextureDefines.h —
// 저장 형식이라 바꾸지 않는다) 이름은 UE 접두사(TC_·TMGS_·TEXTUREGROUP_·TF_·TA_ 등)를 뗀 나머지
// 그대로다(UE 접두가 붙은 이름은 받지 않는다 — UE 교환은 대응표로 옮긴다). 바탕 형식은 모두
// uint8이다. UE의 _MAX 표지는 두지 않는다.
namespace rocks {

// 텍스처 LOD 그룹이다(UE TextureGroup 82종 — TextureDefines.h 28행, 값은 UE와 같다). Project01–48은
// 프로젝트 설정에서 표시 이름을 바꾼다. 숫자로 시작하는 UE 이름(8BitData·16BitData)은 C++ 이름을
// EightBitData·SixteenBitData로 쓰고 도구 이름은 UE 이름 그대로 노출한다.
ROCKS_ENUM()
enum class TextureGroup : uint8 {
    World = 0,
    WorldNormalMap = 1,
    WorldSpecular = 2,
    Character = 3,
    CharacterNormalMap = 4,
    CharacterSpecular = 5,
    Weapon = 6,
    WeaponNormalMap = 7,
    WeaponSpecular = 8,
    Vehicle = 9,
    VehicleNormalMap = 10,
    VehicleSpecular = 11,
    Cinematic = 12,
    Effects = 13,
    EffectsNotFiltered = 14,
    Skybox = 15,
    UI = 16,
    Lightmap = 17,
    RenderTarget = 18,
    MobileFlattened = 19,
    ProcBuilding_Face = 20,
    ProcBuilding_LightMap = 21,
    Shadowmap = 22,
    ColorLookupTable = 23,
    Terrain_Heightmap = 24,
    Terrain_Weightmap = 25,
    Bokeh = 26,
    IESLightProfile = 27,
    Pixels2D = 28,
    HierarchicalLOD = 29,
    Impostor = 30,
    ImpostorNormalDepth = 31,
    EightBitData ROCKS_ENUMERATOR(Name = "8BitData") = 32,
    SixteenBitData ROCKS_ENUMERATOR(Name = "16BitData") = 33,
    Project01 = 34,
    Project02 = 35,
    Project03 = 36,
    Project04 = 37,
    Project05 = 38,
    Project06 = 39,
    Project07 = 40,
    Project08 = 41,
    Project09 = 42,
    Project10 = 43,
    Project11 = 44,
    Project12 = 45,
    Project13 = 46,
    Project14 = 47,
    Project15 = 48,
    Project16 = 49,
    Project17 = 50,
    Project18 = 51,
    Project19 = 52,
    Project20 = 53,
    Project21 = 54,
    Project22 = 55,
    Project23 = 56,
    Project24 = 57,
    Project25 = 58,
    Project26 = 59,
    Project27 = 60,
    Project28 = 61,
    Project29 = 62,
    Project30 = 63,
    Project31 = 64,
    Project32 = 65,
    Project33 = 66,
    Project34 = 67,
    Project35 = 68,
    Project36 = 69,
    Project37 = 70,
    Project38 = 71,
    Project39 = 72,
    Project40 = 73,
    Project41 = 74,
    Project42 = 75,
    Project43 = 76,
    Project44 = 77,
    Project45 = 78,
    Project46 = 79,
    Project47 = 80,
    Project48 = 81,
};

// mip 생성 방식이다(UE TextureMipGenSettings 22종 — TextureDefines.h 128행). Sharpen·Blur·Angular·
// LeaveExistingMips는 계약이다(12.4.1절 — CheckTextureSettings가 UnsupportedSetting).
ROCKS_ENUM()
enum class TextureMipGenSettings : uint8 {
    FromTextureGroup = 0,
    SimpleAverage = 1,
    Sharpen0 = 2,
    Sharpen1 = 3,
    Sharpen2 = 4,
    Sharpen3 = 5,
    Sharpen4 = 6,
    Sharpen5 = 7,
    Sharpen6 = 8,
    Sharpen7 = 9,
    Sharpen8 = 10,
    Sharpen9 = 11,
    Sharpen10 = 12,
    NoMipmaps = 13,
    LeaveExistingMips = 14,
    Blur1 = 15,
    Blur2 = 16,
    Blur3 = 17,
    Blur4 = 18,
    Blur5 = 19,
    Unfiltered = 20,
    Angular = 21,
};

// 크기를 2의 거듭제곱으로 맞추는 방식이다(UE ETexturePowerOfTwoSetting — 12.5절).
ROCKS_ENUM()
enum class TexturePowerOfTwoSetting : uint8 {
    None = 0,
    PadToPowerOfTwo = 1,
    PadToSquarePowerOfTwo = 2,
    StretchToPowerOfTwo = 3,
    StretchToSquarePowerOfTwo = 4,
    ResizeToSpecificResolution = 5,
};

// 샘플러 필터다(UE ETextureSamplerFilter — 쿠킹 데이터의 샘플러, 12.4.5절 그룹 표).
ROCKS_ENUM()
enum class TextureSamplerFilter : uint8 {
    Point = 0,
    Bilinear = 1,
    Trilinear = 2,
    AnisotropicPoint = 3,
    AnisotropicLinear = 4,
};

// mip 로드 방식이다(UE ETextureMipLoadOptions — OnlyFirstMip은 계약).
ROCKS_ENUM()
enum class TextureMipLoadOptions : uint8 {
    Default = 0,
    AllMips = 1,
    OnlyFirstMip = 2,
};

// 픽셀을 어디에 두는지다(UE ETextureAvailability — CPU는 Texture2D 전용).
ROCKS_ENUM()
enum class TextureAvailability : uint8 {
    GPU = 0,
    CPU = 1,
};

// 축소 필터다(UE ETextureDownscaleOptions — 계약).
ROCKS_ENUM()
enum class TextureDownscaleOptions : uint8 {
    Default = 0,
    Unfiltered = 1,
    SimpleAverage = 2,
    Sharpen0 = 3,
    Sharpen1 = 4,
    Sharpen2 = 5,
    Sharpen3 = 6,
    Sharpen4 = 7,
    Sharpen5 = 8,
    Sharpen6 = 9,
    Sharpen7 = 10,
    Sharpen8 = 11,
    Sharpen9 = 12,
    Sharpen10 = 13,
};

// 손실 압축 정도다(UE ETextureLossyCompressionAmount — 계약).
ROCKS_ENUM()
enum class TextureLossyCompressionAmount : uint8 {
    Default = 0,
    None = 1,
    Lowest = 2,
    Low = 3,
    Medium = 4,
    High = 5,
    Highest = 6,
};

// 합성 텍스처 방식이다(UE ECompositeTextureMode — 계약).
ROCKS_ENUM()
enum class TextureCompositeMode : uint8 {
    Disabled = 0,
    NormalRoughnessToRed = 1,
    NormalRoughnessToGreen = 2,
    NormalRoughnessToBlue = 3,
    NormalRoughnessToAlpha = 4,
};

// 압축 설정이다(UE TextureCompressionSettings — 12.4.3절 쿠킹 형식 표).
// LQ·EncodedReflectionCapture는 UE도 숨김이다: 저장 값으로는 받고(binary) CheckTextureSettings가
// InvalidSetting으로, 텍스트 입력은 HiddenEnumerator(AssetService가 InvalidSetting으로 사상)로
// 거부한다.
ROCKS_ENUM()
enum class TextureCompressionSettings : uint8 {
    Default = 0,
    Normalmap = 1,
    Masks = 2,
    Grayscale = 3,
    Displacementmap = 4,
    VectorDisplacementmap = 5,
    HDR = 6,
    EditorIcon = 7,
    Alpha = 8,
    DistanceFieldFont = 9,
    HDR_Compressed = 10,
    BC7 = 11,
    HalfFloat = 12,
    LQ ROCKS_ENUMERATOR(Hidden) = 13,
    EncodedReflectionCapture ROCKS_ENUMERATOR(Hidden) = 14,
    SingleFloat = 15,
    HDR_F32 = 16,
};

// 원본 부호화 덮어쓰기다(UE ETextureSourceEncoding — None·Linear·sRGB 밖은 계약).
ROCKS_ENUM()
enum class TextureSourceEncoding : uint8 {
    None = 0,
    Linear = 1,
    sRGB = 2,
    ST2084 = 3,
    Gamma22 = 4,
    BT1886 = 5,
    Gamma26 = 6,
    Cineon = 7,
    REDLog = 8,
    REDLog3G10 = 9,
    SLog1 = 10,
    SLog2 = 11,
    SLog3 = 12,
    AlexaV3LogC = 13,
    CanonLog = 14,
    ProTune = 15,
    VLog = 16,
    HLG = 17,
};

// 원본 색 공간이다(UE ETextureColorSpace — None·sRGB 밖은 계약).
ROCKS_ENUM()
enum class TextureColorSpace : uint8 {
    None = 0,
    sRGB = 1,
    Rec2020 = 2,
    ACESAP0 = 3,
    ACESAP1 = 4,
    P3DCI = 5,
    P3D65 = 6,
    REDWideGamut = 7,
    SonySGamut3 = 8,
    SonySGamut3Cine = 9,
    AlexaWideGamut = 10,
    CanonCinemaGamut = 11,
    GoProProtuneNative = 12,
    PanasonicVGamut = 13,
    Custom = 99,
};

// 쿠킹 플랫폼 타일링이다(UE TextureCookPlatformTilingSettings — 타일링 플랫폼이 없어 결과는
// 비타일).
ROCKS_ENUM()
enum class TextureCookPlatformTilingSettings : uint8 {
    FromTextureGroup = 0,
    Tile = 1,
    DoNotTile = 2,
};

// 색 순응 방식이다(UE ETextureChromaticAdaptationMethod — 계약).
ROCKS_ENUM()
enum class TextureChromaticAdaptationMethod : uint8 {
    None = 0,
    Bradford = 1,
    CAT02 = 2,
};

// 텍스처 필터 설정이다(UE TextureFilter — Default는 그룹 값).
ROCKS_ENUM()
enum class TextureFilter : uint8 {
    Nearest = 0,
    Bilinear = 1,
    Trilinear = 2,
    Default = 3,
};

// 주소 방식이다(UE TextureAddress Wrap·Clamp·Mirror + Rocks Border — 12.4.2절).
ROCKS_ENUM()
enum class TextureAddress : uint8 {
    Wrap = 0,
    Clamp = 1,
    Mirror = 2,
    Border = 3,
};

// Border 주소의 색이다(Rocks 확장 — GraphicsTypes 샘플러 Border 색과 같은 셋).
ROCKS_ENUM()
enum class TextureBorderColor : uint8 {
    TransparentBlack = 0,
    OpaqueBlack = 1,
    OpaqueWhite = 2,
};

// ASTC 압축 품질이다(UE ETextureCompressionQuality — 계약).
ROCKS_ENUM()
enum class TextureCompressionQuality : uint8 {
    Default = 0,
    Lowest = 1,
    Low = 2,
    Medium = 3,
    High = 4,
    Highest = 5,
};

// 가상 텍스처 producer 우선순위다(UE EVTProducerPriority — VirtualTextureEnum.h 10행, 계약).
ROCKS_ENUM()
enum class VirtualTextureProducerPriority : uint8 {
    Lowest = 0,
    Lower = 1,
    Low = 2,
    BelowNormal = 3,
    Normal = 4,
    AboveNormal = 5,
    High = 6,
    Highest = 7,
    Count ROCKS_ENUMERATOR(Hidden) = 8,
};

// 원본 픽셀 형식이다(Rocks — 12.4.4절. UE ETextureSourceFormat 대응: GA8·GA16 추가, 색 순서 RGBA,
// BGRE8은 디코딩 때 float로).
ROCKS_ENUM()
enum class TextureSourceFormat : uint8 {
    Invalid = 0,
    G8 = 1,
    GA8 = 2,
    G16 = 3,
    GA16 = 4,
    RGBA8 = 5,
    RGBA16 = 6,
    RGBA16F = 7,
    RGBA32F = 8,
    R16F = 9,
    R32F = 10,
};

// 쿠킹 텍스처의 차원이다.
ROCKS_ENUM()
enum class TextureDimension : uint8 {
    Texture2D = 0,
    TextureCube = 1,
    Texture2DArray = 2,
    TextureCubeArray = 3,
    Volume = 4,
};

// 출력 채널이 형식의 어느 채널인지다(쿠킹 swizzle — UE sampler type 계약 데이터,
// Docs/Design/AssetSystem 12.16.4절). 셰이더가 sampler type에 따라 복제하며 렌더러는 view swizzle을
// 항등으로 둔다(D3D11에는 view swizzle이 없다 — 모든 backend가 같은 결과).
ROCKS_ENUM()
enum class TextureSwizzle : uint8 {
    R = 0,
    G = 1,
    B = 2,
    A = 3,
    Zero = 4,
    One = 5,
};

} // namespace rocks
