#pragma once

#include <UiFramework/Layout/StackPanel.h>

namespace rocks::ui {

// 가로로 자식을 배치한다. 흐름 방향이 오른쪽에서 왼쪽이면 시각적 순서도 뒤집힌다.
class ROCKS_UIFRAMEWORK_API HorizontalStack final : public StackPanel {
    ROCKS_UI_ELEMENT(HorizontalStack, StackPanel)

public:
    explicit HorizontalStack(Allocator& allocator) noexcept;
};

} // namespace rocks::ui
