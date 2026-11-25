#pragma once

#include <MaterialSchema/ParameterKey.h>
#include <Meta/Annotations.h>

// dynamic bool 파라미터 override다(셰이더 분기 — 9.3절)(Docs/Design/MaterialSystem 4.3·19.11절).
// 키는 9.2절 ParameterKey다.
namespace rocks::material {

ROCKS_STRUCT()
struct DynamicBoolParameterOverride {
    ROCKS_GENERATED_BODY()

    explicit DynamicBoolParameterOverride(Allocator& allocator) noexcept : key(allocator) {
    }

    ROCKS_PROPERTY() ParameterKey key;
    ROCKS_PROPERTY() bool value = false;
};

} // namespace rocks::material
