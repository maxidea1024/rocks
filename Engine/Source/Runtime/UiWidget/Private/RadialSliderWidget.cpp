#include <UiFramework/Controls/RadialSlider.h>
#include <UiWidget/RadialSliderWidget.generated.h>

namespace rocks::ui {

Result<void> RadialSliderWidget::SetLimits(float minimum, float maximum, float step) noexcept {
    if (!math::IsFinite(minimum) || !math::IsFinite(maximum) || minimum >= maximum ||
        !math::IsFinite(maximum - minimum) || !math::IsFinite(step) || step <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto live = CachedElement()) {
        auto& slider = StaticElementCast<RadialSlider>(*live);
        (void)slider.SetRange(minimum, maximum);
        (void)slider.SetStep(step);
    }

    minimum_ = minimum;
    maximum_ = maximum;
    step_ = step;
    value_ = math::Clamp(value_, minimum, maximum);
    return Result<void>::Success();
}

Result<void> RadialSliderWidget::SetValue(float value) noexcept {
    if (!math::IsFinite(value)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const float next = math::Clamp(value, minimum_, maximum_);
    if (next == value_) {
        return Result<void>::Success();
    }

    value_ = next;
    if (auto live = CachedElement()) {
        auto applied = StaticElementCast<RadialSlider>(*live).SetValue(value_);
        if (!applied) {
            return applied;
        }
    }

    NotifyPropertyChanged("value");
    return Result<void>::Success();
}

float RadialSliderWidget::Value() const noexcept {
    return value_;
}

Result<void> RadialSliderWidget::SetArc(float start, float end, float offset) noexcept {
    if (!RadialSlider::IsValidArc(start, end, offset)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    startAngle_ = start;
    endAngle_ = end;
    angularOffset_ = offset;
    if (auto live = CachedElement()) {
        return StaticElementCast<RadialSlider>(*live).SetArc(start, end, offset);
    }

    return Result<void>::Success();
}

void RadialSliderWidget::SetSnapToStep(bool enabled) noexcept {
    snapToStep_ = enabled;
    if (auto live = CachedElement()) {
        StaticElementCast<RadialSlider>(*live).SetSnapToStep(enabled);
    }
}

void RadialSliderWidget::SetHandVisibility(bool handle, bool hand) noexcept {
    showHandle_ = handle;
    showHand_ = hand;
    if (auto live = CachedElement()) {
        StaticElementCast<RadialSlider>(*live).SetHandVisibility(handle, hand);
    }
}

Result<void> RadialSliderWidget::SetColors(LinearColor track, LinearColor progress,
                                           LinearColor handle) noexcept {
    if (!track.IsFinite() || !progress.IsFinite() || !handle.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    trackColor_ = track;
    progressColor_ = progress;
    handleColor_ = handle;
    if (auto live = CachedElement()) {
        auto& slider = StaticElementCast<RadialSlider>(*live);
        slider.SetColor(track);
        slider.SetFillColor(progress);
        slider.SetHandleColor(handle);
    }

    return Result<void>::Success();
}

void RadialSliderWidget::SetOnChanged(Delegate<void(float)>&& callback) noexcept {
    changed_ = Move(callback);
}

Result<SharedPtr<Element>> RadialSliderWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto& allocator = objects.ObjectAllocator();
    auto made = SharedPtr<RadialSlider>::TryMake(allocator, allocator);
    if (!made) {
        return Result<SharedPtr<Element>>::Failure(made.Error());
    }

    auto ready = made.Value()->Construct(
        {.minimum = minimum_,
         .maximum = maximum_,
         .step = step_,
         .value = value_,
         .startAngle = startAngle_,
         .endAngle = endAngle_,
         .angularOffset = angularOffset_,
         .snapToStep = snapToStep_,
         .showHandle = showHandle_,
         .showHand = showHand_,
         .track = trackColor_,
         .progress = progressColor_,
         .handle = handleColor_,
         .onChanged = Delegate<void(float)>::CreateLambda(
             [&objects, owner = Handle(), native = made.Value().Get()](float amount) noexcept {
                 auto widget =
                     objects.ResolveForWrite<RadialSliderWidget>(objects.OwnerAccess(), owner);
                 if (widget) {
                     auto* current = widget.Value().Get().Value();
                     if (current->CachedElement().Get() == native) {
                         current->value_ = amount;
                         current->changed_.ExecuteIfBound(amount);
                     }
                 }
             })});
    if (!ready) {
        return Result<SharedPtr<Element>>::Failure(ready.Error());
    }

    return Result<SharedPtr<Element>>::Success(Move(made.Value()));
}

} // namespace rocks::ui
