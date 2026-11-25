#pragma once

#include <Core/Math/Vector2.h>
#include <Meta/Annotations.h>

// Temporal Sobol 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionTemporalSobol — RE/Public/Materials/MaterialExpressionTemporalSobol.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct TemporalSobolProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Vec2d constSeed = Vec2d(0.0, 0.0);
};

} // namespace rocks::material
