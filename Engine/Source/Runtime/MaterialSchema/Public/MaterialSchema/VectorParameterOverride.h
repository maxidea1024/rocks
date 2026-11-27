#pragma once

#include <Core/Math/Color.h>
#include <MaterialSchema/ParameterKey.h>
#include <Meta/Annotations.h>

// vector 파라미터 override다(Docs/Design/MaterialSystem 4.3·19.11절). 키는 9.2절 ParameterKey다.
namespace rocks::material {

ROCKS_STRUCT()
struct VectorParameterOverride {
    ROCKS_GENERATED_BODY()

    explicit VectorParameterOverride(Allocator& allocator) noexcept : key(allocator) {
    }

    ROCKS_PROPERTY() ParameterKey key;
    ROCKS_PROPERTY() LinearColor value;
};

} // namespace rocks::material
