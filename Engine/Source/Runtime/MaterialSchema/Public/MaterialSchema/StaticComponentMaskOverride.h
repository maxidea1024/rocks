#pragma once

#include <MaterialSchema/ParameterKey.h>
#include <Meta/Annotations.h>

// static component mask 파라미터 override다(Docs/Design/MaterialSystem 9.3·19.11절, static 변형).
namespace rocks::material {

ROCKS_STRUCT()
struct StaticComponentMaskOverride {
    ROCKS_GENERATED_BODY()

    explicit StaticComponentMaskOverride(Allocator& allocator) noexcept : key(allocator) {
    }

    ROCKS_PROPERTY() ParameterKey key;
    ROCKS_PROPERTY() bool r = false;
    ROCKS_PROPERTY() bool g = false;
    ROCKS_PROPERTY() bool b = false;
    ROCKS_PROPERTY() bool a = false;
};

} // namespace rocks::material
