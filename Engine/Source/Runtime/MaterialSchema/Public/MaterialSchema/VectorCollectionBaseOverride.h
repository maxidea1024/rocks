#pragma once

#include <Base/Guid.h>
#include <Core/Math/Color.h>
#include <Meta/Annotations.h>

// 상속한 파라미터 컬렉션의 vector 기본값 override다(Docs/Design/MaterialSystem 4.6.1절, UE
// UMaterialParameterCollection::VectorParameterBaseOverrides — MaterialParameterCollection.h:228).
namespace rocks::material {

ROCKS_STRUCT()
struct VectorCollectionBaseOverride {
    ROCKS_GENERATED_BODY()

    // 부모 컬렉션 항목의 id(VectorCollectionParameter::id)다.
    ROCKS_PROPERTY() Guid parameterId;
    ROCKS_PROPERTY() LinearColor value;
};

} // namespace rocks::material
