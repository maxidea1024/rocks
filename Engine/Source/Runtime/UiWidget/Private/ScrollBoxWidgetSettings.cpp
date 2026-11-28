#include <UiWidget/ScrollBoxWidget.generated.h>
#include <UiFramework/Scrolling/ScrollView.h>

namespace rocks::ui {

Result<SharedPtr<Element>> ScrollBoxWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto element = PanelWidget::BuildElement(objects);
    if (!element) {
        return element;
    }

    auto* scroll = StaticElementCast<ScrollView>(element.Value().Get());
    scroll->SetOnUserScrolled(Delegate<void(float)>::CreateLambda(
        [&objects, id = Handle(), scroll](float offset) noexcept {
            auto ref = objects.ResolveForWrite<ScrollBoxWidget>(objects.OwnerAccess(), id);
            if (ref) {
                auto* widget = ref.Value().Get().Value();
                if (widget->CachedElement().Get() == scroll) {
                    widget->initialOffset_ = offset;
                    widget->scrolled_.Broadcast({objects, objects.OwnerAccess(), id}, offset);
                }
            }
        }));
    if (endRequested_) {
        scroll->ScrollToEnd();
        endRequested_ = false;
    }

    return element;
}

void ScrollBoxWidget::ApplyProperties(Element& element) noexcept {
    auto& view = StaticElementCast<ScrollView>(element);
    const auto direction = horizontal_ ? StackDirection::Horizontal : StackDirection::Vertical;
    view.SetDirection(direction);
    StaticElementCast<StackPanel>(ChildHost(view)).SetDirection(direction);
    view.SetWheelPolicy(static_cast<ConsumeMouseWheel>(wheelPolicy_));
    (void)view.SetWheelMultiplier(wheelMultiplier_);
    (void)view.SetScrollbarThickness(thickness_);
    view.SetShowScrollbar(showBar_);
    view.SetScrollBarVisibility(static_cast<ScrollBarVisibility>(visibility_));
    if (placement_ != 0) {
        view.SetScrollBarPlacement(static_cast<ScrollBarPlacement>(placement_ - 1));
    } else {
        view.ResetScrollBarPlacement();
    }

    (void)view.SetScrollbarPadding(barPadding_);
    view.SetAlwaysShowTrack(alwaysShowTrack_);
    view.SetScrollPadding(padBefore_, padAfter_);
    view.SetTouchScrolling(touchScrolling_);
    view.SetRightDragScrolling(rightDrag_);
}

void ScrollBoxWidget::ScrollToEnd() noexcept {
    if (auto live = CachedElement()) {
        StaticElementCast<ScrollView>(*live).ScrollToEnd();
    } else {
        endRequested_ = true;
    }
}

Result<void> ScrollBoxWidget::ScrollToStart() noexcept {
    return SetScrollOffset(0);
}

Result<void> ScrollBoxWidget::ScrollWidgetIntoView(const Widget& child,
                                                   ScrollIntoViewAlignment alignment,
                                                   float padding) noexcept {
    auto view = CachedElement(), entry = child.CachedElement();
    if (!view || !entry) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    return StaticElementCast<ScrollView>(*view).ScrollIntoView(*entry, alignment, padding);
}

float ScrollBoxWidget::GetViewFraction() const noexcept {
    auto view = CachedElement();
    return view ? StaticElementCast<ScrollView>(*view).GetViewFraction() : 1;
}

float ScrollBoxWidget::GetViewOffsetFraction() const noexcept {
    auto view = CachedElement();
    return view ? StaticElementCast<ScrollView>(*view).GetViewOffsetFraction() : 0;
}

float ScrollBoxWidget::GetScrollOffsetOfEnd() const noexcept {
    auto view = CachedElement();
    return view ? StaticElementCast<ScrollView>(*view).GetMaxScrollOffset() : 0;
}

bool ScrollBoxWidget::IsScrolling() const noexcept {
    auto view = CachedElement();
    return view ? StaticElementCast<ScrollView>(*view).IsScrolling() : false;
}

Result<void> ScrollBoxWidget::SetOrientation(StackDirection value) noexcept {
    if (value > StackDirection::Vertical) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    horizontal_ = value == StackDirection::Horizontal;
    if (auto view = CachedElement()) {
        ApplyProperties(*view);
    }

    return Result<void>::Success();
}

Result<void> ScrollBoxWidget::SetConsumeMouseWheel(ConsumeMouseWheel value) noexcept {
    if (value > ConsumeMouseWheel::Never) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    wheelPolicy_ = static_cast<uint8>(value);
    if (auto view = CachedElement()) {
        ApplyProperties(*view);
    }

    return Result<void>::Success();
}

Result<void> ScrollBoxWidget::SetWheelScrollMultiplier(float value) noexcept {
    if (!math::IsFinite(value) || value < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    wheelMultiplier_ = value;
    if (auto view = CachedElement()) {
        ApplyProperties(*view);
    }

    return Result<void>::Success();
}

Result<void> ScrollBoxWidget::SetScrollbarThickness(float value) noexcept {
    if (!math::IsFinite(value) || value < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    thickness_ = value;
    if (auto view = CachedElement()) {
        ApplyProperties(*view);
    }

    return Result<void>::Success();
}

void ScrollBoxWidget::SetShowScrollbar(bool visible) noexcept {
    showBar_ = visible;
    if (auto view = CachedElement()) {
        ApplyProperties(*view);
    }
}

void ScrollBoxWidget::SetScrollBarVisibility(ScrollBarVisibility visibility) noexcept {
    visibility_ = static_cast<uint8>(visibility);
    if (auto view = CachedElement()) {
        ApplyProperties(*view);
    }
}

ScrollBarVisibility ScrollBoxWidget::GetScrollBarVisibility() const noexcept {
    return static_cast<ScrollBarVisibility>(visibility_);
}

void ScrollBoxWidget::SetScrollBarPlacement(ScrollBarPlacement placement) noexcept {
    placement_ = static_cast<uint8>(static_cast<uint8>(placement) + 1);
    if (auto view = CachedElement()) {
        ApplyProperties(*view);
    }
}

void ScrollBoxWidget::UseDefaultScrollBarPlacement() noexcept {
    placement_ = 0;
    if (auto view = CachedElement()) {
        ApplyProperties(*view);
    }
}

Result<void> ScrollBoxWidget::SetScrollbarPadding(Vec4 padding) noexcept {
    if (!padding.IsFinite() || padding.x < 0 || padding.y < 0 || padding.z < 0 || padding.w < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto live = CachedElement()) {
        auto set = StaticElementCast<ScrollView>(*live).SetScrollbarPadding(padding);
        if (!set) {
            return set;
        }
    }

    barPadding_ = padding;
    return Result<void>::Success();
}

Vec4 ScrollBoxWidget::GetScrollbarPadding() const noexcept {
    return barPadding_;
}

void ScrollBoxWidget::SetAlwaysShowScrollbarTrack(bool show) noexcept {
    alwaysShowTrack_ = show;
    if (auto live = CachedElement()) {
        StaticElementCast<ScrollView>(*live).SetAlwaysShowTrack(show);
    }
}

bool ScrollBoxWidget::IsAlwaysShowScrollbarTrack() const noexcept {
    return alwaysShowTrack_;
}

void ScrollBoxWidget::SetScrollPadding(bool before, bool after) noexcept {
    padBefore_ = before;
    padAfter_ = after;
    if (auto live = CachedElement()) {
        StaticElementCast<ScrollView>(*live).SetScrollPadding(before, after);
    }
}

void ScrollBoxWidget::SetTouchScrolling(bool enabled) noexcept {
    touchScrolling_ = enabled;
    if (auto view = CachedElement()) {
        ApplyProperties(*view);
    }
}

void ScrollBoxWidget::SetRightDragScrolling(bool enabled) noexcept {
    rightDrag_ = enabled;
    if (auto view = CachedElement()) {
        ApplyProperties(*view);
    }
}

Event<float>& ScrollBoxWidget::OnUserScrolled() noexcept {
    return scrolled_;
}
} // namespace rocks::ui
