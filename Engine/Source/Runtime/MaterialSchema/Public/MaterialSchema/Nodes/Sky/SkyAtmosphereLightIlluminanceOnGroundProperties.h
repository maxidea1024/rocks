#pragma once

#include <Meta/Annotations.h>

// Sky Atmosphere Light Illuminance On Ground 노드의
// 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionSkyAtmosphereLightIlluminanceOnGround —
// RE/Public/Materials/MaterialExpressionSkyAtmosphereLightIlluminance.h). 이름·기본값은 UE와 같다(b
// 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct SkyAtmosphereLightIlluminanceOnGroundProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 lightIndex {};
};

} // namespace rocks::material
