#include "MakeWidgetElement.h"
#include <UiWidget/OffscreenLayerWidget.h>
#include <UiFramework/Effects/OffscreenLayer.h>

namespace rocks::ui {

OffscreenLayerWidget::OffscreenLayerWidget(Allocator& allocator) noexcept
    : ContentWidget(allocator) {
}

Result<void> OffscreenLayerWidget::SetEffectTint(LinearColor tint) noexcept {
    if (!tint.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    effectTint_ = tint;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }

    return Result<void>::Success();
}

Result<SharedPtr<Element>> OffscreenLayerWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<OffscreenLayer>(allocator);
}

Result<void> OffscreenLayerWidget::SetPhase(uint32 phase, uint32 count) noexcept {
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

void OffscreenLayerWidget::RequestRender() noexcept {
    if (auto live = CachedElement()) {
        StaticElementCast<OffscreenLayer>(*live).RequestRender();
    }
}

void OffscreenLayerWidget::ApplyProperties(Element& element) noexcept {
    auto& native = StaticElementCast<OffscreenLayer>(element);
    (void)native.SetEffectTint(effectTint_);
    (void)native.SetPhase(phase_, phaseCount_);
}

} // namespace rocks::ui
