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

void ScreenInputRouter::SetInputHost(ScreenInputHost* host) noexcept {
    inputHost_ = host;
    appliedConfig_ = {};
}

bool ScreenInputRouter::ApplyInputConfig() noexcept {
    if (!inputHost_) {
        return false;
    }

    const ScreenInputConfig next = screens_.InputConfig();
    if (appliedConfig_ && *appliedConfig_ == next) {
        return false;
    }

    // 앞선 값이 없으면 무시하지 않던 상태다 — true를 부른 적 없이 false를 부르지 않는다
    // (1888–1899행).
    const bool oldIgnoreMove = appliedConfig_ && appliedConfig_->ignoreMove;
    const bool oldIgnoreLook = appliedConfig_ && appliedConfig_->ignoreLook;
    // 앞선 값이 없으면 GameAndMenu다(UE GetActiveInputMode 기본 인자 ECommonInputMode::All —
    // CommonUIActionRouterBase.h 94행).
    const ScreenInputMode oldMode =
        appliedConfig_ ? appliedConfig_->mode : ScreenInputMode::GameAndMenu;
    appliedConfig_ = next;
    if (oldIgnoreMove != next.ignoreMove) {
        inputHost_->SetIgnoreMoveInput(next.ignoreMove);
    }

    if (oldIgnoreLook != next.ignoreLook) {
        inputHost_->SetIgnoreLookInput(next.ignoreLook);
    }

    // Menu로 들어가면 눌린 채 남은 게임 입력을 푼다(1901–1906행). 이 호출은 다음 입력을 처리하기
    // 앞이다(Route 맨 앞 또는 호스트의 프레임 시작). 눌린 UI 패드 키·아날로그 반복도 푼다.
    if (next.mode != oldMode) {
        if (next.mode == ScreenInputMode::Menu) {
            inputHost_->FlushPressedKeys();
        }

        app_.CancelGamepadKeys(screens_.UserId());
    }

    // 붙잡지 않으면 커서는 늘 보인다(1938–1949행), 붙잡으면 showCursor대로다(1918–1921행).
    inputHost_->SetMouseCapture(next.captureMouse, next.captureMouse ? next.showCursor : true);
    return true;
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
    bool active = pressed != 0;
    for (usize i = 0; i < input::GamepadAxisCount && pad.connected; ++i) {
        if (math::Abs(pad.axes[i]) > 0.25f && math::Abs(old.axes[i] - pad.axes[i]) > 0.02f) {
            active = true;
        }
    }

    if (active) {
        Observe(ScreenInputDevice::Gamepad);
    }

    if (!allowUi) {
        // UI가 받지 않아도 패드 상태는 맞춘다(다음에 UI가 받을 때 누른 버튼을 새 누름으로 보지
        // 않게).
        app_.SyncGamepadState(screens_.UserId(), index, pad);
        return Result<bool>::Success(false);
    }

    // 초점이 사라진 채 방향·확인이 오면 화면의 원하는 초점 대상에 초점을 준다(20.8절 — UE는 초점
    // 없는 navigation을 하지 않는다).
    const uint32 user = screens_.UserId();
    const uint32 navigationButtons =
        Mask(input::GamepadButton::DPadLeft) | Mask(input::GamepadButton::DPadRight) |
        Mask(input::GamepadButton::DPadUp) | Mask(input::GamepadButton::DPadDown) |
        Mask(input::GamepadButton::South);
    const bool stick = static_cast<bool>(app_.GetNavigationConfig(user).DirectionFromAnalog(
        pad.axes[static_cast<uint32>(input::GamepadAxis::LeftX)],
        pad.axes[static_cast<uint32>(input::GamepadAxis::LeftY)]));
    if (((pressed & navigationButtons) || stick) && !app_.GetFocus(user) &&
        screens_.FocusDesiredTarget()) {
        app_.SyncGamepadState(screens_.UserId(), index, pad);
        return Result<bool>::Success(true);
    }

    // 화면 action 매핑이 먼저다(키보드와 같은 순서). 받으면 패드 상태만 맞춘다.
    if (pressed) {
        auto mapped = Dispatch(input::Key::None, pressed);
        if (!mapped || mapped.Value()) {
            app_.SyncGamepadState(user, index, pad);
            return mapped;
        }
    }

    // 버튼·스틱은 키 경로로 간다(D-pad navigation, South 확인, East 뒤로 — 20.8절).
    const auto root = screens_.ActiveRoot();
    if (root) {
        auto routed = app_.RouteGamepadState(user, index, pad, root);
        if (!routed || routed.Value()) {
            return routed;
        }
    }

    if (!pressed) {
        return Result<bool>::Success(false);
    }

    // 아무도 받지 않은 뒤로는 화면 뒤로 가기다.
    if (pressed & Mask(input::GamepadButton::East)) {
        return screens_.Back();
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
    (void)ApplyInputConfig();
    using input::InputEventType;
    const auto mode = screens_.InputConfig().mode;
    const bool allowUi = mode != ScreenInputMode::Game;
    const bool block = allowUi && (mode == ScreenInputMode::Menu || screens_.IsModal());
    if (event.type == InputEventType::FocusLost) {
        for (usize i = 0; i < input::MaxGamepads; ++i) {
            pads_[i] = {};
        }

        app_.ResetGamepads();

        return adapter_.Route(event);
    }

    if (event.type == InputEventType::PointerCaptureLost) {
        return adapter_.Route(event);
    }

    if (event.type == InputEventType::GamepadState) {
        auto handled = Gamepad(event, allowUi);
        return handled && block ? Result<bool>::Success(true) : Move(handled);
    }

    if (event.type == InputEventType::TouchBegan || event.type == InputEventType::TouchMoved) {
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

    // 초점이 사라진 채 방향·Tab·확인 키가 오면 화면의 원하는 초점 대상에 초점을 준다(20.8절).
    if (event.type == InputEventType::KeyDown && !app_.GetFocus(local.userId)) {
        const NavigationConfig& config = app_.GetNavigationConfig(local.userId);
        KeyChord chord;
        chord.shift = app_.IsShiftDown(local.userId);
        chord.control = app_.IsControlDown(local.userId);
        chord.alt = app_.IsAltDown(local.userId);
        if ((config.DirectionFromKey(event, chord) ||
             config.ActionFromKey(event) == NavigationAction::Accept) &&
            screens_.FocusDesiredTarget()) {
            return Result<bool>::Success(true);
        }
    }

    if (event.type == InputEventType::KeyDown && event.key == input::Key::Escape && !event.repeat) {
        return screens_.Back();
    }

    return Result<bool>::Success(block);
}

} // namespace rocks::ui
