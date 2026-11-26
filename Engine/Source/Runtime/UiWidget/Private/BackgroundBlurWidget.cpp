#include <UiWidget/BackgroundBlurWidgetSlot.generated.h>
#include "MakeWidgetElement.h"
#include <UiWidget/BackgroundBlurWidget.h>
#include <UiFramework/Effects/BackgroundBlur.h>

namespace rocks::ui {

TypeId BackgroundBlurWidget::SlotType() const noexcept {
    return GeneratedType<BackgroundBlurWidgetSlot>::Id();
}

BackgroundBlurWidget::BackgroundBlurWidget(Allocator& allocator) noexcept
    : ContentWidget(allocator) {
}

Result<SharedPtr<Element>> BackgroundBlurWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<BackgroundBlur>(allocator);
}

Result<void> BackgroundBlurWidget::SetStrength(float pixels) noexcept {
    if (!math::IsFinite(pixels) || pixels < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    strength_ = pixels;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }

    return Result<void>::Success();
}

Result<void> BackgroundBlurWidget::SetCornerRadius(Vec4 corners) noexcept {
    if (!corners.IsFinite() || corners.x < 0 || corners.y < 0 || corners.z < 0 || corners.w < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    cornerRadius_ = corners;
    if (auto live = CachedElement()) {
        return static_cast<BackgroundBlur&>(*live).SetCornerRadius(corners);
    }

    return Result<void>::Success();
}

Result<void> BackgroundBlurWidget::SetBlurRadius(int32 kernel) noexcept {
    if (kernel < AutoBlurKernelSize || kernel > MaxBlurKernelSize) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    blurRadius_ = kernel;
    if (auto live = CachedElement()) {
        return static_cast<BackgroundBlur&>(*live).SetBlurRadius(kernel);
    }

    return Result<void>::Success();
}

void BackgroundBlurWidget::SetApplyAlphaToBlur(bool apply) noexcept {
    applyAlphaToBlur_ = apply;
    if (auto live = CachedElement()) {
        static_cast<BackgroundBlur&>(*live).SetApplyAlphaToBlur(apply);
    }
}

void BackgroundBlurWidget::SetLowQuality(bool enabled) noexcept {
    lowQuality_ = enabled;
    if (auto live = CachedElement()) {
        static_cast<BackgroundBlur&>(*live).SetLowQuality(enabled);
    }
}

Result<void> BackgroundBlurWidget::SetLowQualityFallbackBrush(const Brush& brush) noexcept {
    if (!brush.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    fallback_ = brush;
    fallbackTint_ = brush.tint;
    if (auto live = CachedElement()) {
        return static_cast<BackgroundBlur&>(*live).SetLowQualityFallbackBrush(brush);
    }

    return Result<void>::Success();
}

void BackgroundBlurWidget::ApplyProperties(Element& element) noexcept {
    auto& native = static_cast<BackgroundBlur&>(element);
    (void)native.SetStrength(strength_);
    (void)native.SetCornerRadius(cornerRadius_);
    (void)native.SetBlurRadius(blurRadius_);
    native.SetLowQuality(lowQuality_);
    native.SetApplyAlphaToBlur(applyAlphaToBlur_);
    auto fallback = fallback_;
    fallback.tint = fallbackTint_;
    (void)native.SetLowQualityFallbackBrush(fallback);
}

} // namespace rocks::ui
