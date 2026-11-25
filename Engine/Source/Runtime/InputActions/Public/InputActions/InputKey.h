#pragma once

#include <Base/Types.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Input/InputState.h>
#include <Input/InputTypes.h>
#include <InputActions/InputValue.h>
#include <InputActions/KeySample.h>

namespace rocks::input {

// 키의 종류다(Docs/Design/InputActions 2.2절 — UE FKey를 한 값으로).
enum class InputKeyKind : uint8 {
    None,
    Keyboard,
    MouseButton,
    MouseAxis,
    GamepadButton,
    GamepadAxis,
    Touch
};

// 마우스 축 키다. MouseY는 위로 +다(UE와 같다 — 화면 픽셀 delta의 부호를 뒤집는다).
enum class MouseAxisKey : uint8 {
    MouseX,
    MouseY,
    MouseWheel,
    MouseXY
};

inline constexpr uint32 MouseAxisKeyCount = 4;

// 게임패드 축 키다. 1D 축은 input::GamepadAxis와 같은 순서이고 2D 스틱이 뒤에 온다.
enum class GamepadAxisKey : uint8 {
    LeftX,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,
    LeftStick,
    RightStick
};

inline constexpr uint32 GamepadAxisKeyCount = 8;

// 키 하나다(UE FKey). code는 kind의 enum 값(Touch는 손가락 번호)이다.
//
// 사용 예:
//   const InputKey jump = InputKey::FromKey(Key::Space);
//   const InputKey move = InputKey::FromGamepadAxis(GamepadAxisKey::LeftStick);
struct ROCKS_INPUTACTIONS_API InputKey {
    InputKeyKind kind = InputKeyKind::None;
    uint16 code = 0;

    static constexpr InputKey FromKey(Key key) noexcept {
        return InputKey{InputKeyKind::Keyboard, static_cast<uint16>(key)};
    }

    static constexpr InputKey FromMouseButton(MouseButton button) noexcept {
        return InputKey{InputKeyKind::MouseButton, static_cast<uint16>(button)};
    }

    static constexpr InputKey FromMouseAxis(MouseAxisKey axis) noexcept {
        return InputKey{InputKeyKind::MouseAxis, static_cast<uint16>(axis)};
    }

    static constexpr InputKey FromGamepadButton(GamepadButton button) noexcept {
        return InputKey{InputKeyKind::GamepadButton, static_cast<uint16>(button)};
    }

    static constexpr InputKey FromGamepadAxis(GamepadAxisKey axis) noexcept {
        return InputKey{InputKeyKind::GamepadAxis, static_cast<uint16>(axis)};
    }

    // 범위 밖 손가락 번호는 잘못된 키(IsValid 거짓)다.
    static constexpr InputKey FromTouch(uint32 finger) noexcept {
        return InputKey{InputKeyKind::Touch,
                        static_cast<uint16>(finger < MaxTouches ? finger : MaxTouches)};
    }

    // 알려진 종류의 범위 안 code인지다.
    bool IsValid() const noexcept;

    // 아날로그(축) 키인지다(마우스 축·패드 축). 터치는 위치 키다.
    bool IsAxis() const noexcept;

    // 게임패드 키인지다(리바인딩의 같은 종류 판정 — 5.2절).
    bool IsGamepad() const noexcept {
        return kind == InputKeyKind::GamepadButton || kind == InputKeyKind::GamepadAxis;
    }

    bool IsTouch() const noexcept {
        return kind == InputKeyKind::Touch;
    }

    // 키 원시값의 타입이다(디지털 Boolean, 1D 축 Axis1D, 2D 축·터치 Axis2D).
    InputValueType RawType() const noexcept;

    friend constexpr bool operator==(const InputKey&, const InputKey&) noexcept = default;
};

// state에서 key를 읽는다. gamepad는 패드 키가 읽을 패드 번호다. 잘못된 키는 빈 표본이다.
ROCKS_INPUTACTIONS_API KeySample ReadKey(const InputState& state, uint32 gamepad,
                                         InputKey key) noexcept;

// 저장·리바인딩용 키 이름이다(UE EKeys 이름 — "SpaceBar", "Gamepad_FaceButton_Bottom", "Mouse2D").
// 잘못된 키는 "None"이다. 반환 문자열은 정적 수명이다.
ROCKS_INPUTACTIONS_API StringView InputKeyName(InputKey key) noexcept;

// InputKeyName의 역이다(ASCII 대소문자 무시). 모르는 이름은 NotFound다.
ROCKS_INPUTACTIONS_API Result<InputKey> ParseInputKey(StringView name) noexcept;

} // namespace rocks::input
