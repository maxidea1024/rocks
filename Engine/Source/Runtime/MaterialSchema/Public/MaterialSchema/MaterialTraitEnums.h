#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 머티리얼 기본 속성(traits)의 열거다(Docs/Design/MaterialSystem 4.6·19.9절). 값은 UE 5.8 열거와
// 같은 순서·같은 뜻이고 이름은 UE 접두(MDR_·MTP_·TLM_ 등)를 뗀 나머지다. 저장 형식이라 바꾸지
// 않는다. UE의 _MAX·_Count 표지와 deprecated 별칭은 두지 않는다.
namespace rocks::material {

// 데칼이 이 머티리얼에 쓰는 속성이다(UE EMaterialDecalResponse —
// RE/Public/Materials/Material.h:118).
ROCKS_ENUM()
enum class DecalResponse : uint8 {
    None = 0,
    ColorNormalRoughness = 1,
    Color = 2,
    ColorNormal = 3,
    ColorRoughness = 4,
    Normal = 5,
    NormalRoughness = 6,
    Roughness = 7
};

// 반투명을 그리는 pass 위치다(UE EMaterialTranslucencyPass — Material.h:142).
ROCKS_ENUM()
enum class TranslucencyPass : uint8 {
    BeforeDepthOfField = 0,
    AfterDepthOfField = 1,
    AfterMotionBlur = 2
};

// 반투명 조명 방식이다(UE ETranslucencyLightingMode — RE/Classes/Engine/EngineTypes.h:316).
ROCKS_ENUM()
enum class TranslucencyLightingMode : uint8 {
    VolumetricNonDirectional = 0,
    VolumetricDirectional = 1,
    VolumetricPerVertexNonDirectional = 2,
    VolumetricPerVertexDirectional = 3,
    Surface = 4,
    SurfacePerPixelLighting = 5
};

// 굴절 방식이다(UE ERefractionMode — EngineTypes.h:361).
ROCKS_ENUM()
enum class RefractionMethod : uint8 {
    IndexOfRefraction = 0,
    PixelNormalOffset = 1,
    Offset2D = 2,
    None = 3,
    IndexOfRefractionFromF0 = 4
};

// 굴절이 coverage를 고려하는가다(UE ERefractionCoverageMode — EngineTypes.h:400).
ROCKS_ENUM()
enum class RefractionCoverageMode : uint8 {
    CoverageIgnored = 0,
    CoverageAccountedFor = 1
};

// pixel depth offset 방향이다(UE EPixelDepthOffsetMode — EngineTypes.h:418).
ROCKS_ENUM()
enum class PixelDepthOffsetMode : uint8 {
    // UE PDOM_Legacy: view 깊이 방향으로 민다. UE 기본값이라 이름만 "Legacy"이고 deprecated가
    // 아니다.
    ViewDepth = 0,
    AlongCameraVector = 1
};

// 가변 shading rate다(UE EMaterialShadingRate — EngineTypes.h:991).
ROCKS_ENUM()
enum class MaterialShadingRate : uint8 {
    Rate1x1 = 0,
    Rate2x1 = 1,
    Rate1x2 = 2,
    Rate2x2 = 3,
    Rate4x2 = 4,
    Rate2x4 = 5,
    Rate4x4 = 6
};

// PostProcess 영역 머티리얼의 삽입 위치다(UE EBlendableLocation —
// RE/Classes/Engine/BlendableInterface.h:14). 값은 UE 번호 그대로다(순서가 번호와 다르다).
ROCKS_ENUM()
enum class BlendableLocation : uint8 {
    SceneColorAfterTonemapping = 0,
    SceneColorAfterDepthOfField = 1,
    SceneColorBeforeDepthOfField = 2,
    ReplacingTonemapper = 3,
    ScreenSpaceReflectionInput = 4,
    TranslucencyAfterDepthOfField = 5,
    SceneColorBeforeBloom = 6
};

// PostProcess stencil 비교다(UE EMaterialStencilCompare — EngineTypes.h:977).
ROCKS_ENUM()
enum class StencilCompare : uint8 {
    Less = 0,
    LessEqual = 1,
    Greater = 2,
    GreaterEqual = 3,
    Equal = 4,
    NotEqual = 5,
    Never = 6,
    Always = 7
};

// 머티리얼 함수의 쓰임이다(UE EMaterialFunctionUsage —
// RE/Public/Materials/MaterialFunctionInterface.h:32).
ROCKS_ENUM()
enum class MaterialFunctionUsage : uint8 {
    General = 0,
    Layer = 1,
    LayerBlend = 2
};

// shading model preset 값이다(UE EMaterialShadingModel 전부 —
// RE/Classes/Engine/EngineTypes.h:707–728). 인스턴스 override와 SurfaceFromShadingModel이 쓴다.
// Renderer 번호표(RenderData ShadingModel)와 달리 UE의 PreintegratedSkin과 FromMaterialExpression도
// 받고, SurfaceFromShadingModel이 번호표로 바꾼다(10.3절). UE 내부 값 MSM_Strata(12)·MSM_NUM(13)은
// 두지 않는다(번호는 UE와 같게 비운다).
ROCKS_ENUM()
enum class ShadingModelPreset : uint8 {
    Unlit = 0,
    DefaultLit = 1,
    Subsurface = 2,
    PreintegratedSkin = 3,
    ClearCoat = 4,
    SubsurfaceProfile = 5,
    TwoSidedFoliage = 6,
    Hair = 7,
    Cloth = 8,
    Eye = 9,
    SingleLayerWater = 10,
    ThinTranslucent = 11,
    // 그래프의 ShadingModel 출력이 정한다.
    FromMaterialExpression = 14
};

} // namespace rocks::material
