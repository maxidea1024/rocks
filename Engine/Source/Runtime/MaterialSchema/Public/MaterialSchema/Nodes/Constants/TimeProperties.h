#pragma once

#include <Meta/Annotations.h>

// Time (Stopped) 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionTime — RE/Public/Materials/MaterialExpressionTime.h). 이름·기본값은 UE와
// 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct TimeProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() bool ignorePause {};
    ROCKS_PROPERTY() bool override_Period = false;
    ROCKS_PROPERTY() float period = 0.0f;
};

} // namespace rocks::material
