#pragma once

#include <MaterialSchema/Nodes/Constants/ConstantsNodeEnums.h>
#include <Meta/Annotations.h>

// View Property 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionViewProperty — RE/Public/Materials/MaterialExpressionViewProperty.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct ViewPropertyProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() ExposedViewProperty property = ExposedViewProperty::FieldOfView;
};

} // namespace rocks::material
