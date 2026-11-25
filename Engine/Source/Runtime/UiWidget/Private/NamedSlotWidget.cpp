#include "MakeWidgetElement.h"
#include <UiWidget/NamedSlotWidget.h>
#include <UiFramework/SizeBox.h>

namespace rocks::ui {

NamedSlotWidget::NamedSlotWidget(Allocator& allocator) noexcept : ContentWidget(allocator) {
}

Result<SharedPtr<Element>> NamedSlotWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<SizeBox>(allocator);
}

} // namespace rocks::ui
