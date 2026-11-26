#include <UiWidget/ScrollBoxWidget.generated.h>
#include <UiFramework/Scrolling/ScrollView.h>

namespace rocks::ui {

Result<SharedPtr<Element>> ScrollBoxWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto element = PanelWidget::BuildElement(objects);
    if (!element) {
        return element;
    }

    auto* scroll = static_cast<ScrollView*>(element.Value().Get());
    scroll->SetOnUserScrolled(Delegate<void(float)>::CreateLambda(
        [&objects, id = Handle(), scroll](float offset) noexcept {
            auto ref = objects.ResolveForWrite<ScrollBoxWidget>(objects.OwnerAccess(), id);
            if (ref) {
                auto* widget = ref.Value().Get().Value();
                if (widget->CachedElement().Get() == scroll) {
                    widget->initialOffset_ = offset;
                    widget->scrolled_.ExecuteIfBound(offset);
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
    auto& view = static_cast<ScrollView&>(element);
    const auto direction = horizontal_ ? StackDirection::Horizontal : StackDirection::Vertical;
    view.SetDirection(direction);
    static_cast<StackPanel&>(ChildHost(view)).SetDirection(direction);
    view.SetWheelPolicy(static_cast<ConsumeMouseWheel>(wheelPolicy_));
    (void)view.SetWheelMultiplier(wheelMultiplier_);
    (void)view.SetScrollbarThickness(thickness_);
    view.SetShowScrollbar(showBar_, alwaysShow_);
    (void)view.SetScrollbarPadding(barPadding_);
    view.SetAlwaysShowTrack(alwaysShowTrack_);
    view.SetScrollPadding(padBefore_, padAfter_);
    view.SetTouchScrolling(touchScrolling_);
    view.SetRightDragScrolling(rightDrag_);
}

void ScrollBoxWidget::ScrollToEnd() noexcept {
    if (auto live = CachedElement()) {
        static_cast<ScrollView&>(*live).ScrollToEnd();
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

    return static_cast<ScrollView&>(*view).ScrollIntoView(*entry, alignment, padding);
}

float ScrollBoxWidget::GetViewFraction() const noexcept {
    auto view = CachedElement();
    return view ? static_cast<ScrollView&>(*view).GetViewFraction() : 1;
}

float ScrollBoxWidget::GetViewOffsetFraction() const noexcept {
    auto view = CachedElement();
    return view ? static_cast<ScrollView&>(*view).GetViewOffsetFraction() : 0;
}

float ScrollBoxWidget::GetScrollOffsetOfEnd() const noexcept {
    auto view = CachedElement();
    return view ? static_cast<ScrollView&>(*view).GetMaxScrollOffset() : 0;
}

bool ScrollBoxWidget::IsScrolling() const noexcept {
    auto view = CachedElement();
    return view ? static_cast<ScrollView&>(*view).IsScrolling() : false;
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

void ScrollBoxWidget::SetShowScrollbar(bool visible, bool alwaysShow) noexcept {
    showBar_ = visible;
    alwaysShow_ = alwaysShow;
    if (auto view = CachedElement()) {
        ApplyProperties(*view);
    }
}

Result<void> ScrollBoxWidget::SetScrollbarPadding(Vec4 padding) noexcept {
    if (!padding.IsFinite() || padding.x < 0 || padding.y < 0 || padding.z < 0 || padding.w < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto live = CachedElement()) {
        auto set = static_cast<ScrollView&>(*live).SetScrollbarPadding(padding);
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
        static_cast<ScrollView&>(*live).SetAlwaysShowTrack(show);
    }
}

bool ScrollBoxWidget::IsAlwaysShowScrollbarTrack() const noexcept {
    return alwaysShowTrack_;
}

void ScrollBoxWidget::SetScrollPadding(bool before, bool after) noexcept {
    padBefore_ = before;
    padAfter_ = after;
    if (auto live = CachedElement()) {
        static_cast<ScrollView&>(*live).SetScrollPadding(before, after);
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

void ScrollBoxWidget::SetOnUserScrolled(Delegate<void(float)>&& callback) noexcept {
    scrolled_ = Move(callback);
}
} // namespace rocks::ui
