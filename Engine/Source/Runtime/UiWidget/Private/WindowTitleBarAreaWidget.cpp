#include <UiWidget/WindowTitleBarAreaWidgetSlot.generated.h>
#include "MakeWidgetElement.h"
#include <UiWidget/WindowTitleBarAreaWidget.h>
#include <UiFramework/WindowTitleBarArea.h>
namespace rocks::ui {

TypeId WindowTitleBarAreaWidget::SlotType() const noexcept {
    return GeneratedType<WindowTitleBarAreaWidgetSlot>::Id();
}
WindowTitleBarAreaWidget::WindowTitleBarAreaWidget(Allocator& allocator) noexcept
    : ContentWidget(allocator) {
}
Result<SharedPtr<Element>> WindowTitleBarAreaWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<WindowTitleBarArea>(allocator);
}
void WindowTitleBarAreaWidget::ApplyProperties(Element& element) noexcept {
    static_cast<WindowTitleBarArea&>(element).SetDoubleClickTogglesFullscreen(fullscreen_);
}
void WindowTitleBarAreaWidget::SetDoubleClickTogglesFullscreen(bool enabled) noexcept {
    fullscreen_ = enabled;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }
}
} // namespace rocks::ui
