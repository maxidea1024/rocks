#pragma once

#include <Base/Guid.h>
#include <Core/Text/Name.h>
#include <Meta/Annotations.h>

// Static Component Mask Parameter 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionStaticComponentMaskParameter —
// RE/Public/Materials/MaterialExpressionStaticComponentMaskParameter.h). 이름·기본값은 UE와 같다(b
// 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct StaticComponentMaskParameterProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name parameterName;
    ROCKS_PROPERTY() Guid parameterId {};
    ROCKS_PROPERTY() Name group;
    ROCKS_PROPERTY() int32 sortPriority = 32;
    ROCKS_PROPERTY() bool defaultR {};
    ROCKS_PROPERTY() bool defaultG {};
    ROCKS_PROPERTY() bool defaultB {};
    ROCKS_PROPERTY() bool defaultA {};
};

} // namespace rocks::material
