#include "MakeWidgetElement.h"
#include <UiWidget/RetainerBoxWidget.h>
#include <UiFramework/RetainerBox.h>

namespace rocks::ui {

RetainerBoxWidget::RetainerBoxWidget(Allocator& allocator) noexcept
    : ContentWidget(allocator) {
}

Result<void> RetainerBoxWidget::SetEffectTint(LinearColor tint) noexcept {
    if (!tint.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    effectTint_ = tint;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }
    return Result<void>::Success();
}

Result<SharedPtr<Element>> RetainerBoxWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<RetainerBox>(allocator);
}

Result<void> RetainerBoxWidget::SetPhase(uint32 phase, uint32 count) noexcept {
    if (count == 0 || phase >= count) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    phase_ = phase;
    phaseCount_ = count;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }
    return Result<void>::Success();
}

void RetainerBoxWidget::RequestRender() noexcept {
    if (auto live = CachedElement()) {
        static_cast<RetainerBox&>(*live).RequestRender();
    }
}

void RetainerBoxWidget::ApplyProperties(Element& element) noexcept {
    auto& native = static_cast<RetainerBox&>(element);
    (void)native.SetEffectTint(effectTint_);
    (void)native.SetPhase(phase_, phaseCount_);
}

} // namespace rocks::ui
