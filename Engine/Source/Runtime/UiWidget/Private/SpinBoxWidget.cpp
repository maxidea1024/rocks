#include <UiWidget/SpinBoxWidget.h>
#include "ControlCaption.h"
#include "BindLiveWidgetEvent.h"
#include <UiFramework/SpinBox.h>
#include <UiWidget/SpinBoxWidget.generated.h>

namespace rocks::ui {

Result<void> SpinBoxWidget::SetLimits(float minimum, float maximum, float step) noexcept {
    if (!math::IsFinite(minimum) || !math::IsFinite(maximum) || !math::IsFinite(step) ||
        minimum > maximum || step <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (auto live = CachedElement()) {
        auto result = static_cast<SpinBox&>(*live).SetLimits(minimum, maximum, step);
        if (!result) {
            return result;
        }
    }
    minimum_ = minimum;
    maximum_ = maximum;
    step_ = step;
    value_ = math::Clamp(value_, minimum, maximum);
    return Result<void>::Success();
}

Result<void> SpinBoxWidget::SetValue(float value) noexcept {
    if (!math::IsFinite(value)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (auto live = CachedElement()) {
        auto result = static_cast<SpinBox&>(*live).SetValue(value);
        if (!result) {
            return result;
        }
    }
    const float next = math::Clamp(value, minimum_, maximum_);
    if (next == value_) {
        return Result<void>::Success();
    }
    value_ = next;
    NotifyPropertyChanged("value");
    return Result<void>::Success();
}

Result<void> SpinBoxWidget::SetFractionalDigits(int32 minimum, int32 maximum) noexcept {
    if (minimum < 0 || maximum < minimum || maximum > 9) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (auto live = CachedElement()) {
        auto updated = static_cast<SpinBox&>(*live).SetFractionalDigits(minimum, maximum);
        if (!updated) {
            return updated;
        }
    }
    minFractionDigits_ = minimum;
    maxFractionDigits_ = maximum;
    return Result<void>::Success();
}
void SpinBoxWidget::SetSliderEnabled(bool enabled) noexcept {
    sliderEnabled_ = enabled;
    if (auto live = CachedElement()) {
        static_cast<SpinBox&>(*live).SetSliderEnabled(enabled);
    }
}
void SpinBoxWidget::SetSnapToStep(bool enabled) noexcept {
    snapToStep_ = enabled;
    if (auto live = CachedElement()) {
        static_cast<SpinBox&>(*live).SetSnapToStep(enabled);
    }
}
void SpinBoxWidget::SetOnCommitted(Delegate<void(float, SpinCommitReason)>&& callback) noexcept {
    committed_ = Move(callback);
}
void SpinBoxWidget::SetOnDragBegin(Delegate<void()>&& callback) noexcept {
    beginDrag_ = Move(callback);
}
void SpinBoxWidget::SetOnDragEnd(Delegate<void(float)>&& callback) noexcept {
    endDrag_ = Move(callback);
}
float SpinBoxWidget::Value() const noexcept {
    return value_;
}

void SpinBoxWidget::SetOnChanged(Delegate<void(float)>&& callback) noexcept {
    changed_ = Move(callback);
}

Result<SharedPtr<Element>> SpinBoxWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto& allocator = objects.ObjectAllocator();
    auto made = SharedPtr<SpinBox>::TryMake(allocator, allocator);
    if (!made) {
        return Result<SharedPtr<Element>>::Failure(made.Error());
    }
    auto& box = *made.Value();
    auto initialized = box.SetLimits(minimum_, maximum_, step_);
    if (initialized) {
        initialized = box.SetValue(value_);
    }
    if (!initialized) {
        return Result<SharedPtr<Element>>::Failure(initialized.Error());
    }
    if (maxFractionDigits_ != -1 || minFractionDigits_ != 0) {
        initialized = box.SetFractionalDigits(minFractionDigits_, maxFractionDigits_);
        if (!initialized) {
            return Result<SharedPtr<Element>>::Failure(initialized.Error());
        }
    }
    box.SetSliderEnabled(sliderEnabled_);
    box.SetSnapToStep(snapToStep_);
    box.SetOnDragBegin(BindLiveWidgetEvent(objects, Handle(), &box, &SpinBoxWidget::beginDrag_));
    box.SetOnDragEnd(BindLiveWidgetEvent(objects, Handle(), &box, &SpinBoxWidget::endDrag_));
    box.SetOnCommitted(BindLiveWidgetEvent(objects, Handle(), &box, &SpinBoxWidget::committed_));
    auto caption = AttachControlCaption(objects, Handle(), box);
    if (!caption) {
        return Result<SharedPtr<Element>>::Failure(caption.Error());
    }
    initialized = box.SetCaption(Move(caption.Value()));
    if (!initialized) {
        return Result<SharedPtr<Element>>::Failure(initialized.Error());
    }
    box.SetOnChanged(Delegate<void(float)>::CreateLambda(
        [&objects, owner = Handle(), native = &box](float value) noexcept {
            auto widget = objects.ResolveForWrite<SpinBoxWidget>(objects.OwnerAccess(), owner);
            if (widget) {
                auto* current = widget.Value().Get().Value();
                if (current->CachedElement().Get() == native) {
                    current->value_ = value;
                    current->NotifyPropertyChanged("value");
                    current->changed_.ExecuteIfBound(value);
                }
            }
        }));
    return Result<SharedPtr<Element>>::Success(Move(made.Value()));
}

} // namespace rocks::ui
