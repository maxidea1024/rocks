#pragma once

#include <MaterialSchema/Nodes/Vectors/VectorsNodeEnums.h>
#include <Meta/Annotations.h>

// Object Position 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionObjectPositionWS — RE/Public/Materials/MaterialExpressionObjectPositionWS.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct ObjectPositionProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() PositionOrigin originType = PositionOrigin::Absolute;
};

} // namespace rocks::material
