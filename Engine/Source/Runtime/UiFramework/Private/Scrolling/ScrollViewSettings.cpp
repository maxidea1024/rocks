#include <UiFramework/Scrolling/ScrollView.h>

namespace rocks::ui {

void ScrollView::SetDirection(StackDirection direction) noexcept {
    if (direction_ != direction) {
        direction_ = direction;
        arranged_ = false;
        dragging_ = false;
        InvalidateLayout();
    }
}

void ScrollView::SetWheelPolicy(ConsumeMouseWheel policy) noexcept {
    wheelPolicy_ = policy;
}

Result<void> ScrollView::SetWheelMultiplier(float value) noexcept {
    if (!math::IsFinite(value) || value < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    wheelMultiplier_ = value;
    return Result<void>::Success();
}

Result<void> ScrollView::SetScrollbarThickness(float value) noexcept {
    if (!math::IsFinite(value) || value < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (thickness_ != value) {
        thickness_ = value;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

void ScrollView::SetShowScrollbar(bool visible, bool alwaysShow) noexcept {
    if (showBar_ != visible || alwaysShowBar_ != alwaysShow) {
        showBar_ = visible;
        alwaysShowBar_ = alwaysShow;
        InvalidatePaint();
    }
}

Result<void> ScrollView::SetScrollbarPadding(Vec4 value) noexcept {
    if (!value.IsFinite() || value.x < 0 || value.y < 0 || value.z < 0 || value.w < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (barPadding_ != value) {
        barPadding_ = value;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

void ScrollView::SetAlwaysShowTrack(bool show) noexcept {
    if (alwaysShowTrack_ != show) {
        alwaysShowTrack_ = show;
        InvalidatePaint();
    }
}

void ScrollView::SetScrollPadding(bool before, bool after) noexcept {
    if (padBefore_ != before || padAfter_ != after) {
        padBefore_ = before;
        padAfter_ = after;
        InvalidateArrangement();
    }
}

void ScrollView::SetTouchScrolling(bool enabled) noexcept {
    touchScrolling_ = enabled;
}

void ScrollView::SetRightDragScrolling(bool enabled) noexcept {
    rightDragScrolling_ = enabled;
}

void ScrollView::SetOnUserScrolled(Delegate<void(float)>&& callback) noexcept {
    userScrolled_ = Move(callback);
}

void ScrollView::SetUserOffset(float value) noexcept {
    auto before = offset_;
    if (SetScrollOffset(value) && before != offset_) {
        userScrolled_.ExecuteIfBound(offset_);
    }
}

bool ScrollView::IsScrolling() const noexcept {
    return dragging_;
}

float ScrollView::GetViewFraction() const noexcept {
    const float viewport =
        direction_ == StackDirection::Vertical ? GetGeometry().size.y : GetGeometry().size.x;
    return viewport + maxOffset_ > 0 ? viewport / (viewport + maxOffset_) : 1;
}

float ScrollView::GetViewOffsetFraction() const noexcept {
    const float viewport =
        direction_ == StackDirection::Vertical ? GetGeometry().size.y : GetGeometry().size.x;
    return viewport + maxOffset_ > 0 ? offset_ / (viewport + maxOffset_) : 0;
}

void ScrollView::ScrollToEnd() noexcept {
    endRequested_ = true;
    InvalidateArrangement();
}

Result<void> ScrollView::ScrollIntoView(const Element& child, ScrollIntoViewAlignment alignment,
                                        float padding) noexcept {
    if (!arranged_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    if (&child == this || !child.IsWithin(*this) || alignment > ScrollIntoViewAlignment::End ||
        !math::IsFinite(padding) || padding < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    AffineTransform2D inverse;
    if (!GetGeometry().layout.TryInverse(inverse)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const auto& geometry = child.GetGeometry();
    const auto local = inverse.Compose(geometry.layout);
    const bool vertical = direction_ == StackDirection::Vertical;
    const auto project = [&](Vec2 p) {
        const auto value = local.Apply(p);
        return vertical ? value.y : value.x;
    };
    const float a = project({}), b = project({geometry.size.x, 0}),
                c = project({0, geometry.size.y}), d = project(geometry.size);
    const float first = math::Min(math::Min(a, b), math::Min(c, d)) - padding;
    const float last = math::Max(math::Max(a, b), math::Max(c, d)) + padding;
    const float length = vertical ? GetGeometry().size.y : GetGeometry().size.x;
    float delta = 0;
    switch (alignment) {
    case ScrollIntoViewAlignment::Start:
        delta = first;
        break;
    case ScrollIntoViewAlignment::End:
        delta = last - length;
        break;
    case ScrollIntoViewAlignment::Center:
        delta = (first + last - length) * 0.5f;
        break;
    case ScrollIntoViewAlignment::Nearest:
        if (first < 0 && last > length) {
            return Result<void>::Success();
        }

        delta = first < 0 ? first : math::Max(last - length, 0.0f);
        break;
    }

    return SetScrollOffset(offset_ + delta);
}
} // namespace rocks::ui
