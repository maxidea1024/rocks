#include <UiWidget/OverlayWidgetSlot.generated.h>
#include <UiWidget/OverlayWidget.generated.h>
#include <UiFramework/Overlay.h>

namespace rocks::ui {

TypeId OverlayWidget::SlotType() const noexcept {
    return GeneratedType<OverlayWidgetSlot>::Id();
}

OverlayWidget::OverlayWidget(Allocator& allocator) noexcept : PanelWidget(allocator) {
}
Result<SharedPtr<Element>> OverlayWidget::CreatePanel(Allocator& allocator) noexcept {
    auto panel = SharedPtr<Overlay>::TryMake(allocator, allocator);
    if (!panel) {
        return Result<SharedPtr<Element>>::Failure(panel.Error());
    }
    return Result<SharedPtr<Element>>::Success(Move(panel.Value()));
}

} // namespace rocks::ui
