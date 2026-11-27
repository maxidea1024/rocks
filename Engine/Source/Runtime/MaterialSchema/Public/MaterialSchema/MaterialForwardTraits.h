#pragma once

#include <Meta/Annotations.h>

// Forward+ 경로 전용 기본 속성 묶음이다(Docs/Design/MaterialSystem 19.9절 "traits.forward"). 효과는
// Renderer Forward+ 단계가 구현한다. 기본값은 UE와 같다(모두 false).
namespace rocks::material {

ROCKS_STRUCT()
struct MaterialForwardTraits {
    ROCKS_GENERATED_BODY()

    // UE bForwardRenderUsePreintegratedGFForSimpleIBL.
    ROCKS_PROPERTY() bool preintegratedSimpleIbl = false;
    // UE bUseHQForwardReflections.
    ROCKS_PROPERTY() bool highQualityReflections = false;
    // UE bForwardBlendsSkyLightCubemaps.
    ROCKS_PROPERTY() bool blendSkyLightCubemaps = false;
    // UE bUsePlanarForwardReflections.
    ROCKS_PROPERTY() bool planarReflections = false;
};

} // namespace rocks::material
