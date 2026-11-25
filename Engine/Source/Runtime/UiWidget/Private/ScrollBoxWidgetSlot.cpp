#include <UiWidget/ScrollBoxWidgetSlot.h>
#include <UiWidget/ScrollBoxWidget.generated.h>
#include <UiWidget/Widget.generated.h>

namespace rocks::ui {

StackSizeRule ScrollBoxWidgetSlot::SizeRule() const noexcept {
    return static_cast<StackSizeRule>(sizeRule_);
}

float ScrollBoxWidgetSlot::FillWeight() const noexcept {
    return fillWeight_;
}

Result<void> ScrollBoxWidgetSlot::SetSize(ObjectRegistry& objects, StackSizeRule rule,
                                          float weight) noexcept {
    if (rule > StackSizeRule::FillContent || !math::IsFinite(weight) || weight < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    const auto access = objects.OwnerAccess();
    auto content = objects.Resolve<Widget>(access, Content());
    if (!content) {
        return Result<void>::Failure(content.Error());
    }
    const auto* widget = content.Value().Get().Value();
    if (widget->Slot() != Handle()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }
    auto outer = objects.GetOuter(access, Handle());
    if (!outer) {
        return Result<void>::Failure(outer.Error());
    }
    auto owner = objects.ResolveForWrite<ScrollBoxWidget>(access, outer.Value());
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }
    auto* panel = owner.Value().Get().Value();
    if (auto live = panel->CachedElement()) {
        auto child = widget->CachedElement();
        if (!child) {
            return Result<void>::Failure(ErrorCode::NotReady);
        }
        auto updated = panel->ApplySlotSize(*live, *child, rule, weight);
        if (!updated) {
            return updated;
        }
    }
    sizeRule_ = static_cast<uint8>(rule);
    fillWeight_ = weight;
    return Result<void>::Success();
}

} // namespace rocks::ui
