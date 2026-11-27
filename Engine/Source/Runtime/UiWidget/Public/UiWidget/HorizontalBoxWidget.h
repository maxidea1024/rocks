#pragma once

#include <UiWidget/StackPanelWidget.h>

namespace rocks::ui {

// 자식을 왼쪽에서 오른쪽으로 배치한다. 아이콘과 수치를 한 줄에 표시할 때 쓴다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API HorizontalBoxWidget final : public StackPanelWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit HorizontalBoxWidget(Allocator& allocator) noexcept;

protected:
    TypeId SlotType() const noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
};

} // namespace rocks::ui
