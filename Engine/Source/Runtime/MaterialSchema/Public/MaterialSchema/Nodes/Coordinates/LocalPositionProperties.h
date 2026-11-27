#pragma once

#include <MaterialSchema/Nodes/Coordinates/CoordinatesNodeEnums.h>
#include <Meta/Annotations.h>

// Local Position 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionLocalPosition — RE/Public/Materials/MaterialExpressionLocalPosition.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct LocalPositionProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY()
    PositionIncludedOffsets includedOffsets = PositionIncludedOffsets::IncludeOffsets;
    ROCKS_PROPERTY() LocalPositionOrigin localOrigin = LocalPositionOrigin::Instance;
};

} // namespace rocks::material
