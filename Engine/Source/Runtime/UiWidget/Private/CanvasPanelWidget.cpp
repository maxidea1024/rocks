#include "MakeWidgetElement.h"
#include <UiWidget/CanvasPanelWidget.h>
#include <UiWidget/CanvasWidgetSlot.generated.h>

namespace rocks::ui {

CanvasPanelWidget::CanvasPanelWidget(Allocator& allocator) noexcept : PanelWidget(allocator) {
}

TypeId CanvasPanelWidget::SlotType() const noexcept {
    return GeneratedType<CanvasWidgetSlot>::Id();
}

Result<SharedPtr<Element>> CanvasPanelWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<AnchorCanvas>(allocator);
}

Result<ObjectHandle> CanvasPanelWidget::AddAt(ObjectRegistry& objects, ObjectHandle child,
                                              const CanvasSlot& layout) noexcept {
    return AddChildWithSlot(objects, child,
                            ConfigureSlot::CreateLambda(
                                [&layout](ObjectRegistry& registry, ObjectHandle handle) noexcept {
                                    auto slot = registry.ResolveForWrite<CanvasWidgetSlot>(
                                        registry.OwnerAccess(), handle);
                                    return slot ? slot.Value().Get().Value()->Configure(layout)
                                                : Result<void>::Failure(slot.Error());
                                }));
}

Result<void> CanvasPanelWidget::AttachChild(ObjectRegistry& objects, ObjectHandle handle,
                                            Element& panel,
                                            const SharedPtr<Element>& child) noexcept {
    auto slot = objects.Resolve<CanvasWidgetSlot>(objects.OwnerAccess(), handle);
    if (!slot) {
        return Result<void>::Failure(slot.Error());
    }
    const auto layout = slot.Value().Get().Value()->Layout();
    return static_cast<AnchorCanvas&>(panel).TryAddSlot(child, layout);
}

} // namespace rocks::ui
