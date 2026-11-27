#include <UiWidget/SliderWidget.generated.h>
#include "BindLiveWidgetEvent.h"
#include <UiFramework/Controls/Slider.h>

namespace rocks::ui {

Result<void> SliderWidget::SetRange(float minimum, float maximum) noexcept {
    if (!math::IsFinite(minimum) || !math::IsFinite(maximum) ||
        !math::IsFinite(maximum - minimum) || minimum >= maximum) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    minimum_ = minimum;
    maximum_ = maximum;
    value_ = math::Clamp(value_, minimum, maximum);
    if (auto live = CachedElement()) {
        return StaticElementCast<Slider>(*live).SetRange(minimum, maximum);
    }

    return Result<void>::Success();
}

Result<void> SliderWidget::SetStep(float step) noexcept {
    if (!math::IsFinite(step) || step <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    step_ = step;
    if (auto live = CachedElement()) {
        return StaticElementCast<Slider>(*live).SetStep(step);
    }

    return Result<void>::Success();
}

void SliderWidget::SetVertical(bool vertical) noexcept {
    vertical_ = vertical;
    if (auto live = CachedElement()) {
        StaticElementCast<Slider>(*live).SetVertical(vertical);
    }
}

void SliderWidget::SetSnapToStep(bool enabled) noexcept {
    snapToStep_ = enabled;
    if (auto live = CachedElement()) {
        StaticElementCast<Slider>(*live).SetSnapToStep(enabled);
    }
}

void SliderWidget::SetLocked(bool locked) noexcept {
    locked_ = locked;
    if (auto live = CachedElement()) {
        StaticElementCast<Slider>(*live).SetLocked(locked);
    }
}

void SliderWidget::SetIndentHandle(bool indent) noexcept {
    indentHandle_ = indent;
    if (auto live = CachedElement()) {
        StaticElementCast<Slider>(*live).SetIndentHandle(indent);
    }
}

Result<void> SliderWidget::SetTints(LinearColor bar, LinearColor handle) noexcept {
    if (!bar.IsFinite() || !handle.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    barTint_ = bar;
    handleTint_ = handle;
    if (auto live = CachedElement()) {
        auto& slider = StaticElementCast<Slider>(*live);
        slider.SetColor(bar);
        slider.SetHandleColor(handle);
    }

    return Result<void>::Success();
}

float SliderWidget::GetNormalizedValue() const noexcept {
    return (value_ - minimum_) / (maximum_ - minimum_);
}

void SliderWidget::SetRequiresControllerLock(bool required) noexcept {
    requiresControllerLock_ = required;
    if (auto live = CachedElement()) {
        StaticElementCast<Slider>(*live).SetRequiresControllerLock(required);
    }
}

void SliderWidget::SetOnControllerCaptureBegin(Delegate<void()>&& callback) noexcept {
    controllerBegin_ = Move(callback);
}

void SliderWidget::SetOnControllerCaptureEnd(Delegate<void()>&& callback) noexcept {
    controllerEnd_ = Move(callback);
}

void SliderWidget::SetOnCaptureBegin(Delegate<void()>&& callback) noexcept {
    captureBegin_ = Move(callback);
}

void SliderWidget::SetOnCaptureEnd(Delegate<void()>&& callback) noexcept {
    captureEnd_ = Move(callback);
}

float SliderWidget::Value() const noexcept {
    return value_;
}

void SliderWidget::SetOnChanged(Delegate<void(float)>&& callback) noexcept {
    changed_ = Move(callback);
}

Result<void> SliderWidget::SetValue(float value) noexcept {
    if (!math::IsFinite(value)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const float next = math::Clamp(value, minimum_, maximum_);
    if (next == value_) {
        return Result<void>::Success();
    }

    value_ = next;
    if (auto element = CachedElement()) {
        ApplyProperties(*element);
    }

    NotifyPropertyChanged("value");
    return Result<void>::Success();
}

void SliderWidget::ApplyProperties(Element& element) noexcept {
    (void)StaticElementCast<Slider>(element).SetValue(value_);
}

Result<SharedPtr<Element>> SliderWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto& allocator = objects.ObjectAllocator();
    auto slider = SharedPtr<Slider>::TryMake(allocator, allocator);
    if (!slider) {
        return Result<SharedPtr<Element>>::Failure(slider.Error());
    }

    auto ready = slider.Value()->SetRange(minimum_, maximum_);
    if (ready) {
        ready = slider.Value()->SetStep(step_);
    }

    if (!ready) {
        return Result<SharedPtr<Element>>::Failure(ready.Error());
    }

    slider.Value()->SetRequiresControllerLock(requiresControllerLock_);
    slider.Value()->SetOnControllerCaptureBegin(BindLiveWidgetEvent(
        objects, Handle(), slider.Value().Get(), &SliderWidget::controllerBegin_));
    slider.Value()->SetOnControllerCaptureEnd(BindLiveWidgetEvent(
        objects, Handle(), slider.Value().Get(), &SliderWidget::controllerEnd_));
    slider.Value()->SetLocked(locked_);
    slider.Value()->SetIndentHandle(indentHandle_);
    slider.Value()->SetColor(barTint_);
    slider.Value()->SetHandleColor(handleTint_);
    slider.Value()->SetOnCaptureBegin(
        BindLiveWidgetEvent(objects, Handle(), slider.Value().Get(), &SliderWidget::captureBegin_));
    slider.Value()->SetOnCaptureEnd(
        BindLiveWidgetEvent(objects, Handle(), slider.Value().Get(), &SliderWidget::captureEnd_));
    slider.Value()->SetVertical(vertical_);
    slider.Value()->SetSnapToStep(snapToStep_);
    slider.Value()->SetOnChanged(Delegate<void(float)>::CreateLambda(
        [&objects, owner = Handle(), native = slider.Value().Get()](float amount) noexcept {
            auto widget = objects.ResolveForWrite<SliderWidget>(objects.OwnerAccess(), owner);
            if (!widget) {
                return;
            }

            auto* value = widget.Value().Get().Value();
            if (value->CachedElement().Get() != native) {
                return;
            }

            value->value_ = amount;
            value->NotifyPropertyChanged("value");
            value->changed_.ExecuteIfBound(amount);
        }));
    return Result<SharedPtr<Element>>::Success(Move(slider.Value()));
}

} // namespace rocks::ui
