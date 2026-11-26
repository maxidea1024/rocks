#include <Core/Text/StringView.h>
#include <InputActions/InputKey.h>

// 키 읽기·이름이다(Docs/Design/InputActions 2.2절).
namespace rocks::input {

namespace {

// UE EKeys 이름(InputCoreTypes.cpp)이다. 키보드는 input::Key 순서다. 숫자패드 Enter는 UE에 따로
// 없어(EKeys::Enter 하나) Rocks 이름이다.
constexpr const char* KeyboardKeyNames[] = {
    "None",
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
    "Zero",
    "One",
    "Two",
    "Three",
    "Four",
    "Five",
    "Six",
    "Seven",
    "Eight",
    "Nine",
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
    "BackSpace",
    "SpaceBar",
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
    "LeftCommand",
    "RightCommand",
    "Hyphen",
    "Equals",
    "LeftBracket",
    "RightBracket",
    "Backslash",
    "Semicolon",
    "Apostrophe",
    "Comma",
    "Period",
    "Slash",
    "Tilde",
    "CapsLock",
    "NumPadZero",
    "NumPadOne",
    "NumPadTwo",
    "NumPadThree",
    "NumPadFour",
    "NumPadFive",
    "NumPadSix",
    "NumPadSeven",
    "NumPadEight",
    "NumPadNine",
    "Add",
    "Subtract",
    "Multiply",
    "Divide",
    "Decimal",
    "NumPadEnter",
    "Android_Back",
};
static_assert(sizeof(KeyboardKeyNames) / sizeof(KeyboardKeyNames[0]) == KeyCount,
              "keyboard key name table must match input::Key");

constexpr const char* MouseButtonNames[] = {
    "LeftMouseButton",  "RightMouseButton",  "MiddleMouseButton",
    "ThumbMouseButton", "ThumbMouseButton2",
};
static_assert(sizeof(MouseButtonNames) / sizeof(MouseButtonNames[0]) == MouseButtonCount,
              "mouse button name table must match input::MouseButton");

constexpr const char* MouseAxisNames[] = {"MouseX", "MouseY", "MouseWheelAxis", "Mouse2D"};
static_assert(sizeof(MouseAxisNames) / sizeof(MouseAxisNames[0]) == MouseAxisKeyCount,
              "mouse axis name table must match MouseAxisKey");

constexpr const char* GamepadButtonNames[] = {
    "Gamepad_FaceButton_Bottom", "Gamepad_FaceButton_Right", "Gamepad_FaceButton_Left",
    "Gamepad_FaceButton_Top",    "Gamepad_LeftShoulder",     "Gamepad_RightShoulder",
    "Gamepad_Special_Left",      "Gamepad_Special_Right",    "Gamepad_LeftThumbstick",
    "Gamepad_RightThumbstick",   "Gamepad_DPad_Up",          "Gamepad_DPad_Down",
    "Gamepad_DPad_Left",         "Gamepad_DPad_Right",
};
static_assert(sizeof(GamepadButtonNames) / sizeof(GamepadButtonNames[0]) == GamepadButtonCount,
              "gamepad button name table must match input::GamepadButton");

constexpr const char* GamepadAxisNames[] = {
    "Gamepad_LeftX",  "Gamepad_LeftY",           "Gamepad_RightX",
    "Gamepad_RightY", "Gamepad_LeftTriggerAxis", "Gamepad_RightTriggerAxis",
    "Gamepad_Left2D", "Gamepad_Right2D",
};
static_assert(sizeof(GamepadAxisNames) / sizeof(GamepadAxisNames[0]) == GamepadAxisKeyCount,
              "gamepad axis name table must match GamepadAxisKey");

constexpr const char* TouchNames[] = {
    "Touch1", "Touch2", "Touch3", "Touch4", "Touch5",
    "Touch6", "Touch7", "Touch8", "Touch9", "Touch10",
};
static_assert(sizeof(TouchNames) / sizeof(TouchNames[0]) == MaxTouches,
              "touch name table must match input::MaxTouches");

static_assert(
    static_cast<uint8>(GamepadAxisKey::LeftX) == static_cast<uint8>(GamepadAxis::LeftX) &&
        static_cast<uint8>(GamepadAxisKey::LeftY) == static_cast<uint8>(GamepadAxis::LeftY) &&
        static_cast<uint8>(GamepadAxisKey::RightX) == static_cast<uint8>(GamepadAxis::RightX) &&
        static_cast<uint8>(GamepadAxisKey::RightY) == static_cast<uint8>(GamepadAxis::RightY) &&
        static_cast<uint8>(GamepadAxisKey::LeftTrigger) ==
            static_cast<uint8>(GamepadAxis::LeftTrigger) &&
        static_cast<uint8>(GamepadAxisKey::RightTrigger) ==
            static_cast<uint8>(GamepadAxis::RightTrigger),
    "1D GamepadAxisKey values must match input::GamepadAxis");

// 1D 패드 축 키의 input::GamepadAxis다(2D 스틱은 둘을 읽는다).
GamepadAxis KeyPadAxis(GamepadAxisKey axis) noexcept {
    return static_cast<GamepadAxis>(static_cast<uint8>(axis));
}

KeySample KeyDigitalSample(bool down, bool pressed, bool released) noexcept {
    KeySample sample;
    // 한 프레임에 눌렀다 뗐으면 눌림 값이다(UE EnhancedPlayerInput.cpp:603-612).
    const bool value = down || pressed;
    sample.raw = InputValue::FromBool(value);
    sample.down = down;
    sample.pressed = pressed;
    sample.released = released;
    return sample;
}

KeySample KeyAxisSample(InputValueType type, const Vec3d& raw) noexcept {
    KeySample sample;
    sample.raw = InputValue(type, raw);
    sample.down = sample.raw.MagnitudeSquared() != 0.0;
    return sample;
}

} // namespace

bool InputKey::IsValid() const noexcept {
    switch (kind) {
    case InputKeyKind::None:
        return false;
    case InputKeyKind::Keyboard:
        return code > 0 && code < KeyCount;
    case InputKeyKind::MouseButton:
        return code < MouseButtonCount;
    case InputKeyKind::MouseAxis:
        return code < MouseAxisKeyCount;
    case InputKeyKind::GamepadButton:
        return code < GamepadButtonCount;
    case InputKeyKind::GamepadAxis:
        return code < GamepadAxisKeyCount;
    case InputKeyKind::Touch:
        return code < MaxTouches;
    }

    return false;
}

bool InputKey::IsAxis() const noexcept {
    return kind == InputKeyKind::MouseAxis || kind == InputKeyKind::GamepadAxis;
}

InputValueType InputKey::RawType() const noexcept {
    switch (kind) {
    case InputKeyKind::MouseAxis:
        return code == static_cast<uint16>(MouseAxisKey::MouseXY) ? InputValueType::Axis2D
                                                                  : InputValueType::Axis1D;
    case InputKeyKind::GamepadAxis:
        return code >= static_cast<uint16>(GamepadAxisKey::LeftStick) ? InputValueType::Axis2D
                                                                      : InputValueType::Axis1D;
    case InputKeyKind::Touch:
        return InputValueType::Axis2D;
    default:
        return InputValueType::Boolean;
    }
}

KeySample ReadKey(const InputState& state, uint32 gamepad, InputKey key) noexcept {
    if (!key.IsValid()) {
        return KeySample{};
    }

    switch (key.kind) {
    case InputKeyKind::Keyboard: {
        const auto value = static_cast<Key>(key.code);
        if (value == Key::Enter) {
            // UE는 숫자패드 Enter도 EKeys::Enter로 받는다. 둘 중 하나라도 눌려 있으면 눌림이고,
            // 놓임은 둘 다 놓였을 때다(2.2절).
            const bool down = state.IsDown(Key::Enter) || state.IsDown(Key::NumpadEnter);
            const bool pressed = state.WasPressed(Key::Enter) || state.WasPressed(Key::NumpadEnter);
            const bool released =
                !down && (state.WasReleased(Key::Enter) || state.WasReleased(Key::NumpadEnter));
            return KeyDigitalSample(down, pressed, released);
        }

        return KeyDigitalSample(state.IsDown(value), state.WasPressed(value),
                                state.WasReleased(value));
    }

    case InputKeyKind::MouseButton: {
        const auto value = static_cast<MouseButton>(key.code);
        return KeyDigitalSample(state.IsDown(value), state.WasPressed(value),
                                state.WasReleased(value));
    }

    case InputKeyKind::MouseAxis: {
        // MouseY는 위로 +다(화면 픽셀 delta는 아래로 + — UE SceneViewport와 같은 부호).
        const double x = state.MouseDeltaX();
        const double y = -static_cast<double>(state.MouseDeltaY());
        switch (static_cast<MouseAxisKey>(key.code)) {
        case MouseAxisKey::MouseX:
            return KeyAxisSample(InputValueType::Axis1D, Vec3d(x, 0.0, 0.0));
        case MouseAxisKey::MouseY:
            return KeyAxisSample(InputValueType::Axis1D, Vec3d(y, 0.0, 0.0));
        case MouseAxisKey::MouseWheel:
            return KeyAxisSample(InputValueType::Axis1D, Vec3d(state.MouseWheel(), 0.0, 0.0));
        case MouseAxisKey::MouseXY:
            return KeyAxisSample(InputValueType::Axis2D, Vec3d(x, y, 0.0));
        }

        return KeySample{};
    }

    case InputKeyKind::GamepadButton: {
        const auto value = static_cast<GamepadButton>(key.code);
        return KeyDigitalSample(state.IsDown(gamepad, value), state.WasPressed(gamepad, value),
                                state.WasReleased(gamepad, value));
    }

    case InputKeyKind::GamepadAxis: {
        const auto axis = static_cast<GamepadAxisKey>(key.code);
        if (axis == GamepadAxisKey::LeftStick || axis == GamepadAxisKey::RightStick) {
            const bool left = axis == GamepadAxisKey::LeftStick;
            const double x =
                state.GamepadAxisRawValue(gamepad, left ? GamepadAxis::LeftX : GamepadAxis::RightX);
            const double y =
                state.GamepadAxisRawValue(gamepad, left ? GamepadAxis::LeftY : GamepadAxis::RightY);
            return KeyAxisSample(InputValueType::Axis2D, Vec3d(x, y, 0.0));
        }

        return KeyAxisSample(InputValueType::Axis1D,
                             Vec3d(state.GamepadAxisRawValue(gamepad, KeyPadAxis(axis)), 0.0, 0.0));
    }

    case InputKeyKind::Touch: {
        // 닿아 있는 동안 화면 좌표, 놓으면 0이다(UE EnhancedPlayerInput.cpp:551-566). 한 프레임에
        // 닿았다 떨어졌으면 그 좌표로 한 번 눌린 것으로 본다.
        const uint32 finger = key.code;
        const bool down = state.IsTouchDown(finger);
        const bool began = state.WasTouchBegan(finger);
        const bool ended = state.WasTouchEnded(finger);
        KeySample sample;
        const bool touching = down || began;
        sample.raw =
            InputValue(InputValueType::Axis2D,
                       touching ? Vec3d(state.TouchX(finger), state.TouchY(finger), 0.0) : Vec3d());
        sample.down = down;
        sample.pressed = began;
        sample.released = ended;
        return sample;
    }

    case InputKeyKind::None:
        break;
    }

    return KeySample{};
}

uint32 InputKeySlot(InputKey key) noexcept {
    if (!key.IsValid()) {
        return InputKeySlotCount;
    }

    uint32 base = 0;
    switch (key.kind) {
    case InputKeyKind::Touch:
        base += GamepadAxisKeyCount;
        [[fallthrough]];
    case InputKeyKind::GamepadAxis:
        base += GamepadButtonCount;
        [[fallthrough]];
    case InputKeyKind::GamepadButton:
        base += MouseAxisKeyCount;
        [[fallthrough]];
    case InputKeyKind::MouseAxis:
        base += MouseButtonCount;
        [[fallthrough]];
    case InputKeyKind::MouseButton:
        base += KeyCount;
        [[fallthrough]];
    case InputKeyKind::Keyboard:
        return base + key.code;
    case InputKeyKind::None:
        break;
    }

    return InputKeySlotCount;
}

StringView InputKeyName(InputKey key) noexcept {
    if (!key.IsValid()) {
        return StringView("None");
    }

    switch (key.kind) {
    case InputKeyKind::Keyboard:
        return StringView(KeyboardKeyNames[key.code]);
    case InputKeyKind::MouseButton:
        return StringView(MouseButtonNames[key.code]);
    case InputKeyKind::MouseAxis:
        return StringView(MouseAxisNames[key.code]);
    case InputKeyKind::GamepadButton:
        return StringView(GamepadButtonNames[key.code]);
    case InputKeyKind::GamepadAxis:
        return StringView(GamepadAxisNames[key.code]);
    case InputKeyKind::Touch:
        return StringView(TouchNames[key.code]);
    case InputKeyKind::None:
        break;
    }

    return StringView("None");
}

Result<InputKey> ParseInputKey(StringView name) noexcept {
    struct Table {
        InputKeyKind kind;
        const char* const* names;
        uint32 count;
        uint32 first;
    };

    const Table tables[] = {
        {InputKeyKind::Keyboard, KeyboardKeyNames, KeyCount, 1},
        {InputKeyKind::MouseButton, MouseButtonNames, MouseButtonCount, 0},
        {InputKeyKind::MouseAxis, MouseAxisNames, MouseAxisKeyCount, 0},
        {InputKeyKind::GamepadButton, GamepadButtonNames, GamepadButtonCount, 0},
        {InputKeyKind::GamepadAxis, GamepadAxisNames, GamepadAxisKeyCount, 0},
        {InputKeyKind::Touch, TouchNames, MaxTouches, 0},
    };
    for (const Table& table : tables) {
        for (uint32 index = table.first; index < table.count; ++index) {
            if (EqualsIgnoringAsciiCase(name, StringView(table.names[index]))) {
                return Result<InputKey>::Success(InputKey{table.kind, static_cast<uint16>(index)});
            }
        }
    }

    return Result<InputKey>::Failure(ErrorCode::NotFound);
}

} // namespace rocks::input
