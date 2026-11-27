#pragma once

#include <Base/Guid.h>
#include <Core/Text/Name.h>
#include <Meta/Annotations.h>

// 파라미터 컬렉션의 scalar 항목이다(Docs/Design/MaterialSystem 4.2·9.7절, UE
// FCollectionScalarParameter). id는 이름 변경에도 남는 안정 Guid다.
namespace rocks::material {

ROCKS_STRUCT()
struct ScalarCollectionParameter {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Guid id;
    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() float defaultValue = 0.0f;
};

} // namespace rocks::material
