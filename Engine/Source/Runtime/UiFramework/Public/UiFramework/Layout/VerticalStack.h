#pragma once

#include <UiFramework/Layout/StackPanel.h>

namespace rocks::ui {

// 위에서 아래로 자식을 배치한다. Fill 슬롯은 남은 높이를 나눠 갖는다.
class ROCKS_UIFRAMEWORK_API VerticalStack final : public StackPanel {
    ROCKS_UI_ELEMENT(VerticalStack, StackPanel)

public:
    explicit VerticalStack(Allocator& allocator) noexcept;
};

} // namespace rocks::ui
