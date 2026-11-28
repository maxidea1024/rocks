#include <UiWidget/SpinBoxWidget.h>
#include "BindLiveWidgetEvent.h"
#include <UiFramework/Controls/SpinBox.h>
#include <UiWidget/SpinBoxWidget.generated.h>
#include <UiWidget/WidgetFontSize.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

SpinBoxWidget::SpinBoxWidget(Allocator& allocator) noexcept
    : committed_(allocator), beginDrag_(allocator), endDrag_(allocator), changed_(allocator) {
}

Result<void> SpinBoxWidget::SetLimits(float minimum, float maximum, float step) noexcept {
    if (!math::IsFinite(minimum) || !math::IsFinite(maximum) || !math::IsFinite(step) ||
        minimum > maximum || step <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto live = CachedElement()) {
        auto& box = StaticElementCast<SpinBox>(*live);
        auto result = box.SetValueRange(minimum, maximum);
        if (result) {
            result = box.SetDelta(step);
        }

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
        auto result = StaticElementCast<SpinBox>(*live).SetValue(value);
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
    if (minimum < 0 || maximum < minimum || maximum > MaxNumericFractionalDigits) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto live = CachedElement()) {
        auto updated = StaticElementCast<SpinBox>(*live).SetFractionalDigits(minimum, maximum);
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
        StaticElementCast<SpinBox>(*live).SetSliderEnabled(enabled);
    }
}

void SpinBoxWidget::SetSnapToStep(bool enabled) noexcept {
    snapToStep_ = enabled;
    if (auto live = CachedElement()) {
        StaticElementCast<SpinBox>(*live).SetAlwaysUsesDeltaSnap(enabled);
    }
}

Event<float, WidgetSpinCommitReason>& SpinBoxWidget::OnCommitted() noexcept {
    return committed_;
}

Event<>& SpinBoxWidget::OnDragBegin() noexcept {
    return beginDrag_;
}

Event<float>& SpinBoxWidget::OnDragEnd() noexcept {
    return endDrag_;
}

float SpinBoxWidget::Value() const noexcept {
    return value_;
}

Event<float>& SpinBoxWidget::OnChanged() noexcept {
    return changed_;
}

Result<SharedPtr<Element>> SpinBoxWidget::BuildElement(ObjectRegistry& objects) noexcept {
    const auto access = objects.OwnerAccess();
    auto outer = objects.GetOuter(access, Handle());
    if (!outer) {
        return Result<SharedPtr<Element>>::Failure(outer.Error());
    }

    auto tree = objects.Resolve<WidgetTree>(access, outer.Value());
    if (!tree) {
        return Result<SharedPtr<Element>>::Failure(tree.Error());
    }

    // 숫자 글자는 편집기 자식이 셰이핑한다(24.5절 — 트리의 셰이퍼).
    auto& allocator = objects.ObjectAllocator();
    auto made = SharedPtr<SpinBox>::TryMake(allocator, allocator,
                                            tree.Value().Get().Value()->GetTextShaper(),
                                            tree.Value().Get().Value()->GetPlatform());
    if (!made) {
        return Result<SharedPtr<Element>>::Failure(made.Error());
    }

    auto& box = *made.Value();
    auto initialized = box.Initialize(DefaultWidgetFontSize);
    // 옛 계약(범위·단계)은 값 범위·Delta다(슬라이더 범위는 값 범위를 따른다 — UE USpinBox).
    if (initialized) {
        initialized = box.SetValueRange(minimum_, maximum_);
    }

    if (initialized) {
        initialized = box.SetDelta(step_);
    }

    if (initialized) {
        initialized = box.SetValue(value_);
    }

    if (initialized && (maxFractionDigits_ != -1 || minFractionDigits_ != 0)) {
        initialized = box.SetFractionalDigits(
            minFractionDigits_,
            maxFractionDigits_ < 0 ? DefaultMaxNumericFractionalDigits : maxFractionDigits_);
    }

    if (!initialized) {
        return Result<SharedPtr<Element>>::Failure(initialized.Error());
    }

    box.SetSliderEnabled(sliderEnabled_);
    box.SetAlwaysUsesDeltaSnap(snapToStep_);
    box.SetOnDragBegin(BindLiveWidgetEvent(objects, Handle(), &box, &SpinBoxWidget::beginDrag_));
    box.SetOnDragEnd(Delegate<void(double)>::CreateLambda(
        [&objects, owner = Handle(), native = &box](double value) noexcept {
            auto widget = objects.ResolveForWrite<SpinBoxWidget>(objects.OwnerAccess(), owner);
            if (widget && widget.Value().Get().Value()->CachedElement().Get() == native) {
                widget.Value().Get().Value()->endDrag_.Broadcast(
                    {objects, objects.OwnerAccess(), owner}, static_cast<float>(value));
            }
        }));
    box.SetOnCommitted(Delegate<void(double, SpinCommitReason)>::CreateLambda(
        [&objects, owner = Handle(), native = &box](double value,
                                                    SpinCommitReason reason) noexcept {
            auto widget = objects.ResolveForWrite<SpinBoxWidget>(objects.OwnerAccess(), owner);
            if (widget && widget.Value().Get().Value()->CachedElement().Get() == native) {
                widget.Value().Get().Value()->committed_.Broadcast(
                    {objects, objects.OwnerAccess(), owner}, static_cast<float>(value),
                    ToWidgetSpinCommitReason(reason));
            }
        }));
    box.SetOnChanged(Delegate<void(double)>::CreateLambda(
        [&objects, owner = Handle(), native = &box](double value) noexcept {
            auto widget = objects.ResolveForWrite<SpinBoxWidget>(objects.OwnerAccess(), owner);
            if (widget) {
                auto* current = widget.Value().Get().Value();
                if (current->CachedElement().Get() == native) {
                    current->value_ = static_cast<float>(value);
                    current->NotifyPropertyChanged("value");
                    current->changed_.Broadcast({objects, objects.OwnerAccess(), owner},
                                                static_cast<float>(value));
                }
            }
        }));
    return Result<SharedPtr<Element>>::Success(Move(made.Value()));
}

} // namespace rocks::ui
