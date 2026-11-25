#pragma once

#include <Base/Guid.h>
#include <Core/Text/Name.h>
#include <Meta/Annotations.h>

// Static Bool Parameter 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionStaticBoolParameter —
// RE/Public/Materials/MaterialExpressionStaticBoolParameter.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct StaticBoolParameterProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name parameterName;
    ROCKS_PROPERTY() Guid parameterId {};
    ROCKS_PROPERTY() Name group;
    ROCKS_PROPERTY() int32 sortPriority = 32;
    ROCKS_PROPERTY() bool defaultValue {};
};

} // namespace rocks::material
