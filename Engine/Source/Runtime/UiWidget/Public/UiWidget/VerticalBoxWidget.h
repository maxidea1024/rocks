#pragma once

#include <UiWidget/StackPanelWidget.h>

namespace rocks::ui {

// 자식의 원하는 높이에 따라 위에서 아래로 쌓는다. 메뉴나 설정 항목의 순서는 추가 순서다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API VerticalBoxWidget final : public StackPanelWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit VerticalBoxWidget(Allocator& allocator) noexcept;

protected:
    TypeId SlotType() const noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
};

} // namespace rocks::ui
