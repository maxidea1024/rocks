#include <UiWidget/StackBoxWidgetSlot.generated.h>
#include <UiWidget/StackBoxWidget.h>
#include "MakeWidgetElement.h"
#include <UiFramework/Layout/StackPanel.h>

namespace rocks::ui {

StackBoxWidget::StackBoxWidget(Allocator& allocator) noexcept : StackPanelWidget(allocator) {
}

TypeId StackBoxWidget::SlotType() const noexcept {
    return GeneratedType<StackBoxWidgetSlot>::Id();
}

Result<SharedPtr<Element>> StackBoxWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<StackPanel>(allocator, vertical_ ? StackDirection::Vertical
                                                              : StackDirection::Horizontal);
}

void StackBoxWidget::SetVertical(bool vertical) noexcept {
    vertical_ = vertical;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }
}

void StackBoxWidget::ApplyProperties(Element& element) noexcept {
    StaticElementCast<StackPanel>(element).SetDirection(vertical_ ? StackDirection::Vertical
                                                                  : StackDirection::Horizontal);
}

} // namespace rocks::ui
