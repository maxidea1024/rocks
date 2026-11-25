#pragma once

#include <Meta/Annotations.h>

// Sky Atmosphere Light Disk Luminance 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionSkyAtmosphereLightDiskLuminance —
// RE/Public/Materials/MaterialExpressionSkyAtmosphereLightIlluminance.h). 이름·기본값은 UE와 같다(b
// 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct SkyAtmosphereLightDiskLuminanceProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 lightIndex {};
};

} // namespace rocks::material
