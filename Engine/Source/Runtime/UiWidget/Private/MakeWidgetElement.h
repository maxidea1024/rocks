#pragma once

#include <UiCore/Element.h>

namespace rocks::ui {

// 리플렉션 위젯의 네이티브 표현을 생성하고 할당 오류를 그대로 호출자에게 전달한다.
template <typename T, typename... Args>
Result<SharedPtr<Element>> MakeWidgetElement(Allocator& allocator, Args&&... args) noexcept {
    auto value = SharedPtr<T>::TryMake(allocator, allocator, Forward<Args>(args)...);
    if (!value) {
        return Result<SharedPtr<Element>>::Failure(value.Error());
    }

    return Result<SharedPtr<Element>>::Success(Move(value.Value()));
}

} // namespace rocks::ui
