#pragma once

#include <Base/Guid.h>
#include <Core/Memory/Allocator.h>
#include <InputActions/InputActionSettings.h>
#include <Meta/Annotations.h>

namespace rocks::input {

// 액션 하나의 사본(id + 설정)이다(Docs/Design/InputActions 2.7절). 컨텍스트 bundle과 주입이 쓴다.
ROCKS_STRUCT()
struct InputActionData {
    ROCKS_GENERATED_BODY()

    explicit InputActionData(Allocator& allocator) noexcept : settings(allocator) {
    }

    ROCKS_PROPERTY() Guid id;
    ROCKS_PROPERTY() InputActionSettings settings;
};

} // namespace rocks::input
