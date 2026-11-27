#pragma once

#include <MaterialSchema/Nodes/Math/MathNodeEnums.h>
#include <Meta/Annotations.h>

// Clamp 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE UMaterialExpressionClamp —
// RE/Public/Materials/MaterialExpressionClamp.h). 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct ClampProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() ClampMode clampMode = ClampMode::Clamp;
};

} // namespace rocks::material
