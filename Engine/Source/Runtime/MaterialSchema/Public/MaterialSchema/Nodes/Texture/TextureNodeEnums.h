#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// Texture 범주 노드 속성의 열거다(Docs/Design/MaterialSystem 19.13.2절). 값 번호와 뜻은
// UE 5.8 열거와 같고 이름은 UE 접두를 뗀 나머지다. UE의 숨은 값과 _MAX 표지는 두지 않는다. 저장
// 형식이라 바꾸지 않는다.
namespace rocks::material {

// ExposedTextureProperty 값이다(UE EMaterialExposedTextureProperty —
// RE/Public/Materials/MaterialExpressionTextureProperty.h).
ROCKS_ENUM()
enum class ExposedTextureProperty : uint8 {
    TextureSize = 0,
    TexelSize = 1
};

// PathTracingBufferTextureId 값이다(UE EPathTracingBufferTextureId —
// RE/Public/Materials/MaterialExpressionPathTracingBufferTexture.h).
ROCKS_ENUM()
enum class PathTracingBufferTextureId : uint8 {
    Radiance = 0,
    DenoisedRadiance = 1,
    Albedo = 2,
    Normal = 3,
    Variance = 4
};

// SamplerSourceMode 값이다(UE ESamplerSourceMode — RE/Classes/Engine/EngineTypes.h).
ROCKS_ENUM()
enum class SamplerSourceMode : uint8 {
    FromTextureAsset = 0,
    WrapWorldGroupSettings = 1,
    ClampWorldGroupSettings = 2
};

// SamplerType 값이다(UE EMaterialSamplerType — RE/Classes/Engine/EngineTypes.h).
ROCKS_ENUM()
enum class SamplerType : uint8 {
    Color = 0,
    Grayscale = 1,
    Alpha = 2,
    Normal = 3,
    Masks = 4,
    DistanceFieldFont = 5,
    LinearColor = 6,
    LinearGrayscale = 7,
    Data = 8,
    External = 9,
    VirtualColor = 10,
    VirtualGrayscale = 11,
    VirtualAlpha = 12,
    VirtualNormal = 13,
    VirtualMasks = 14,
    VirtualLinearColor = 15,
    VirtualLinearGrayscale = 16
};

// SceneAttributeInputMode 값이다(UE EMaterialSceneAttributeInputMode —
// RE/Public/Materials/MaterialExpressionSceneColor.h).
ROCKS_ENUM()
enum class SceneAttributeInputMode : uint8 {
    Coordinates = 0,
    OffsetFraction = 1
};

// SceneTextureId 값이다(UE ESceneTextureId — RE/Public/MaterialSceneTextureId.h).
ROCKS_ENUM()
enum class SceneTextureId : uint8 {
    SceneColor = 0,
    SceneDepth = 1,
    DiffuseColor = 2,
    SpecularColor = 3,
    SubsurfaceColor = 4,
    BaseColor = 5,
    Specular = 6,
    Metallic = 7,
    WorldNormal = 8,
    SeparateTranslucency = 9,
    Opacity = 10,
    Roughness = 11,
    MaterialAO = 12,
    CustomDepth = 13,
    PostProcessInput0 = 14,
    PostProcessInput1 = 15,
    PostProcessInput2 = 16,
    PostProcessInput3 = 17,
    PostProcessInput4 = 18,
    PostProcessInput5 = 19,
    PostProcessInput6 = 20,
    DecalMask = 21,
    ShadingModelColor = 22,
    ShadingModelID = 23,
    AmbientOcclusion = 24,
    CustomStencil = 25,
    StoredBaseColor = 26,
    StoredSpecular = 27,
    Velocity = 28,
    WorldTangent = 29,
    Anisotropy = 30,
    IsFirstPerson = 31
};

// TextureCollectionMemberType 값이다(UE ETextureCollectionMemberType —
// RE/Public/Materials/MaterialExpressionTextureObjectFromCollection.h).
ROCKS_ENUM()
enum class TextureCollectionMemberType : uint8 {
    Texture2D = 0,
    TextureCube = 1,
    Texture2DArray = 2,
    TextureCubeArray = 3,
    Texture3D = 4
};

// TextureColorChannel 값이다(UE ETextureColorChannel —
// RE/Public/Materials/MaterialExpressionAntialiasedTextureMask.h).
ROCKS_ENUM()
enum class TextureColorChannel : uint8 {
    Red = 0,
    Green = 1,
    Blue = 2,
    Alpha = 3
};

// TextureGatherMode 값이다(UE ETextureGatherMode —
// RE/Public/Materials/MaterialExpressionTextureSample.h).
ROCKS_ENUM()
enum class TextureGatherMode : uint8 {
    None = 0,
    Red = 1,
    Green = 2,
    Blue = 3,
    Alpha = 4
};

// TextureMipValueMode 값이다(UE ETextureMipValueMode — RE/Classes/Engine/EngineTypes.h).
ROCKS_ENUM()
enum class TextureMipValueMode : uint8 {
    None = 0,
    MipLevel = 1,
    MipBias = 2,
    Derivative = 3
};

} // namespace rocks::material
