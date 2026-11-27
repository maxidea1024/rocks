#pragma once

#include <MaterialSchema/Nodes/Noise/NoiseNodeEnums.h>
#include <MaterialSchema/Nodes/Vectors/VectorsNodeEnums.h>
#include <Meta/Annotations.h>

// Vector Noise 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionVectorNoise — RE/Public/Materials/MaterialExpressionVectorNoise.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct VectorNoiseProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() PositionOrigin worldPositionOriginType = PositionOrigin::Absolute;
    ROCKS_PROPERTY() VectorNoiseFunction noiseFunction = VectorNoiseFunction::CellnoiseALU;
    ROCKS_PROPERTY() int32 quality = 1;
    ROCKS_PROPERTY() bool tiling = false;
    ROCKS_PROPERTY() uint32 tileSize = 300;
};

} // namespace rocks::material
