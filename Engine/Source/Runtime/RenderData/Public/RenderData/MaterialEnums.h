#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 머티리얼 번호표다(Docs/Design/Renderer 14.2절, 결정 RN5). 정의 권한은 Renderer 스펙에 있다.
// 머티리얼 스키마·에셋·pass 셰이더가 이 열거를 참조한다. HLSL 상수는 `/Engine/Shared/` 공용 헤더에
// 두고 static_assert로 맞춘다(MS2a에서 더한다).
//
// 값은 UE 5.8 열거와 같은 번호다(UE 에셋 교환이 번호를 그대로 옮긴다). 저장 형식이라 바꾸지 않는다.
// UE의 _MAX·_NUM 표지와 deprecated 값은 두지 않는다. 지금 렌더러가 그리지 않는 값도 둔다:
// 머티리얼은 받고, 그 값을 그리는 단계 전에는 거르기·진단이 처리한다(머티리얼 시스템 19장).
namespace rocks {

// 머티리얼 영역이다(UE EMaterialDomain — RE/Public/MaterialDomain.h:12–30). UE
// MD_RuntimeVirtualTexture는 deprecated라 두지 않는다(값 6 비움).
ROCKS_ENUM()
enum class MaterialDomain : uint8 {
    Surface = 0,
    DeferredDecal = 1,
    LightFunction = 2,
    Volume = 3,
    PostProcess = 4,
    UserInterface = 5
};

// 블렌드 모드다(UE EBlendMode — RE/Classes/Engine/EngineTypes.h:245–255).
ROCKS_ENUM()
enum class BlendMode : uint8 {
    Opaque = 0,
    Masked = 1,
    Translucent = 2,
    Additive = 3,
    Modulate = 4,
    // premultiplied alpha.
    AlphaComposite = 5,
    // TODO(MPQ-AlphaHoldout): holdout pass가 생기면 렌더러가 그린다. 그 전에는 머티리얼 진단이다.
    AlphaHoldout = 6,
    // TODO(MPQ-ColoredTransmittance): dual-source blending 반투명 경로가 생기면 렌더러가 그린다.
    TranslucentColoredTransmittance = 7
};

// shading model이다(UE EMaterialShadingModel — RE/Classes/Engine/EngineTypes.h:707–728). GBuffer 4
// bit에 들어간다. UE MSM_PreintegratedSkin(3)은 SubsurfaceProfile이 받으므로(Renderer 14.2절)
// 비운다. UE MSM_Strata(12)는 Substrate 내부 값이라 두지 않는다. 그래프에서 정하는 경우(UE
// MSM_FromMaterialExpression)는 번호가 아니라 머티리얼 번역이 계산한 집합이다.
ROCKS_ENUM()
enum class ShadingModel : uint8 {
    Unlit = 0,
    DefaultLit = 1,
    Subsurface = 2,
    ClearCoat = 4,
    SubsurfaceProfile = 5,
    TwoSidedFoliage = 6,
    Hair = 7,
    Cloth = 8,
    Eye = 9,
    SingleLayerWater = 10,
    ThinTranslucent = 11
};

// shading model 번호의 상한(4 bit)이다.
inline constexpr uint32 MaxShadingModelCount = 16;

} // namespace rocks
