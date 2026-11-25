#pragma once

#include <Base/Guid.h>
#include <Core/Math/Vector4.h>
#include <Core/Text/Name.h>
#include <Meta/Annotations.h>

// Double Vector Parameter 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionDoubleVectorParameter —
// RE/Public/Materials/MaterialExpressionDoubleVectorParameter.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct DoubleVectorParameterProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name parameterName;
    ROCKS_PROPERTY() Guid parameterId {};
    ROCKS_PROPERTY() Name group;
    ROCKS_PROPERTY() int32 sortPriority = 32;
    ROCKS_PROPERTY() Vec4d defaultValue {};
};

} // namespace rocks::material
