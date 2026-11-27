#pragma once

#include <MaterialSchema/ParameterKey.h>
#include <Meta/Annotations.h>

// static switch·static bool 파라미터 override다(static 변형
// — 9.5절)(Docs/Design/MaterialSystem 4.3·19.11절). 키는 9.2절 ParameterKey다.
namespace rocks::material {

ROCKS_STRUCT()
struct StaticSwitchOverride {
    ROCKS_GENERATED_BODY()

    explicit StaticSwitchOverride(Allocator& allocator) noexcept : key(allocator) {
    }

    ROCKS_PROPERTY() ParameterKey key;
    ROCKS_PROPERTY() bool value = false;
};

} // namespace rocks::material
