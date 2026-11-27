#pragma once

#include <MaterialSchema/ParameterKey.h>
#include <Meta/Annotations.h>

// scalar 파라미터 override다(Docs/Design/MaterialSystem 4.3·19.11절). 키는 9.2절 ParameterKey다.
namespace rocks::material {

ROCKS_STRUCT()
struct ScalarParameterOverride {
    ROCKS_GENERATED_BODY()

    explicit ScalarParameterOverride(Allocator& allocator) noexcept : key(allocator) {
    }

    ROCKS_PROPERTY() ParameterKey key;
    ROCKS_PROPERTY() float value = 0.0f;
};

} // namespace rocks::material
