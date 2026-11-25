#include "MakeWidgetElement.h"
#include <UiWidget/ThrobberWidget.h>
#include <UiFramework/ActivityIndicator.h>

namespace rocks::ui {

Result<SharedPtr<Element>> ThrobberWidget::BuildElement(ObjectRegistry& objects) noexcept {
    return MakeWidgetElement<ActivityIndicator>(objects.ObjectAllocator(), false);
}

Result<void> ThrobberWidget::SetPieceCount(uint32 count) noexcept {
    if (count == 0 || count > 64) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    pieceCount_ = count;
    if (auto live = CachedElement()) {
        return static_cast<ActivityIndicator&>(*live).SetPieceCount(count);
    }

    return Result<void>::Success();
}

void ThrobberWidget::SetAnimating(bool animate) noexcept {
    animating_ = animate;
    if (auto live = CachedElement()) {
        static_cast<ActivityIndicator&>(*live).SetAnimating(animate);
    }
}

Result<void> ThrobberWidget::SetPieceBrush(const Brush& brush) noexcept {
    if (!brush.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    brush_ = brush;
    pieceSize_ = brush.imageSize;
    pieceTint_ = brush.tint;
    if (auto live = CachedElement()) {
        return static_cast<ActivityIndicator&>(*live).SetPieceBrush(brush);
    }

    return Result<void>::Success();
}

void ThrobberWidget::SetAnimationAxes(bool horizontal, bool vertical, bool opacity) noexcept {
    animateHorizontally_ = horizontal;
    animateVertically_ = vertical;
    animateOpacity_ = opacity;
    if (auto live = CachedElement()) {
        static_cast<ActivityIndicator&>(*live).SetAnimationAxes(horizontal, vertical, opacity);
    }
}

void ThrobberWidget::ApplyProperties(Element& element) noexcept {
    auto& indicator = static_cast<ActivityIndicator&>(element);
    (void)indicator.SetPieceCount(pieceCount_);
    auto brush = brush_;
    brush.imageSize = pieceSize_;
    brush.tint = pieceTint_;
    (void)indicator.SetPieceBrush(brush);
    indicator.SetAnimationAxes(animateHorizontally_, animateVertically_, animateOpacity_);
    indicator.SetAnimating(animating_);
}

} // namespace rocks::ui
