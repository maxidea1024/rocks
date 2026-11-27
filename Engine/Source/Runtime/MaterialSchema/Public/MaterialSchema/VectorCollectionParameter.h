#pragma once

#include <Base/Guid.h>
#include <Core/Math/Color.h>
#include <Core/Text/Name.h>
#include <Meta/Annotations.h>

// 파라미터 컬렉션의 vector 항목이다(Docs/Design/MaterialSystem 4.2·9.7절, UE
// FCollectionVectorParameter).
namespace rocks::material {

ROCKS_STRUCT()
struct VectorCollectionParameter {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Guid id;
    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() LinearColor defaultValue;
};

} // namespace rocks::material
