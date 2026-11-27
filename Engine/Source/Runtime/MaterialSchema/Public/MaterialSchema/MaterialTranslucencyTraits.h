#pragma once

#include <Core/Math/Color.h>
#include <MaterialSchema/MaterialTraitEnums.h>
#include <Meta/Annotations.h>

// 반투명 머티리얼의 기본 속성 묶음이다(Docs/Design/MaterialSystem 19.9절 "traits.translucency").
// 효과는 Renderer RS5가 구현한다. 기본값은 UE UMaterial
// 생성자(RE/Private/Materials/Material.cpp:1137–)와 같다.
namespace rocks::material {

ROCKS_STRUCT()
struct MaterialTranslucencyTraits {
    ROCKS_GENERATED_BODY()

    // 그리는 pass 위치(UE TranslucencyPass).
    ROCKS_PROPERTY() TranslucencyPass pass = TranslucencyPass::AfterDepthOfField;
    // 조명 방식(UE TranslucencyLightingMode).
    ROCKS_PROPERTY()
    TranslucencyLightingMode lightingMode = TranslucencyLightingMode::VolumetricNonDirectional;
    // 모바일 분리 반투명(UE bEnableMobileSeparateTranslucency).
    ROCKS_PROPERTY() bool mobileSeparateTranslucency = false;
    ROCKS_PROPERTY() float directionalLightingIntensity = 1.0f;
    ROCKS_PROPERTY() float shadowDensityScale = 0.5f;
    ROCKS_PROPERTY() float selfShadowDensityScale = 2.0f;
    ROCKS_PROPERTY() float selfShadowSecondDensityScale = 10.0f;
    ROCKS_PROPERTY() float selfShadowSecondOpacity = 0.0f;
    ROCKS_PROPERTY() float backscatteringExponent = 30.0f;
    ROCKS_PROPERTY() LinearColor multipleScatteringExtinction { 1.0f, 0.833f, 0.588f, 1.0f };
    // 그림자 시작 오프셋(cm, UE TranslucentShadowStartOffset).
    ROCKS_PROPERTY() float shadowStartOffset = 100.0f;
    // 지역 광원 그림자(UE bAllowTranslucentLocalLightShadow)와 품질.
    ROCKS_PROPERTY() bool localLightShadow = false;
    ROCKS_PROPERTY() float localLightShadowQuality = 0.0f;
    ROCKS_PROPERTY() float directionalLightShadowQuality = 0.0f;
};

} // namespace rocks::material
