#pragma once

#include <Meta/Annotations.h>

// Surface Vertical Layer 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionSubstrateVerticalLayering —
// RE/Public/Materials/MaterialExpressionSubstrate.h). 이름·기본값은 UE와 같다(b 접두를 떼고
// camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct SurfaceVerticalLayerProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() bool useParameterBlending = false;
};

} // namespace rocks::material
