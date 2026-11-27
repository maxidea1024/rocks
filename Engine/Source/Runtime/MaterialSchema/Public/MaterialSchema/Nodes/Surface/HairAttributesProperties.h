#pragma once

#include <Meta/Annotations.h>

// Hair Attributes 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionHairAttributes — RE/Public/Materials/MaterialExpressionHairAttributes.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct HairAttributesProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() bool useTangentSpace = true;
};

} // namespace rocks::material
