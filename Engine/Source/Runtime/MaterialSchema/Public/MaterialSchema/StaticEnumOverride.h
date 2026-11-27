#pragma once

#include <Base/Types.h>
#include <MaterialSchema/ParameterKey.h>
#include <Meta/Annotations.h>

// static enum 파라미터 override다(Docs/Design/MaterialSystem 9.3·4.6.1절, static 변형). 값은 열거
// 항목 번호다.
namespace rocks::material {

ROCKS_STRUCT()
struct StaticEnumOverride {
    ROCKS_GENERATED_BODY()

    explicit StaticEnumOverride(Allocator& allocator) noexcept : key(allocator) {
    }

    ROCKS_PROPERTY() ParameterKey key;
    ROCKS_PROPERTY() int32 value = 0;
};

} // namespace rocks::material
