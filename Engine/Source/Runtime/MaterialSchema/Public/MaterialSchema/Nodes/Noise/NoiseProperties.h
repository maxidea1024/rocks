#pragma once

#include <MaterialSchema/Nodes/Noise/NoiseNodeEnums.h>
#include <MaterialSchema/Nodes/Vectors/VectorsNodeEnums.h>
#include <Meta/Annotations.h>

// Noise 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE UMaterialExpressionNoise —
// RE/Public/Materials/MaterialExpressionNoise.h). 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct NoiseProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() PositionOrigin worldPositionOriginType = PositionOrigin::Absolute;
    ROCKS_PROPERTY() float scale = 1.0f;
    ROCKS_PROPERTY() int32 quality = 1;
    ROCKS_PROPERTY() NoiseFunction noiseFunction = NoiseFunction::SimplexTex;
    ROCKS_PROPERTY() bool turbulence = true;
    ROCKS_PROPERTY() int32 levels = 6;
    ROCKS_PROPERTY() float outputMin = -1.0f;
    ROCKS_PROPERTY() float outputMax = 1.0f;
    ROCKS_PROPERTY() float levelScale = 2.0f;
    ROCKS_PROPERTY() bool tiling = false;
    ROCKS_PROPERTY() uint32 repeatSize = 512;
};

} // namespace rocks::material
