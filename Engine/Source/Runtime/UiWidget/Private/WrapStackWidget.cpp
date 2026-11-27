#include "MakeWidgetElement.h"
#include <UiWidget/WrapStackWidget.h>
#include <UiWidget/WrapStackWidgetSlot.generated.h>

namespace rocks::ui {

WrapStackWidget::WrapStackWidget(Allocator& allocator) noexcept : PanelWidget(allocator) {
}

TypeId WrapStackWidget::SlotType() const noexcept {
    return GeneratedType<WrapStackWidgetSlot>::Id();
}

Result<ObjectHandle> WrapStackWidget::AddAt(ObjectRegistry& objects, ObjectHandle child,
                                            const WrapSlot& layout) noexcept {
    return AddChildWithSlot(objects, child,
                            ConfigureSlot::CreateLambda(
                                [&layout](ObjectRegistry& registry, ObjectHandle handle) noexcept {
                                    auto slot = registry.ResolveForWrite<WrapStackWidgetSlot>(
                                        registry.OwnerAccess(), handle);
                                    return slot ? slot.Value().Get().Value()->Configure(layout)
                                                : Result<void>::Failure(slot.Error());
                                }));
}

Result<void> WrapStackWidget::AttachChild(ObjectRegistry& objects, ObjectHandle handle,
                                          Element& panel,
                                          const SharedPtr<Element>& child) noexcept {
    auto slot = objects.Resolve<WrapStackWidgetSlot>(objects.OwnerAccess(), handle);
    if (!slot) {
        return slot.Error() == ErrorCode::TypeMismatch
                   ? PanelWidget::AttachChild(objects, handle, panel, child)
                   : Result<void>::Failure(slot.Error());
    }

    return StaticElementCast<WrapStack>(panel).TryAddSlot(child,
                                                          slot.Value().Get().Value()->WrapLayout());
}

Result<void> WrapStackWidget::UpdateAlignedSlot(ObjectRegistry& objects, ObjectHandle handle,
                                                Element& panel, Element& child,
                                                const SlotPlacement& placement) noexcept {
    auto slot = objects.Resolve<WrapStackWidgetSlot>(objects.OwnerAccess(), handle);
    if (!slot) {
        return slot.Error() == ErrorCode::TypeMismatch
                   ? PanelWidget::UpdateAlignedSlot(objects, handle, panel, child, placement)
                   : Result<void>::Failure(slot.Error());
    }

    auto layout = slot.Value().Get().Value()->WrapLayout();
    layout.placement = placement;
    return StaticElementCast<WrapStack>(panel).SetSlot(child, layout);
}

Result<SharedPtr<Element>> WrapStackWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<WrapStack>(allocator);
}

Result<void> WrapStackWidget::SetSpacing(Vec2 value) noexcept {
    if (!value.IsFinite() || value.x < 0 || value.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    spacing_ = value;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }

    return Result<void>::Success();
}

Vec2 WrapStackWidget::Spacing() const noexcept {
    return spacing_;
}

void WrapStackWidget::ApplyProperties(Element& element) noexcept {
    auto& native = StaticElementCast<WrapStack>(element);
    (void)native.SetSpacing(spacing_);
    native.SetUseAllottedSize(true);
}

} // namespace rocks::ui
