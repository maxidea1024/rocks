#pragma once

#include <Base/Types.h>
#include <Core/Text/Name.h>
#include <MaterialSchema/MaterialTraitEnums.h>
#include <Meta/Annotations.h>

// PostProcess 영역 머티리얼의 기본 속성 묶음이다(Docs/Design/MaterialSystem 19.9절
// "traits.postProcess"). 삽입 위치는 계획 26절 후처리 사슬이 쓴다. 기본값은 UE와 같다.
namespace rocks::material {

ROCKS_STRUCT()
struct MaterialPostProcessTraits {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() BlendableLocation location = BlendableLocation::SceneColorAfterTonemapping;
    ROCKS_PROPERTY() int32 priority = 0;
    // 후처리 volume 혼합 대상인가(UE bIsBlendable).
    ROCKS_PROPERTY() bool blendable = true;
    ROCKS_PROPERTY() bool outputAlpha = false;
    // 사용자 scene texture 이름과 해상도 나눗수(UE UserSceneTexture·UserTextureDivisor).
    ROCKS_PROPERTY() Name userSceneTexture;
    ROCKS_PROPERTY() int32 userTextureDivisorX = 0;
    ROCKS_PROPERTY() int32 userTextureDivisorY = 0;
    ROCKS_PROPERTY() Name resolutionRelativeToInput;
    ROCKS_PROPERTY() bool disablePreExposureScale = false;
    ROCKS_PROPERTY() bool stencilTest = false;
    ROCKS_PROPERTY() StencilCompare stencilCompare = StencilCompare::Less;
    ROCKS_PROPERTY() uint8 stencilRef = 0;
};

} // namespace rocks::material
