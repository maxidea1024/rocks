#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <InputActions/PlayerKeyMapping.h>

namespace rocks::input {

// 매핑 이름 하나의 사용자 키 목록이다(UE FKeyMappingRow — Docs/Design/InputActions 5.2절). slot은
// 서로 다르다.
struct KeyMappingRow {
    explicit KeyMappingRow(Allocator& allocator) noexcept
        : mappingName(allocator), mappings(allocator) {
    }

    String mappingName;
    Array<PlayerKeyMapping> mappings;
};

} // namespace rocks::input
