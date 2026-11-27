#pragma once

#include <Base/Types.h>

namespace rocks::input {

// 이벤트 바인딩 하나의 handle이다(InputActionBindings::BindAction). 0은 없음이다.
struct ActionBindingHandle {
    uint32 id = 0;

    friend constexpr bool operator==(ActionBindingHandle, ActionBindingHandle) noexcept = default;
};

} // namespace rocks::input
