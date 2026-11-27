#include <UiWidget/ScaleLayerWidgetSlot.generated.h>
#include "MakeWidgetElement.h"
#include <UiWidget/ScaleLayerWidget.h>
#include <UiFramework/Layout/FitBox.h>

namespace rocks::ui {

TypeId ScaleLayerWidget::SlotType() const noexcept {
    return GeneratedType<ScaleLayerWidgetSlot>::Id();
}

ScaleLayerWidget::ScaleLayerWidget(Allocator& allocator) noexcept : ContentWidget(allocator) {
}

Result<SharedPtr<Element>> ScaleLayerWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<FitBox>(allocator);
}

Result<void> ScaleLayerWidget::SetAlignment(Vec2 value) noexcept {
    if (!value.IsFinite() || value.x < 0 || value.y < 0 || value.x > 1 || value.y > 1) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    alignment_ = value;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }

    return Result<void>::Success();
}

Result<void> ScaleLayerWidget::SetFit(FitMode mode, FitDirection direction) noexcept {
    if (mode > FitMode::User || direction > FitDirection::GrowOnly) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    fitMode_ = static_cast<uint8>(mode);
    fitDirection_ = static_cast<uint8>(direction);
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }

    return Result<void>::Success();
}

Result<void> ScaleLayerWidget::SetUserScale(float value) noexcept {
    if (!math::IsFinite(value) || value < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    userScale_ = value;
    if (auto live = CachedElement()) {
        return StaticElementCast<FitBox>(*live).SetUserScale(value);
    }

    return Result<void>::Success();
}

Vec2 ScaleLayerWidget::Alignment() const noexcept {
    return alignment_;
}

void ScaleLayerWidget::ApplyProperties(Element& element) noexcept {
    auto& native = StaticElementCast<FitBox>(element);
    native.SetFit(static_cast<FitMode>(fitMode_), static_cast<FitDirection>(fitDirection_));
    (void)native.SetUserScale(userScale_);
    (void)native.SetAlignment(alignment_);
}

} // namespace rocks::ui
