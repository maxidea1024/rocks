#include <Core/Math/MathUtility.h>
#include <Input/InputState.h>

namespace rocks::input {

namespace {

constexpr uint32 ToIndex(Key key) noexcept {
    return static_cast<uint32>(key);
}

// 마우스로 터치 흉내가 쓰는 손가락 번호다(UE와 같이 Touch1 — Docs/Design/TouchInput.ko.md 3.4절).
constexpr uint8 EmulatedTouchIndex = 0;

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
                                            "NumpadEnter",
                                            "Back"};
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

void InputQueue::DrainTo(InputQueue& target) noexcept {
    if (&target == this) {
        return;
    }

    target.dropped_ += dropped_ - forwardedDropped_;
    forwardedDropped_ = dropped_;
    for (const auto& event : events_) {
        target.Push(event);
    }

    Clear();
}

bool InputQueue::Push(const InputEvent& event) noexcept {
    // 초기화 전이거나 가득 찼으면 할당하지 않고 버린다.
    if (events_.Size() >= capacity_ || !events_.TryPushBack(event)) {
        ++dropped_;
        return false;
    }

    return true;
}

Result<void> InputState::SetGamepadDeadZone(const GamepadDeadZone& deadZone) noexcept {
    const float values[] = {deadZone.leftStick, deadZone.rightStick, deadZone.trigger};
    for (float value : values) {
        if (!math::IsFinite(value) || value < 0.0f || value >= 1.0f) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    deadZone_ = deadZone;
    return Result<void>::Success();
}

void InputState::SetMouseTouchEmulation(bool enabled) noexcept {
    if (enabled != mouseTouchEmulation_) {
        mouseTouchEmulation_ = enabled;
        cancelEmulatedTouch_ = true;
    }
}

void InputState::BeginFrame(Span<const InputEvent> events, bool eventsLost) noexcept {
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

    for (Touch& touch : touches_) {
        touch.began = false;
        touch.ended = false;
        touch.cancelled = false;
        touch.deltaX = 0.0f;
        touch.deltaY = 0.0f;
    }

    // 흉내 설정이 바뀌었으면 흉내 중이던 터치(손가락 0)를 취소로 끝낸다.
    if (cancelEmulatedTouch_) {
        cancelEmulatedTouch_ = false;
        Touch& emulated = touches_[EmulatedTouchIndex];
        if (emulated.down) {
            emulated.down = false;
            emulated.ended = true;
            emulated.cancelled = true;
        }
    }

    for (const InputEvent& event : events) {
        switch (event.type) {
        case InputEventType::KeyDown: {
            const uint32 index = ToIndex(event.key);
            if (event.key == Key::None || index >= KeyCount) {
                break;
            }

            // 이미 눌린 키의 down(자동 반복 포함)은 edge를 만들지 않는다. 눌려 있지 않던 키는
            // repeat 표시가 있어도 눌림으로 본다: 키를 누른 채 포커스를 되찾으면 OS의 첫 down이
            // 반복으로 오며, 이를 무시하면 뗄 때 짝 없는 WasReleased만 생긴다.
            if (!keysDown_.Get(index)) {
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
        case InputEventType::TextInputMethod:
            // IME 문자열은 활성 텍스트 입력 문맥(UI 편집 요소)으로 간다. 문맥이 없으면 OS가 확정
            // 문자를 Text로 보낸다(19.2절).
            break;
        case InputEventType::Text:
            if (textCount_ < MaxTextPerFrame) {
                text_[textCount_++] = event.codePoint;
            }
            break;
        case InputEventType::MouseMove:
            if (!math::IsFinite(event.x) || !math::IsFinite(event.y)) {
                break;
            }

            if (mouseTouchEmulation_ && touches_[EmulatedTouchIndex].down) {
                ApplyTouch(InputEvent::TouchMoved(EmulatedTouchIndex, event.x, event.y));
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

            if (mouseTouchEmulation_ && event.mouseButton == MouseButton::Left) {
                ApplyTouch(InputEvent::TouchBegan(EmulatedTouchIndex, mouseX_, mouseY_));
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

            if (mouseTouchEmulation_ && event.mouseButton == MouseButton::Left) {
                ApplyTouch(InputEvent::TouchEnded(EmulatedTouchIndex, mouseX_, mouseY_));
                break;
            }

            if ((mouseDown_ & Bit(index)) != 0) {
                mouseReleased_ |= Bit(index);
            }

            mouseDown_ &= ~Bit(index);
            break;
        }
        case InputEventType::PointerCaptureLost:
            mouseReleased_ |= mouseDown_;
            mouseDown_ = 0;
            break;
        case InputEventType::MouseLeave:
            break;
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
        case InputEventType::TouchBegan:
        case InputEventType::TouchMoved:
        case InputEventType::TouchEnded:
            ApplyTouch(event);
            break;
        case InputEventType::FocusLost:
            ReleaseAll();
            break;
        case InputEventType::NonClientPressed:
            break;
        }
    }

    if (eventsLost) {
        ReleaseAll();
    }
}

void InputState::ApplyTouch(const InputEvent& event) noexcept {
    if (event.touchIndex >= MaxTouches || !math::IsFinite(event.x) || !math::IsFinite(event.y)) {
        return;
    }

    Touch& touch = touches_[event.touchIndex];
    float force = 1.0f;
    if (event.hasForce && math::IsFinite(event.force)) {
        force = event.force < 0.0f ? 0.0f : (event.force > 1.0f ? 1.0f : event.force);
    }

    switch (event.type) {
    case InputEventType::TouchBegan:
        // 이미 닿아 있는 번호의 시작은 이동으로 본다(배정기가 막지만 잘못된 backend에 대비한다).
        if (!touch.down) {
            touch.began = true;
            touch.startX = event.x;
            touch.startY = event.y;
        } else {
            touch.deltaX += event.x - touch.x;
            touch.deltaY += event.y - touch.y;
        }

        touch.down = true;
        touch.x = event.x;
        touch.y = event.y;
        touch.force = force;
        touch.hasForce = event.hasForce;
        break;
    case InputEventType::TouchMoved:
        // 시작 없이 오는 이동은 무시한다(require-release).
        if (!touch.down) {
            break;
        }

        touch.deltaX += event.x - touch.x;
        touch.deltaY += event.y - touch.y;
        touch.x = event.x;
        touch.y = event.y;
        touch.force = force;
        touch.hasForce = event.hasForce;
        break;
    case InputEventType::TouchEnded:
        if (!touch.down) {
            break;
        }

        touch.deltaX += event.x - touch.x;
        touch.deltaY += event.y - touch.y;
        touch.x = event.x;
        touch.y = event.y;
        touch.down = false;
        touch.ended = true;
        touch.cancelled = event.cancelled;
        break;
    default:
        break;
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

    for (float& axis : gamepad.rawAxes) {
        axis = 0.0f;
    }

    if (!snapshot.connected) {
        return;
    }

    for (uint32 axisIndex = 0; axisIndex < GamepadAxisCount; ++axisIndex) {
        const float raw = snapshot.axes[axisIndex];
        gamepad.rawAxes[axisIndex] = math::IsFinite(raw) ? raw : 0.0f;
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
    for (Touch& touch : touches_) {
        if (touch.down) {
            touch.down = false;
            touch.ended = true;
            touch.cancelled = true;
        }
    }

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

bool InputState::IsTouchDown(uint32 index) const noexcept {
    return index < MaxTouches && touches_[index].down;
}

bool InputState::WasTouchBegan(uint32 index) const noexcept {
    return index < MaxTouches && touches_[index].began;
}

bool InputState::WasTouchEnded(uint32 index) const noexcept {
    return index < MaxTouches && touches_[index].ended;
}

bool InputState::WasTouchCancelled(uint32 index) const noexcept {
    return index < MaxTouches && touches_[index].cancelled;
}

float InputState::TouchX(uint32 index) const noexcept {
    return index < MaxTouches ? touches_[index].x : 0.0f;
}

float InputState::TouchY(uint32 index) const noexcept {
    return index < MaxTouches ? touches_[index].y : 0.0f;
}

float InputState::TouchStartX(uint32 index) const noexcept {
    return index < MaxTouches ? touches_[index].startX : 0.0f;
}

float InputState::TouchStartY(uint32 index) const noexcept {
    return index < MaxTouches ? touches_[index].startY : 0.0f;
}

float InputState::TouchDeltaX(uint32 index) const noexcept {
    return index < MaxTouches ? touches_[index].deltaX : 0.0f;
}

float InputState::TouchDeltaY(uint32 index) const noexcept {
    return index < MaxTouches ? touches_[index].deltaY : 0.0f;
}

float InputState::TouchForce(uint32 index) const noexcept {
    return index < MaxTouches ? touches_[index].force : 1.0f;
}

bool InputState::TouchHasForce(uint32 index) const noexcept {
    return index < MaxTouches && touches_[index].hasForce;
}

uint32 InputState::TouchCount() const noexcept {
    uint32 count = 0;
    for (const Touch& touch : touches_) {
        count += touch.down ? 1u : 0u;
    }

    return count;
}

float InputState::GamepadAxisValue(uint32 gamepad, GamepadAxis axis) const noexcept {
    const uint32 index = static_cast<uint32>(axis);
    if (gamepad >= MaxGamepads || index >= GamepadAxisCount || !gamepads_[gamepad].connected) {
        return 0.0f;
    }

    return gamepads_[gamepad].axes[index];
}

float InputState::GamepadAxisRawValue(uint32 gamepad, GamepadAxis axis) const noexcept {
    const uint32 index = static_cast<uint32>(axis);
    if (gamepad >= MaxGamepads || index >= GamepadAxisCount || !gamepads_[gamepad].connected) {
        return 0.0f;
    }

    return gamepads_[gamepad].rawAxes[index];
}

} // namespace rocks::input
