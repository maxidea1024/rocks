#pragma once

#include <Base/Guid.h>
#include <Meta/Annotations.h>

// 상속한 파라미터 컬렉션의 scalar 기본값 override다(Docs/Design/MaterialSystem 4.6.1절, UE
// UMaterialParameterCollection::ScalarParameterBaseOverrides — MaterialParameterCollection.h:224).
namespace rocks::material {

ROCKS_STRUCT()
struct ScalarCollectionBaseOverride {
    ROCKS_GENERATED_BODY()

    // 부모 컬렉션 항목의 id(ScalarCollectionParameter::id)다.
    ROCKS_PROPERTY() Guid parameterId;
    ROCKS_PROPERTY() float value = 0.0f;
};

} // namespace rocks::material
