#include "MakeWidgetElement.h"
#include <UiWidget/WrapBoxWidget.h>
#include <UiWidget/WrapBoxWidgetSlot.generated.h>

namespace rocks::ui {

WrapBoxWidget::WrapBoxWidget(Allocator& allocator) noexcept : PanelWidget(allocator) {
}

TypeId WrapBoxWidget::SlotType() const noexcept {
    return GeneratedType<WrapBoxWidgetSlot>::Id();
}

Result<ObjectHandle> WrapBoxWidget::AddAt(ObjectRegistry& objects, ObjectHandle child,
                                          const WrapSlot& layout) noexcept {
    return AddChildWithSlot(objects, child,
                            ConfigureSlot::CreateLambda(
                                [&layout](ObjectRegistry& registry, ObjectHandle handle) noexcept {
                                    auto slot = registry.ResolveForWrite<WrapBoxWidgetSlot>(
                                        registry.OwnerAccess(), handle);
                                    return slot ? slot.Value().Get().Value()->Configure(layout)
                                                : Result<void>::Failure(slot.Error());
                                }));
}

Result<void> WrapBoxWidget::AttachChild(ObjectRegistry& objects, ObjectHandle handle,
                                        Element& panel, const SharedPtr<Element>& child) noexcept {
    auto slot = objects.Resolve<WrapBoxWidgetSlot>(objects.OwnerAccess(), handle);
    if (!slot) {
        return slot.Error() == ErrorCode::TypeMismatch
                   ? PanelWidget::AttachChild(objects, handle, panel, child)
                   : Result<void>::Failure(slot.Error());
    }
    return static_cast<WrapStack&>(panel).TryAddSlot(child,
                                                     slot.Value().Get().Value()->WrapLayout());
}

Result<void> WrapBoxWidget::UpdateAlignedSlot(ObjectRegistry& objects, ObjectHandle handle,
                                              Element& panel, Element& child,
                                              const SlotPlacement& placement) noexcept {
    auto slot = objects.Resolve<WrapBoxWidgetSlot>(objects.OwnerAccess(), handle);
    if (!slot) {
        return slot.Error() == ErrorCode::TypeMismatch
                   ? PanelWidget::UpdateAlignedSlot(objects, handle, panel, child, placement)
                   : Result<void>::Failure(slot.Error());
    }
    auto layout = slot.Value().Get().Value()->WrapLayout();
    layout.placement = placement;
    return static_cast<WrapStack&>(panel).SetSlot(child, layout);
}

Result<SharedPtr<Element>> WrapBoxWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<WrapStack>(allocator);
}

Result<void> WrapBoxWidget::SetSpacing(Vec2 value) noexcept {
    if (!value.IsFinite() || value.x < 0 || value.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    spacing_ = value;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }
    return Result<void>::Success();
}

Vec2 WrapBoxWidget::Spacing() const noexcept {
    return spacing_;
}

void WrapBoxWidget::ApplyProperties(Element& element) noexcept {
    auto& native = static_cast<WrapStack&>(element);
    (void)native.SetSpacing(spacing_);
    native.SetUseAllottedSize(true);
}

} // namespace rocks::ui
