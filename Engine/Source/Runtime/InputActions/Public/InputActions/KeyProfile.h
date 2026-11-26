#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <InputActions/KeyMappingRow.h>

namespace rocks::input {

// 키 프로필 하나다(UE UEnhancedPlayerMappableKeyProfile — Docs/Design/InputActions 5.2절). id는
// 컨텍스트의 프로필 override·supportedProfiles 이름과 같은 공간이다.
struct KeyProfile {
    explicit KeyProfile(Allocator& allocator) noexcept
        : id(allocator), displayName(allocator), rows(allocator) {
    }

    String id;
    String displayName;
    Array<KeyMappingRow> rows;
};

} // namespace rocks::input
