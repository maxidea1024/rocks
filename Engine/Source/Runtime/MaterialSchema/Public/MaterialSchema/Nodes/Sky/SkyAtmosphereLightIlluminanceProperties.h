#pragma once

#include <MaterialSchema/Nodes/Vectors/VectorsNodeEnums.h>
#include <Meta/Annotations.h>

// Sky Atmosphere Light Illuminance 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionSkyAtmosphereLightIlluminance —
// RE/Public/Materials/MaterialExpressionSkyAtmosphereLightIlluminance.h). 이름·기본값은 UE와 같다(b
// 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct SkyAtmosphereLightIlluminanceProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 lightIndex {};
    ROCKS_PROPERTY() PositionOrigin worldPositionOriginType = PositionOrigin::Absolute;
};

} // namespace rocks::material
