#pragma once

#include <Meta/Annotations.h>

// Component Mask 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionComponentMask — RE/Public/Materials/MaterialExpressionComponentMask.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct ComponentMaskProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() bool r {};
    ROCKS_PROPERTY() bool g {};
    ROCKS_PROPERTY() bool b {};
    ROCKS_PROPERTY() bool a {};
};

} // namespace rocks::material
