#include <Core/ScopeExit.h>
#include <UiScreens/ScreenInputRouter.h>

namespace rocks::ui {

namespace {
uint32 Mask(input::GamepadButton button) noexcept {
    return button < input::GamepadButton::Count ? uint32{1} << static_cast<uint32>(button) : 0;
}
} // namespace

ScreenInputRouter::ScreenInputRouter(Allocator& allocator, Application& application,
                                     ScreenContainer& screens) noexcept
    : app_(application), screens_(screens), adapter_(application), mappings_(allocator) {
}

Result<void> ScreenInputRouter::AddMapping(ScreenActionMapping mapping) noexcept {
    if (routing_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (mapping.action == Guid{} || mapping.key >= input::Key::Count ||
        mapping.button > input::GamepadButton::Count ||
        (mapping.key == input::Key::None && mapping.button == input::GamepadButton::Count)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    usize insertion = mappings_.Size();
    for (usize i = 0; i < mappings_.Size(); ++i) {
        const auto& other = mappings_[i];
        if (other.domain == mapping.domain && other.action == mapping.action) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
        if (insertion == mappings_.Size() && other.priority < mapping.priority) {
            insertion = i;
        }
    }
    return mappings_.TryInsert(mapping, insertion);
}

Result<void> ScreenInputRouter::RemoveMapping(Guid domain, Guid action) noexcept {
    if (routing_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    for (usize i = 0; i < mappings_.Size(); ++i) {
        if (mappings_[i].domain == domain && mappings_[i].action == action) {
            return mappings_.TryRemoveAt(i);
        }
    }
    return Result<void>::Failure(ErrorCode::NotFound);
}

Span<const ScreenActionMapping> ScreenInputRouter::Mappings() const noexcept {
    return mappings_.View();
}
Result<SharedPtr<ScreenDeviceState>> ScreenInputRouter::ObserveDevice() noexcept {
    if (!deviceState_) {
        auto state = SharedPtr<ScreenDeviceState>::TryMake(mappings_.GetAllocator(),
                                                           mappings_.GetAllocator(), device_);
        if (!state) {
            return state;
        }
        deviceState_ = Move(state.Value());
    }
    return Result<SharedPtr<ScreenDeviceState>>::Success(deviceState_);
}

ScreenInputDevice ScreenInputRouter::Device() const noexcept {
    return device_;
}
void ScreenInputRouter::SetOnDeviceChanged(Delegate<void(ScreenInputDevice)>&& callback) noexcept {
    deviceChanged_ = Move(callback);
}

void ScreenInputRouter::Observe(ScreenInputDevice device) noexcept {
    if (device_ != device) {
        device_ = device;
        if (deviceState_) {
            deviceState_->SetDevice(device);
        }
        deviceChanged_.ExecuteIfBound(device);
    }
}

Result<bool> ScreenInputRouter::Dispatch(input::Key key, uint32 pressed) noexcept {
    for (const auto& mapping : mappings_) {
        if ((key != input::Key::None && mapping.key == key) || (pressed & Mask(mapping.button))) {
            auto result = screens_.DispatchAction(mapping.domain, mapping.action);
            if (!result || result.Value()) {
                return result;
            }
        }
    }
    return Result<bool>::Success(false);
}

Result<bool> ScreenInputRouter::Gamepad(const input::InputEvent& event, bool allowUi) noexcept {
    const auto index = event.gamepadIndex;
    if (index >= input::MaxGamepads) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }
    const auto old = pads_[index];
    const auto& pad = event.gamepad;
    pads_[index] = pad;
    const auto buttons = pad.connected ? pad.buttons : 0u;
    const uint32 pressed = buttons & ~old.buttons;
    const uint32 released = old.buttons & ~buttons;
    bool active = pressed != 0;
    for (usize i = 0; i < input::GamepadAxisCount && pad.connected; ++i) {
        if (math::Abs(pad.axes[i]) > 0.25f && math::Abs(old.axes[i] - pad.axes[i]) > 0.02f) {
            active = true;
        }
    }
    if (active) {
        Observe(ScreenInputDevice::Gamepad);
    }
    const auto accept = Mask(input::GamepadButton::South);
    if (released & accept) {
        auto target = acceptTargets_[index].Lock();
        acceptTargets_[index] = {};
        const auto root = screens_.ActiveRoot();
        if (target && (!pad.connected || !allowUi || !root || !target->IsWithin(*root))) {
            target->OnKeyCancelled(screens_.UserId(), input::Key::Enter);
        } else if (target && app_.GetFocus(screens_.UserId()).Get() == target.Get()) {
            return app_.RouteKeyWithin(screens_.UserId(),
                                       input::InputEvent::KeyUp(input::Key::Enter), root);
        }
    }
    if (!allowUi || !pressed) {
        return Result<bool>::Success(false);
    }
    const auto root = screens_.ActiveRoot();
    if (root && app_.RouteGamepadButtons(screens_.UserId(), pressed, root)) {
        return Result<bool>::Success(true);
    }
    auto mapped = Dispatch(input::Key::None, pressed);
    if (!mapped || mapped.Value()) {
        return mapped;
    }
    if (pressed & Mask(input::GamepadButton::East)) {
        return screens_.Back();
    }
    struct DirectionButton {
        input::GamepadButton button;
        FocusDirection direction;
    };
    constexpr DirectionButton directions[] = {
        {input::GamepadButton::DPadLeft, FocusDirection::Left},
        {input::GamepadButton::DPadRight, FocusDirection::Right},
        {input::GamepadButton::DPadUp, FocusDirection::Up},
        {input::GamepadButton::DPadDown, FocusDirection::Down}};
    for (const auto& direction : directions) {
        if (pressed & Mask(direction.button)) {
            return app_.Navigate(screens_.UserId(), direction.direction, screens_.ActiveRoot());
        }
    }
    if (pressed & accept) {
        auto focus = app_.GetFocus(screens_.UserId());
        auto handled =
            app_.RouteKeyWithin(screens_.UserId(), input::InputEvent::KeyDown(input::Key::Enter),
                                screens_.ActiveRoot());
        if (handled && handled.Value()) {
            acceptTargets_[index] = WeakPtr<Element>(focus);
        }
        return handled;
    }
    return Result<bool>::Success(false);
}

Result<bool> ScreenInputRouter::Route(const input::InputEvent& event) noexcept {
    if (routing_) {
        return Result<bool>::Failure(ErrorCode::Busy);
    }
    routing_ = true;
    ROCKS_ON_SCOPE_EXIT {
        routing_ = false;
    };
    using input::InputEventType;
    const auto mode = screens_.InputConfig().mode;
    const bool allowUi = mode != ScreenInputMode::Game;
    const bool block = allowUi && (mode == ScreenInputMode::Menu || screens_.IsModal());
    if (event.type == InputEventType::FocusLost) {
        for (usize i = 0; i < input::MaxGamepads; ++i) {
            pads_[i] = {};
            acceptTargets_[i] = {};
        }
        return adapter_.Route(event);
    }
    if (event.type == InputEventType::PointerCaptureLost) {
        return adapter_.Route(event);
    }
    if (event.type == InputEventType::GamepadState) {
        auto handled = Gamepad(event, allowUi);
        return handled && block ? Result<bool>::Success(true) : Move(handled);
    }
    if (event.type == InputEventType::TouchBegin || event.type == InputEventType::TouchMove) {
        Observe(ScreenInputDevice::Touch);
    } else if (event.type == InputEventType::MouseDown || event.type == InputEventType::MouseMove ||
               event.type == InputEventType::MouseWheel || event.type == InputEventType::KeyDown) {
        Observe(ScreenInputDevice::KeyboardMouse);
    }
    if (!allowUi) {
        return Result<bool>::Success(false);
    }
    if (event.type == InputEventType::KeyDown && !event.repeat) {
        auto mapped = Dispatch(event.key, 0);
        if (!mapped || mapped.Value()) {
            return mapped;
        }
    }
    auto local = event;
    local.userId = screens_.UserId();
    auto handled = adapter_.RouteWithin(local, screens_.ActiveRoot());
    if (!handled || handled.Value()) {
        return handled;
    }
    if (event.type == InputEventType::KeyDown && event.key == input::Key::Escape && !event.repeat) {
        return screens_.Back();
    }
    return Result<bool>::Success(block);
}

} // namespace rocks::ui
