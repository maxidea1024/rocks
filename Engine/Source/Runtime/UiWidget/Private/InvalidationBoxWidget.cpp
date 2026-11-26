#include "MakeWidgetElement.h"
#include <UiWidget/InvalidationBoxWidget.h>
#include <UiFramework/Effects/CachePanel.h>

namespace rocks::ui {

InvalidationBoxWidget::InvalidationBoxWidget(Allocator& allocator) noexcept
    : ContentWidget(allocator) {
}

Result<SharedPtr<Element>> InvalidationBoxWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<CachePanel>(allocator);
}

} // namespace rocks::ui
