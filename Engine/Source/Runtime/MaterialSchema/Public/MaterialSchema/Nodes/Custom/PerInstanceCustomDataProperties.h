#pragma once

#include <Meta/Annotations.h>

// Per Instance Custom Data 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionPerInstanceCustomData —
// RE/Public/Materials/MaterialExpressionPerInstanceCustomData.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct PerInstanceCustomDataProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() uint32 dataIndex {};
};

} // namespace rocks::material
