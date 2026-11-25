#pragma once

#include <MaterialSchema/Nodes/Vectors/VectorsNodeEnums.h>
#include <Meta/Annotations.h>

// Distance Field Approx Ao 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionDistanceFieldApproxAO —
// RE/Public/Materials/MaterialExpressionDistanceFieldApproxAO.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct DistanceFieldApproxAoProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() PositionOrigin worldPositionOriginType = PositionOrigin::Absolute;
    ROCKS_PROPERTY() uint32 numSteps = 1;
    ROCKS_PROPERTY() float stepScaleDefault = 3.0f;
};

} // namespace rocks::material
