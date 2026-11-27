#pragma once

#include <MaterialSchema/ParameterKey.h>
#include <Meta/Annotations.h>

// double vector 파라미터 override다(Docs/Design/MaterialSystem 4.3·19.11절, 값 타입 Double4
// — 5.3절).
namespace rocks::material {

ROCKS_STRUCT()
struct DoubleVectorParameterOverride {
    ROCKS_GENERATED_BODY()

    explicit DoubleVectorParameterOverride(Allocator& allocator) noexcept : key(allocator) {
    }

    ROCKS_PROPERTY() ParameterKey key;
    ROCKS_PROPERTY() double x = 0.0;
    ROCKS_PROPERTY() double y = 0.0;
    ROCKS_PROPERTY() double z = 0.0;
    ROCKS_PROPERTY() double w = 0.0;
};

} // namespace rocks::material
