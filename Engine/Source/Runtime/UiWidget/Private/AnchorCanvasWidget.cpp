#include "MakeWidgetElement.h"
#include <UiWidget/AnchorCanvasWidget.h>
#include <UiWidget/AnchorCanvasWidgetSlot.generated.h>

namespace rocks::ui {

AnchorCanvasWidget::AnchorCanvasWidget(Allocator& allocator) noexcept : PanelWidget(allocator) {
}

TypeId AnchorCanvasWidget::SlotType() const noexcept {
    return GeneratedType<AnchorCanvasWidgetSlot>::Id();
}

Result<SharedPtr<Element>> AnchorCanvasWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<AnchorCanvas>(allocator);
}

Result<ObjectHandle> AnchorCanvasWidget::AddAt(ObjectRegistry& objects, ObjectHandle child,
                                               const CanvasSlot& layout) noexcept {
    return AddChildWithSlot(objects, child,
                            ConfigureSlot::CreateLambda(
                                [&layout](ObjectRegistry& registry, ObjectHandle handle) noexcept {
                                    auto slot = registry.ResolveForWrite<AnchorCanvasWidgetSlot>(
                                        registry.OwnerAccess(), handle);
                                    return slot ? slot.Value().Get().Value()->Configure(layout)
                                                : Result<void>::Failure(slot.Error());
                                }));
}

Result<void> AnchorCanvasWidget::AttachChild(ObjectRegistry& objects, ObjectHandle handle,
                                             Element& panel,
                                             const SharedPtr<Element>& child) noexcept {
    auto slot = objects.Resolve<AnchorCanvasWidgetSlot>(objects.OwnerAccess(), handle);
    if (!slot) {
        return Result<void>::Failure(slot.Error());
    }

    const auto layout = slot.Value().Get().Value()->Layout();
    return StaticElementCast<AnchorCanvas>(panel).TryAddSlot(child, layout);
}

} // namespace rocks::ui
