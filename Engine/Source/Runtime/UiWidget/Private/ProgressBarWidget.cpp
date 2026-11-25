#include <UiWidget/ProgressBarWidget.generated.h>

namespace rocks::ui {

Result<SharedPtr<Element>> ProgressBarWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto& allocator = objects.ObjectAllocator();
    auto made = SharedPtr<ProgressBar>::TryMake(allocator, allocator);
    if (!made) {
        return Result<SharedPtr<Element>>::Failure(made.Error());
    }
    auto initialized = made.Value()->SetDirection(Direction());
    if (!initialized) {
        return Result<SharedPtr<Element>>::Failure(initialized.Error());
    }
    initialized = made.Value()->SetFillStyle(static_cast<ProgressFillStyle>(fillStyle_));
    if (initialized) {
        initialized = made.Value()->SetBorderPadding(borderPadding_);
    }
    if (initialized) {
        initialized = made.Value()->SetFillBrush(fillBrush_);
    }
    if (!initialized) {
        return Result<SharedPtr<Element>>::Failure(initialized.Error());
    }
    made.Value()->SetFillColor(fillColor_);
    bar_ = Move(made.Value());
    return Result<SharedPtr<Element>>::Success(bar_);
}

void ProgressBarWidget::ApplyProperties(Element&) noexcept {
    (void)bar_->SetValue(value_);
    bar_->SetMarquee(marquee_);
}

void ProgressBarWidget::ReleaseElements() noexcept {
    Widget::ReleaseElements();
    bar_.Reset();
}

Result<void> ProgressBarWidget::SetDirection(ProgressDirection direction) noexcept {
    if (direction > ProgressDirection::FromCenterVertical) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    direction_ = static_cast<uint8>(direction);
    return bar_ ? bar_->SetDirection(direction) : Result<void>::Success();
}

ProgressDirection ProgressBarWidget::Direction() const noexcept {
    return static_cast<ProgressDirection>(direction_);
}

Result<void> ProgressBarWidget::SetFillBrush(const Brush& brush) noexcept {
    if (!brush.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    fillBrush_ = brush;
    return bar_ ? bar_->SetFillBrush(brush) : Result<void>::Success();
}
Result<void> ProgressBarWidget::SetFillColor(LinearColor color) noexcept {
    if (!color.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    fillColor_ = color;
    if (bar_) {
        bar_->SetFillColor(color);
    }
    return Result<void>::Success();
}
Result<void> ProgressBarWidget::SetFillStyle(ProgressFillStyle style) noexcept {
    if (style > ProgressFillStyle::Mask) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    fillStyle_ = static_cast<uint8>(style);
    return bar_ ? bar_->SetFillStyle(style) : Result<void>::Success();
}
Result<void> ProgressBarWidget::SetBorderPadding(Vec2 padding) noexcept {
    if (!padding.IsFinite() || padding.x < 0 || padding.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    borderPadding_ = padding;
    return bar_ ? bar_->SetBorderPadding(padding) : Result<void>::Success();
}

void ProgressBarWidget::SetMarquee(bool enabled) noexcept {
    if (marquee_ == enabled) {
        return;
    }
    marquee_ = enabled;
    if (bar_) {
        bar_->SetMarquee(enabled);
    }
    NotifyPropertyChanged("marquee");
}

bool ProgressBarWidget::IsMarquee() const noexcept {
    return marquee_;
}

float ProgressBarWidget::Value() const noexcept {
    return value_;
}

Result<void> ProgressBarWidget::SetValue(float value) noexcept {
    if (!math::IsFinite(value)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    const float next = math::Clamp(value, 0.0f, 1.0f);
    if (value_ != next) {
        if (bar_) {
            auto applied = bar_->SetValue(next);
            if (!applied) {
                return applied;
            }
        }
        value_ = next;
        NotifyPropertyChanged("value");
    }
    return Result<void>::Success();
}

} // namespace rocks::ui
