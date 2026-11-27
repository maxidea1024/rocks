#pragma once

#include <Base/Guid.h>
#include <Core/Text/Name.h>
#include <MaterialSchema/MaterialAssetReference.h>
#include <Meta/Annotations.h>

// Parameter Collection Value 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionCollectionParameter —
// RE/Public/Materials/MaterialExpressionCollectionParameter.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct ParameterCollectionValueProperties {
    ROCKS_GENERATED_BODY()

    // 기대 클래스는 ParameterCollection다(19.4.3절).
    ROCKS_PROPERTY() MaterialAssetReference collection;
    ROCKS_PROPERTY() Name parameterName;
    ROCKS_PROPERTY() Guid parameterId {};
    ROCKS_PROPERTY() Name group;
    ROCKS_PROPERTY() int32 sortPriority = 32;
};

} // namespace rocks::material
