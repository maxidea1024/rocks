#include <UiWidget/HorizontalBoxWidgetSlot.generated.h>
#include "MakeWidgetElement.h"
#include <UiFramework/HorizontalStack.h>
#include <UiWidget/HorizontalBoxWidget.h>
#include <UiFramework/StackPanel.h>

namespace rocks::ui {

HorizontalBoxWidget::HorizontalBoxWidget(Allocator& allocator) noexcept
    : StackPanelWidget(allocator) {
}

TypeId HorizontalBoxWidget::SlotType() const noexcept {
    return GeneratedType<HorizontalBoxWidgetSlot>::Id();
}

Result<SharedPtr<Element>> HorizontalBoxWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<HorizontalStack>(allocator);
}

} // namespace rocks::ui
