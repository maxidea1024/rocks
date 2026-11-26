#include <UiScreens/HardwareVisibilityWidget.h>
#include <UiFramework/Layout/SizeBox.h>

namespace rocks::ui {

HardwareVisibilityWidget::HardwareVisibilityWidget(Allocator& allocator) noexcept
    : ContentWidget(allocator) {
}

Result<SharedPtr<Element>> HardwareVisibilityWidget::CreatePanel(Allocator& allocator) noexcept {
    auto box = SharedPtr<SizeBox>::TryMake(allocator, allocator);
    return box ? Result<SharedPtr<Element>>::Success(Move(box.Value()))
               : Result<SharedPtr<Element>>::Failure(box.Error());
}

void HardwareVisibilityWidget::ApplyProperties(Element& element) noexcept {
    const auto device = state_ ? state_->Device() : ScreenInputDevice::KeyboardMouse;
    const uint8 bit = static_cast<uint8>(1u << static_cast<uint8>(device));
    element.SetVisibility((deviceMask_ & bit) ? Visibility::Visible
                                              : static_cast<Visibility>(hiddenMode_));
}

void HardwareVisibilityWidget::Unsubscribe() noexcept {
    if (state_ && handle_.IsValid()) {
        state_->OnChanged().Remove(handle_);
    }

    handle_.Reset();
}

Result<void> HardwareVisibilityWidget::Subscribe(const SharedPtr<Element>& live) noexcept {
    if (!state_) {
        return Result<void>::Success();
    }

    const auto hidden = static_cast<Visibility>(hiddenMode_);
    const uint8 mask = deviceMask_;
    auto subscription = state_->OnChanged().TryAddWeakLambda(
        live, [view = live.Get(), mask, hidden](ScreenInputDevice device) noexcept {
            view->SetVisibility((mask & (1u << static_cast<uint8>(device))) ? Visibility::Visible
                                                                            : hidden);
        });
    if (!subscription) {
        return Result<void>::Failure(subscription.Error());
    }

    handle_ = subscription.Value();
    return Result<void>::Success();
}

Result<SharedPtr<Element>>
HardwareVisibilityWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto view = PanelWidget::BuildElement(objects);
    if (!view) {
        return view;
    }

    Unsubscribe();
    auto result = Subscribe(view.Value());
    return result ? Move(view) : Result<SharedPtr<Element>>::Failure(result.Error());
}

Result<void>
HardwareVisibilityWidget::SetDeviceState(const SharedPtr<ScreenDeviceState>& state) noexcept {
    auto old = state_;
    auto oldHandle = handle_;
    state_ = state;
    handle_.Reset();
    if (auto live = CachedElement()) {
        auto subscribed = Subscribe(live);
        if (!subscribed) {
            state_ = Move(old);
            handle_ = oldHandle;
            return subscribed;
        }

        ApplyProperties(*live);
    }

    if (old && oldHandle.IsValid()) {
        old->OnChanged().Remove(oldHandle);
    }

    return Result<void>::Success();
}

Result<void> HardwareVisibilityWidget::SetDeviceMask(uint8 mask, Visibility hidden) noexcept {
    if ((mask & ~AllScreenInputDevices) != 0 ||
        (hidden != Visibility::Hidden && hidden != Visibility::Collapsed)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const uint8 oldMask = deviceMask_, oldHidden = hiddenMode_;
    deviceMask_ = mask;
    hiddenMode_ = static_cast<uint8>(hidden);
    auto changed = SetDeviceState(state_);
    if (!changed) {
        deviceMask_ = oldMask;
        hiddenMode_ = oldHidden;
    }

    return changed;
}

void HardwareVisibilityWidget::ReleaseElements() noexcept {
    Unsubscribe();
    PanelWidget::ReleaseElements();
}
} // namespace rocks::ui
