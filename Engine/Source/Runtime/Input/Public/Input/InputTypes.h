#pragma once

#include <Base/Types.h>

// 입력 장치의 키·버튼·축 식별자다. 값은 실행 중 식별용이며 저장 형식(키 바인딩 파일 등)에 숫자를
// 그대로 쓰지 않는다. 저장이 필요하면 이름(KeyName)으로 기록한다.
namespace rocks::input {

// 키보드 키다. 문자 배치(QWERTY·AZERTY)와 무관한 물리 위치 기준이다. 예: Key::W는 US 배열의 W
// 자리이며 프랑스 배열에서는 Z가 찍히는 키다. 입력된 문자가 필요하면 InputEventType::Text를 쓴다.
enum class Key : uint8 {
    None,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Digit0,
    Digit1,
    Digit2,
    Digit3,
    Digit4,
    Digit5,
    Digit6,
    Digit7,
    Digit8,
    Digit9,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    Escape,
    Enter,
    Tab,
    Backspace,
    Space,
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,
    Left,
    Right,
    Up,
    Down,
    LeftShift,
    RightShift,
    LeftControl,
    RightControl,
    LeftAlt,
    RightAlt,
    LeftSuper,
    RightSuper,
    Minus,
    Equals,
    LeftBracket,
    RightBracket,
    Backslash,
    Semicolon,
    Apostrophe,
    Comma,
    Period,
    Slash,
    Grave,
    CapsLock,
    Numpad0,
    Numpad1,
    Numpad2,
    Numpad3,
    Numpad4,
    Numpad5,
    Numpad6,
    Numpad7,
    Numpad8,
    Numpad9,
    NumpadAdd,
    NumpadSubtract,
    NumpadMultiply,
    NumpadDivide,
    NumpadDecimal,
    NumpadEnter,
    Count
};

enum class MouseButton : uint8 {
    Left,
    Right,
    Middle,
    X1,
    X2,
    Count
};

// 게임패드 버튼이다. 얼굴 버튼은 제조사 표기(A/B, ×/○) 대신 위치(South·East·West·North)로 부른다.
// Xbox 배치에서 South=A, East=B, West=X, North=Y다.
enum class GamepadButton : uint8 {
    South,
    East,
    West,
    North,
    LeftShoulder,
    RightShoulder,
    Back,
    Start,
    LeftThumb,
    RightThumb,
    DPadUp,
    DPadDown,
    DPadLeft,
    DPadRight,
    Count
};

// 게임패드 축이다. 스틱은 [-1, 1](오른쪽·위가 +), 트리거는 [0, 1]이다.
enum class GamepadAxis : uint8 {
    LeftX,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,
    Count
};

// 동시에 다루는 게임패드 수다(XInput 사용자 슬롯 수와 같다).
inline constexpr uint32 MaxGamepads = 4;

inline constexpr uint32 KeyCount = static_cast<uint32>(Key::Count);
inline constexpr uint32 MouseButtonCount = static_cast<uint32>(MouseButton::Count);
inline constexpr uint32 GamepadButtonCount = static_cast<uint32>(GamepadButton::Count);
inline constexpr uint32 GamepadAxisCount = static_cast<uint32>(GamepadAxis::Count);

// 키의 영문 이름이다(예: "W", "LeftShift", "Numpad5"). 진단·저장용이며 지역화하지 않는다. 범위 밖
// 값은 "Unknown"이다. 반환 문자열은 정적 수명이다.
const char* KeyName(Key key) noexcept;

} // namespace rocks::input
