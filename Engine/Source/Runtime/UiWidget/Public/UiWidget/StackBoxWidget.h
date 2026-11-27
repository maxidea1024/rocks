#pragma once

#include <UiWidget/StackPanelWidget.h>

namespace rocks::ui {

// 행과 열 사이를 전환하는 패널이다. SetVertical로 방향을 바꾸면 자식 인스턴스를 유지하고 다시
// 배치한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API StackBoxWidget final : public StackPanelWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit StackBoxWidget(Allocator& allocator) noexcept;
    void SetVertical(bool vertical) noexcept;

protected:
    TypeId SlotType() const noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(Name = "vertical") bool vertical_ = false;
};

} // namespace rocks::ui
