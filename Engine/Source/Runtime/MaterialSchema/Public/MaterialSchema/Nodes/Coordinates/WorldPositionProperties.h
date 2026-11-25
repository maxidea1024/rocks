#pragma once

#include <MaterialSchema/Nodes/Coordinates/CoordinatesNodeEnums.h>
#include <Meta/Annotations.h>

// World Position 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionWorldPosition — RE/Public/Materials/MaterialExpressionWorldPosition.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct WorldPositionProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY()
    WorldPositionIncludedOffsets worldPositionShaderOffset = WorldPositionIncludedOffsets::Default;
};

} // namespace rocks::material
