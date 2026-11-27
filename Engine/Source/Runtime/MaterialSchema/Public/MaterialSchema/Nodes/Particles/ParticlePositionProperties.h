#pragma once

#include <MaterialSchema/Nodes/Vectors/VectorsNodeEnums.h>
#include <Meta/Annotations.h>

// Particle Position 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionParticlePositionWS —
// RE/Public/Materials/MaterialExpressionParticlePositionWS.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct ParticlePositionProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() PositionOrigin originType = PositionOrigin::Absolute;
};

} // namespace rocks::material
