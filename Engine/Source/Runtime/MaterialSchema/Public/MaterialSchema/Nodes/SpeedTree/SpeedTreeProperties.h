#pragma once

#include <MaterialSchema/Nodes/SpeedTree/SpeedTreeNodeEnums.h>
#include <Meta/Annotations.h>

// SpeedTree 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionSpeedTree — RE/Public/Materials/MaterialExpressionSpeedTree.h). 이름·기본값은
// UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct SpeedTreeProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() SpeedTreeGeometryType geometryType = SpeedTreeGeometryType::Branch;
    ROCKS_PROPERTY() SpeedTreeWindType windType = SpeedTreeWindType::None;
    ROCKS_PROPERTY() SpeedTreeLodType lodType = SpeedTreeLodType::Pop;
    ROCKS_PROPERTY() bool accurateWindVelocities = false;
};

} // namespace rocks::material
