#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <InputActions/InputBinding.h>
#include <Meta/Annotations.h>

namespace rocks::input {

// 키 프로필 하나의 바인딩 목록이다(UE MappingProfileOverrides 값 — Docs/Design/InputActions 2.4절).
// 그 프로필이 활성이면 컨텍스트의 기본 bindings를 통째로 대신한다.
ROCKS_STRUCT()
struct InputProfileBindings {
    ROCKS_GENERATED_BODY()

    explicit InputProfileBindings(Allocator& allocator) noexcept
        : profile(allocator), bindings(allocator) {
    }

    // 키 프로필 id다(5.2절).
    ROCKS_PROPERTY() String profile;
    ROCKS_PROPERTY() Array<InputBinding> bindings;
};

} // namespace rocks::input
