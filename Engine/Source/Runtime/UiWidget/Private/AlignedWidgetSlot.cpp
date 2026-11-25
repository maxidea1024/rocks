#include <UiWidget/AlignedWidgetSlot.h>
#include <UiWidget/PanelWidget.generated.h>
#include <UiWidget/Widget.generated.h>

namespace rocks::ui {

Result<void> AlignedWidgetSlot::SetLayout(ObjectRegistry& objects,
                                          const SlotPlacement& layout) noexcept {
    if (!layout.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const auto access = objects.OwnerAccess();
    auto content = objects.Resolve<Widget>(access, Content());
    if (!content) {
        return Result<void>::Failure(content.Error());
    }

    auto* widget = content.Value().Get().Value();
    if (widget->Slot() != Handle()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    auto owner = objects.GetOuter(access, Handle());
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }

    auto panel = objects.ResolveForWrite<PanelWidget>(access, owner.Value());
    if (!panel) {
        return Result<void>::Failure(panel.Error());
    }

    auto* host = panel.Value().Get().Value();
    if (auto native = host->CachedElement()) {
        auto child = widget->CachedElement();
        if (!child) {
            return Result<void>::Failure(ErrorCode::NotReady);
        }

        auto updated = host->UpdateAlignedSlot(objects, Handle(), *native, *child, layout);
        if (!updated) {
            return updated;
        }
    }

    return SetLayout(layout);
}

SlotPlacement AlignedWidgetSlot::Layout() const noexcept {
    return {padding_, static_cast<StackAlignment>(horizontal_),
            static_cast<StackAlignment>(vertical_)};
}

Result<void> AlignedWidgetSlot::SetLayout(const SlotPlacement& layout) noexcept {
    if (!layout.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    padding_ = layout.padding;
    horizontal_ = static_cast<uint8>(layout.horizontal);
    vertical_ = static_cast<uint8>(layout.vertical);
    return Result<void>::Success();
}

Result<void> AlignedWidgetSlot::SetPadding(ObjectRegistry& objects, Vec4 value) noexcept {
    auto layout = Layout();
    layout.padding = value;
    return SetLayout(objects, layout);
}

Result<void> AlignedWidgetSlot::SetHorizontalAlignment(ObjectRegistry& objects,
                                                       StackAlignment value) noexcept {
    auto layout = Layout();
    layout.horizontal = value;
    return SetLayout(objects, layout);
}

Result<void> AlignedWidgetSlot::SetVerticalAlignment(ObjectRegistry& objects,
                                                     StackAlignment value) noexcept {
    auto layout = Layout();
    layout.vertical = value;
    return SetLayout(objects, layout);
}

} // namespace rocks::ui
