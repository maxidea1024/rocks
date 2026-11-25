#include "MakeWidgetElement.h"
#include <UiWidget/CircularThrobberWidget.h>
#include <UiFramework/ActivityIndicator.h>

namespace rocks::ui {

Result<SharedPtr<Element>> CircularThrobberWidget::BuildElement(ObjectRegistry& objects) noexcept {
    return MakeWidgetElement<ActivityIndicator>(objects.ObjectAllocator(), true);
}

Result<void> CircularThrobberWidget::SetPieceCount(uint32 count) noexcept {
    if (count == 0 || count > 64) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    pieceCount_ = count;
    if (auto live = CachedElement()) {
        return static_cast<ActivityIndicator&>(*live).SetPieceCount(count);
    }

    return Result<void>::Success();
}

void CircularThrobberWidget::SetAnimating(bool animate) noexcept {
    animating_ = animate;
    if (auto live = CachedElement()) {
        static_cast<ActivityIndicator&>(*live).SetAnimating(animate);
    }
}

Result<void> CircularThrobberWidget::SetPieceBrush(const Brush& brush) noexcept {
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

Result<void> CircularThrobberWidget::SetRadius(float radius) noexcept {
    if (!math::IsFinite(radius) || radius < 0 || radius > NumericLimits<float>::Max() * 0.5f) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    radius_ = radius;
    if (auto live = CachedElement()) {
        return static_cast<ActivityIndicator&>(*live).SetRadius(radius);
    }

    return Result<void>::Success();
}

Result<void> CircularThrobberWidget::SetPeriod(float seconds) noexcept {
    if (!math::IsFinite(seconds) || seconds <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    period_ = seconds;
    if (auto live = CachedElement()) {
        return static_cast<ActivityIndicator&>(*live).SetPeriod(seconds);
    }

    return Result<void>::Success();
}

void CircularThrobberWidget::ApplyProperties(Element& element) noexcept {
    auto& indicator = static_cast<ActivityIndicator&>(element);
    (void)indicator.SetPieceCount(pieceCount_);
    auto brush = brush_;
    brush.imageSize = pieceSize_;
    brush.tint = pieceTint_;
    (void)indicator.SetPieceBrush(brush);
    (void)indicator.SetRadius(radius_);
    (void)indicator.SetPeriod(period_);
    indicator.SetAnimating(animating_);
}

} // namespace rocks::ui
