#include <UiWidget/StackWidgetSlot.h>
#include <UiWidget/StackPanelWidget.generated.h>
#include <UiWidget/Widget.generated.h>
#include <UiFramework/StackPanel.h>

namespace rocks::ui {
Result<void> StackWidgetSlot::Configure(const StackSlot& layout) noexcept {
    StackPlacement placement;
    auto valid = ArrangeStack({&layout, 1}, {&placement, 1}, 0);
    if (!valid || layout.alignment > StackAlignment::End) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    rule_ = static_cast<uint8>(layout.rule);
    weight_ = layout.weight;
    minSize_ = layout.minSize;
    maxSize_ = layout.maxSize;
    padding_ = {layout.before, layout.after, layout.crossBefore, layout.crossAfter};
    alignment_ = static_cast<uint8>(layout.alignment);
    mainAlignment_ = static_cast<uint8>(layout.mainAlignment);
    shrinkWeight_ = layout.shrinkWeight;
    return Result<void>::Success();
}

Result<void> StackWidgetSlot::SetLayout(ObjectRegistry& objects, const StackSlot& layout) noexcept {
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
    auto panel = objects.Resolve<StackPanelWidget>(access, owner.Value());
    if (!panel) {
        return Result<void>::Failure(panel.Error());
    }
    if (auto native = panel.Value().Get().Value()->CachedElement()) {
        auto child = widget->CachedElement();
        if (!child) {
            return Result<void>::Failure(ErrorCode::NotReady);
        }
        auto updated = static_cast<StackPanel&>(*native).SetSlotLayout(*child, layout);
        if (!updated) {
            return updated;
        }
    }
    return Configure(layout);
}

StackSlot StackWidgetSlot::Layout() const noexcept {
    StackSlot layout;
    layout.rule = static_cast<StackSizeRule>(rule_);
    layout.weight = weight_;
    layout.minSize = minSize_;
    layout.maxSize = maxSize_;
    layout.before = padding_.x;
    layout.after = padding_.y;
    layout.crossBefore = padding_.z;
    layout.crossAfter = padding_.w;
    layout.alignment = static_cast<StackAlignment>(alignment_);
    layout.mainAlignment = static_cast<StackAlignment>(mainAlignment_);
    layout.shrinkWeight = shrinkWeight_;
    return layout;
}
} // namespace rocks::ui
