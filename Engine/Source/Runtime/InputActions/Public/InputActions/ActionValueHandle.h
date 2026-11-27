#pragma once

#include <Base/Types.h>

namespace rocks::input {

// 값 바인딩 하나의 handle이다(InputActionBindings::BindActionValue). 0은 없음이다.
struct ActionValueHandle {
    uint32 id = 0;

    friend constexpr bool operator==(ActionValueHandle, ActionValueHandle) noexcept = default;
};

} // namespace rocks::input
