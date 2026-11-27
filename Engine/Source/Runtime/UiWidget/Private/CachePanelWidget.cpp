#include "MakeWidgetElement.h"
#include <UiWidget/CachePanelWidget.h>
#include <UiFramework/Effects/CachePanel.h>

namespace rocks::ui {

CachePanelWidget::CachePanelWidget(Allocator& allocator) noexcept : ContentWidget(allocator) {
}

Result<SharedPtr<Element>> CachePanelWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<CachePanel>(allocator);
}

} // namespace rocks::ui
