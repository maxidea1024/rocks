#include <Core/Math/MathUtility.h>
#include <Input/InputState.h>

namespace rocks::input {

namespace {

constexpr uint32 ToIndex(Key key) noexcept {
    return static_cast<uint32>(key);
}

constexpr uint32 Bit(uint32 index) noexcept {
    return uint32(1) << index;
}

// 원형 데드존: 반지름 deadZone 안은 0, 밖은 [deadZone, 1]을 [0, 1]로 다시 늘린다. 방향은 유지한다.
void ApplyStickDeadZone(float& x, float& y, float deadZone) noexcept {
    const float length = math::Sqrt(x * x + y * y);
    if (!math::IsFinite(length) || length <= deadZone || deadZone >= 1.0f) {
        x = 0.0f;
        y = 0.0f;
        return;
    }

    const float clamped = length > 1.0f ? 1.0f : length;
    const float scale = (clamped - deadZone) / (1.0f - deadZone) / length;
    x *= scale;
    y *= scale;
}

float ApplyTriggerDeadZone(float value, float deadZone) noexcept {
    if (!math::IsFinite(value) || value <= deadZone || deadZone >= 1.0f) {
        return 0.0f;
    }

    const float clamped = value > 1.0f ? 1.0f : value;
    return (clamped - deadZone) / (1.0f - deadZone);
}

} // namespace

const char* KeyName(Key key) noexcept {
    static constexpr const char* names[] = {"None",
                                            "A",
                                            "B",
                                            "C",
                                            "D",
                                            "E",
                                            "F",
                                            "G",
                                            "H",
                                            "I",
                                            "J",
                                            "K",
                                            "L",
                                            "M",
                                            "N",
                                            "O",
                                            "P",
                                            "Q",
                                            "R",
                                            "S",
                                            "T",
                                            "U",
                                            "V",
                                            "W",
                                            "X",
                                            "Y",
                                            "Z",
                                            "Digit0",
                                            "Digit1",
                                            "Digit2",
                                            "Digit3",
                                            "Digit4",
                                            "Digit5",
                                            "Digit6",
                                            "Digit7",
                                            "Digit8",
                                            "Digit9",
                                            "F1",
                                            "F2",
                                            "F3",
                                            "F4",
                                            "F5",
                                            "F6",
                                            "F7",
                                            "F8",
                                            "F9",
                                            "F10",
                                            "F11",
                                            "F12",
                                            "Escape",
                                            "Enter",
                                            "Tab",
                                            "Backspace",
                                            "Space",
                                            "Insert",
                                            "Delete",
                                            "Home",
                                            "End",
                                            "PageUp",
                                            "PageDown",
                                            "Left",
                                            "Right",
                                            "Up",
                                            "Down",
                                            "LeftShift",
                                            "RightShift",
                                            "LeftControl",
                                            "RightControl",
                                            "LeftAlt",
                                            "RightAlt",
                                            "LeftSuper",
                                            "RightSuper",
                                            "Minus",
                                            "Equals",
                                            "LeftBracket",
                                            "RightBracket",
                                            "Backslash",
                                            "Semicolon",
                                            "Apostrophe",
                                            "Comma",
                                            "Period",
                                            "Slash",
                                            "Grave",
                                            "CapsLock",
                                            "Numpad0",
                                            "Numpad1",
                                            "Numpad2",
                                            "Numpad3",
                                            "Numpad4",
                                            "Numpad5",
                                            "Numpad6",
                                            "Numpad7",
                                            "Numpad8",
                                            "Numpad9",
                                            "NumpadAdd",
                                            "NumpadSubtract",
                                            "NumpadMultiply",
                                            "NumpadDivide",
                                            "NumpadDecimal",
                                            "NumpadEnter"};
    static_assert(sizeof(names) / sizeof(names[0]) == KeyCount, "KeyName table is out of sync");
    const uint32 index = ToIndex(key);
    return index < KeyCount ? names[index] : "Unknown";
}

Result<void> InputQueue::TryInitialize(usize capacity) noexcept {
    if (capacity == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    events_.Clear();
    auto reserved = events_.TryReserve(capacity);
    if (!reserved) {
        return reserved;
    }

    capacity_ = capacity;
    return Result<void>::Success();
}

bool InputQueue::Push(const InputEvent& event) noexcept {
    // 초기화 전이거나 가득 찼으면 할당하지 않고 버린다.
    if (events_.Size() >= capacity_ || !events_.TryPushBack(event)) {
        ++dropped_;
        return false;
    }

    return true;
}

void InputState::BeginFrame(Span<const InputEvent> events) noexcept {
    keysPressed_ = {};
    keysReleased_ = {};
    mousePressed_ = 0;
    mouseReleased_ = 0;
    mouseDeltaX_ = 0.0f;
    mouseDeltaY_ = 0.0f;
    wheel_ = 0.0f;
    textCount_ = 0;
    for (Gamepad& gamepad : gamepads_) {
        gamepad.pressed = 0;
        gamepad.released = 0;
    }

    for (const InputEvent& event : events) {
        switch (event.type) {
        case InputEventType::KeyDown: {
            const uint32 index = ToIndex(event.key);
            if (event.key == Key::None || index >= KeyCount) {
                break;
            }
            // 자동 반복이거나 이미 눌린 키의 중복 down은 edge를 만들지 않는다.
            if (!event.repeat && !keysDown_.Get(index)) {
                keysPressed_.Set(index, true);
            }
            keysDown_.Set(index, true);
            break;
        }
        case InputEventType::KeyUp: {
            const uint32 index = ToIndex(event.key);
            if (event.key == Key::None || index >= KeyCount) {
                break;
            }
            if (keysDown_.Get(index)) {
                keysReleased_.Set(index, true);
            }
            keysDown_.Set(index, false);
            break;
        }
        case InputEventType::Text:
            if (textCount_ < MaxTextPerFrame) {
                text_[textCount_++] = event.codePoint;
            }
            break;
        case InputEventType::MouseMove:
            if (!math::IsFinite(event.x) || !math::IsFinite(event.y)) {
                break;
            }
            if (mouseDeltaValid_) {
                mouseDeltaX_ += event.x - mouseX_;
                mouseDeltaY_ += event.y - mouseY_;
            }
            mouseX_ = event.x;
            mouseY_ = event.y;
            hasMousePosition_ = true;
            mouseDeltaValid_ = true;
            break;
        case InputEventType::MouseDown: {
            const uint32 index = static_cast<uint32>(event.mouseButton);
            if (index >= MouseButtonCount) {
                break;
            }
            if ((mouseDown_ & Bit(index)) == 0) {
                mousePressed_ |= Bit(index);
            }
            mouseDown_ |= Bit(index);
            break;
        }
        case InputEventType::MouseUp: {
            const uint32 index = static_cast<uint32>(event.mouseButton);
            if (index >= MouseButtonCount) {
                break;
            }
            if ((mouseDown_ & Bit(index)) != 0) {
                mouseReleased_ |= Bit(index);
            }
            mouseDown_ &= ~Bit(index);
            break;
        }
        case InputEventType::MouseWheel:
            if (math::IsFinite(event.wheel)) {
                wheel_ += event.wheel;
            }
            break;
        case InputEventType::GamepadState:
            if (event.gamepadIndex < MaxGamepads) {
                ApplyGamepad(event.gamepadIndex, event.gamepad);
            }
            break;
        case InputEventType::FocusLost:
            ReleaseAll();
            break;
        }
    }
}

void InputState::ApplyGamepad(uint32 index, const GamepadSnapshot& snapshot) noexcept {
    Gamepad& gamepad = gamepads_[index];
    const uint32 valid = Bit(GamepadButtonCount) - 1;
    const uint32 buttons = snapshot.connected ? (snapshot.buttons & valid) : 0;
    gamepad.pressed |= buttons & ~gamepad.buttons;
    gamepad.released |= gamepad.buttons & ~buttons;
    gamepad.buttons = buttons;
    gamepad.connected = snapshot.connected;
    for (float& axis : gamepad.axes) {
        axis = 0.0f;
    }

    if (!snapshot.connected) {
        return;
    }

    float leftX = snapshot.axes[static_cast<uint32>(GamepadAxis::LeftX)];
    float leftY = snapshot.axes[static_cast<uint32>(GamepadAxis::LeftY)];
    float rightX = snapshot.axes[static_cast<uint32>(GamepadAxis::RightX)];
    float rightY = snapshot.axes[static_cast<uint32>(GamepadAxis::RightY)];
    ApplyStickDeadZone(leftX, leftY, deadZone_.leftStick);
    ApplyStickDeadZone(rightX, rightY, deadZone_.rightStick);
    gamepad.axes[static_cast<uint32>(GamepadAxis::LeftX)] = leftX;
    gamepad.axes[static_cast<uint32>(GamepadAxis::LeftY)] = leftY;
    gamepad.axes[static_cast<uint32>(GamepadAxis::RightX)] = rightX;
    gamepad.axes[static_cast<uint32>(GamepadAxis::RightY)] = rightY;
    gamepad.axes[static_cast<uint32>(GamepadAxis::LeftTrigger)] = ApplyTriggerDeadZone(
        snapshot.axes[static_cast<uint32>(GamepadAxis::LeftTrigger)], deadZone_.trigger);
    gamepad.axes[static_cast<uint32>(GamepadAxis::RightTrigger)] = ApplyTriggerDeadZone(
        snapshot.axes[static_cast<uint32>(GamepadAxis::RightTrigger)], deadZone_.trigger);
}

void InputState::ReleaseAll() noexcept {
    for (uint32 word = 0; word < KeyWords; ++word) {
        keysReleased_.words[word] |= keysDown_.words[word];
        keysDown_.words[word] = 0;
    }

    mouseReleased_ |= mouseDown_;
    mouseDown_ = 0;
    // 포커스를 다시 얻은 뒤 첫 위치는 이전 위치와의 차이를 이동량으로 쓰지 않는다.
    mouseDeltaValid_ = false;
}

bool InputState::IsDown(Key key) const noexcept {
    return ToIndex(key) < KeyCount && keysDown_.Get(ToIndex(key));
}

bool InputState::WasPressed(Key key) const noexcept {
    return ToIndex(key) < KeyCount && keysPressed_.Get(ToIndex(key));
}

bool InputState::WasReleased(Key key) const noexcept {
    return ToIndex(key) < KeyCount && keysReleased_.Get(ToIndex(key));
}

bool InputState::IsDown(MouseButton button) const noexcept {
    const uint32 index = static_cast<uint32>(button);
    return index < MouseButtonCount && (mouseDown_ & Bit(index)) != 0;
}

bool InputState::WasPressed(MouseButton button) const noexcept {
    const uint32 index = static_cast<uint32>(button);
    return index < MouseButtonCount && (mousePressed_ & Bit(index)) != 0;
}

bool InputState::WasReleased(MouseButton button) const noexcept {
    const uint32 index = static_cast<uint32>(button);
    return index < MouseButtonCount && (mouseReleased_ & Bit(index)) != 0;
}

bool InputState::IsGamepadConnected(uint32 index) const noexcept {
    return index < MaxGamepads && gamepads_[index].connected;
}

bool InputState::IsDown(uint32 gamepad, GamepadButton button) const noexcept {
    const uint32 index = static_cast<uint32>(button);
    return gamepad < MaxGamepads && index < GamepadButtonCount &&
           (gamepads_[gamepad].buttons & Bit(index)) != 0;
}

bool InputState::WasPressed(uint32 gamepad, GamepadButton button) const noexcept {
    const uint32 index = static_cast<uint32>(button);
    return gamepad < MaxGamepads && index < GamepadButtonCount &&
           (gamepads_[gamepad].pressed & Bit(index)) != 0;
}

bool InputState::WasReleased(uint32 gamepad, GamepadButton button) const noexcept {
    const uint32 index = static_cast<uint32>(button);
    return gamepad < MaxGamepads && index < GamepadButtonCount &&
           (gamepads_[gamepad].released & Bit(index)) != 0;
}

float InputState::GamepadAxisValue(uint32 gamepad, GamepadAxis axis) const noexcept {
    const uint32 index = static_cast<uint32>(axis);
    if (gamepad >= MaxGamepads || index >= GamepadAxisCount || !gamepads_[gamepad].connected) {
        return 0.0f;
    }

    return gamepads_[gamepad].axes[index];
}

} // namespace rocks::input
