#include <UiWidget/VerticalBoxWidgetSlot.generated.h>
#include "MakeWidgetElement.h"
#include <UiFramework/VerticalStack.h>
#include <UiWidget/VerticalBoxWidget.h>
#include <UiFramework/StackPanel.h>

namespace rocks::ui {

VerticalBoxWidget::VerticalBoxWidget(Allocator& allocator) noexcept : StackPanelWidget(allocator) {
}

TypeId VerticalBoxWidget::SlotType() const noexcept {
    return GeneratedType<VerticalBoxWidgetSlot>::Id();
}

Result<SharedPtr<Element>> VerticalBoxWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<VerticalStack>(allocator);
}

} // namespace rocks::ui
