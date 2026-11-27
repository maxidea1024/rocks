#pragma once

#include <MaterialSchema/Nodes/PostProcess/PostProcessNodeEnums.h>
#include <Meta/Annotations.h>

// Depth Of Field Function 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionDepthOfFieldFunction —
// RE/Public/Materials/MaterialExpressionDepthOfFieldFunction.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct DepthOfFieldFunctionProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY()
    DepthOfFieldFunctionValue functionValue = DepthOfFieldFunctionValue::NearAndFarMask;
};

} // namespace rocks::material
