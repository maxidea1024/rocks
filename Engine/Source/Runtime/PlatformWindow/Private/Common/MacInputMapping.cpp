#include "MacInputMapping.h"

namespace rocks::platform::mac {

namespace {

using input::Key;

// kVK_* 값이다(Carbon HIToolbox/Events.h). ANSI 배치 이름이며 물리 위치를 뜻한다.
enum VirtualKey : uint16 {
    VkA = 0x00,
    VkS = 0x01,
    VkD = 0x02,
    VkF = 0x03,
    VkH = 0x04,
    VkG = 0x05,
    VkZ = 0x06,
    VkX = 0x07,
    VkC = 0x08,
    VkV = 0x09,
    VkB = 0x0B,
    VkQ = 0x0C,
    VkW = 0x0D,
    VkE = 0x0E,
    VkR = 0x0F,
    VkY = 0x10,
    VkT = 0x11,
    Vk1 = 0x12,
    Vk2 = 0x13,
    Vk3 = 0x14,
    Vk4 = 0x15,
    Vk6 = 0x16,
    Vk5 = 0x17,
    VkEqual = 0x18,
    Vk9 = 0x19,
    Vk7 = 0x1A,
    VkMinus = 0x1B,
    Vk8 = 0x1C,
    Vk0 = 0x1D,
    VkRightBracket = 0x1E,
    VkO = 0x1F,
    VkU = 0x20,
    VkLeftBracket = 0x21,
    VkI = 0x22,
    VkP = 0x23,
    VkReturn = 0x24,
    VkL = 0x25,
    VkJ = 0x26,
    VkQuote = 0x27,
    VkK = 0x28,
    VkSemicolon = 0x29,
    VkBackslash = 0x2A,
    VkComma = 0x2B,
    VkSlash = 0x2C,
    VkN = 0x2D,
    VkM = 0x2E,
    VkPeriod = 0x2F,
    VkTab = 0x30,
    VkSpace = 0x31,
    VkGrave = 0x32,
    VkDelete = 0x33,
    VkEscape = 0x35,
    VkRightCommand = 0x36,
    VkCommand = 0x37,
    VkShift = 0x38,
    VkCapsLock = 0x39,
    VkOption = 0x3A,
    VkControl = 0x3B,
    VkRightShift = 0x3C,
    VkRightOption = 0x3D,
    VkRightControl = 0x3E,
    VkKeypadDecimal = 0x41,
    VkKeypadMultiply = 0x43,
    VkKeypadPlus = 0x45,
    VkKeypadDivide = 0x4B,
    VkKeypadEnter = 0x4C,
    VkKeypadMinus = 0x4E,
    VkKeypad0 = 0x52,
    VkKeypad1 = 0x53,
    VkKeypad2 = 0x54,
    VkKeypad3 = 0x55,
    VkKeypad4 = 0x56,
    VkKeypad5 = 0x57,
    VkKeypad6 = 0x58,
    VkKeypad7 = 0x59,
    VkKeypad8 = 0x5B,
    VkKeypad9 = 0x5C,
    VkF5 = 0x60,
    VkF6 = 0x61,
    VkF7 = 0x62,
    VkF3 = 0x63,
    VkF8 = 0x64,
    VkF9 = 0x65,
    VkF11 = 0x67,
    VkF10 = 0x6D,
    VkF12 = 0x6F,
    // Insert 키가 있는 키보드는 Help 코드를 보낸다.
    VkHelp = 0x72,
    VkHome = 0x73,
    VkPageUp = 0x74,
    VkForwardDelete = 0x75,
    VkF4 = 0x76,
    VkEnd = 0x77,
    VkF2 = 0x78,
    VkPageDown = 0x79,
    VkF1 = 0x7A,
    VkLeftArrow = 0x7B,
    VkRightArrow = 0x7C,
    VkDownArrow = 0x7D,
    VkUpArrow = 0x7E,
};

// NSEvent buttonNumber다.
constexpr int64 LeftButtonNumber = 0;
constexpr int64 RightButtonNumber = 1;
constexpr int64 MiddleButtonNumber = 2;
constexpr int64 FirstSideButtonNumber = 3;
constexpr int64 SecondSideButtonNumber = 4;

// 제어 문자 경계다.
constexpr uint32 FirstPrintableCodePoint = 0x20;
constexpr uint32 DeleteCodePoint = 0x7F;

// 반올림 보정이다.
constexpr double RoundingHalf = 0.5;

// modifier 키와 그 장치 의존 비트다.
struct ModifierKey {
    uint16 keyCode;
    Key key;
    uint64 deviceMask;
};

constexpr ModifierKey ModifierKeys[] = {
    {VkShift, Key::LeftShift, DeviceLeftShiftMask},
    {VkRightShift, Key::RightShift, DeviceRightShiftMask},
    {VkControl, Key::LeftControl, DeviceLeftControlMask},
    {VkRightControl, Key::RightControl, DeviceRightControlMask},
    {VkOption, Key::LeftAlt, DeviceLeftOptionMask},
    {VkRightOption, Key::RightAlt, DeviceRightOptionMask},
    {VkCommand, Key::LeftSuper, DeviceLeftCommandMask},
    {VkRightCommand, Key::RightSuper, DeviceRightCommandMask},
};

} // namespace

Key KeyFromVirtualKeyCode(uint16 keyCode) noexcept {
    switch (keyCode) {
    case VkA:
        return Key::A;
    case VkB:
        return Key::B;
    case VkC:
        return Key::C;
    case VkD:
        return Key::D;
    case VkE:
        return Key::E;
    case VkF:
        return Key::F;
    case VkG:
        return Key::G;
    case VkH:
        return Key::H;
    case VkI:
        return Key::I;
    case VkJ:
        return Key::J;
    case VkK:
        return Key::K;
    case VkL:
        return Key::L;
    case VkM:
        return Key::M;
    case VkN:
        return Key::N;
    case VkO:
        return Key::O;
    case VkP:
        return Key::P;
    case VkQ:
        return Key::Q;
    case VkR:
        return Key::R;
    case VkS:
        return Key::S;
    case VkT:
        return Key::T;
    case VkU:
        return Key::U;
    case VkV:
        return Key::V;
    case VkW:
        return Key::W;
    case VkX:
        return Key::X;
    case VkY:
        return Key::Y;
    case VkZ:
        return Key::Z;
    case Vk0:
        return Key::Digit0;
    case Vk1:
        return Key::Digit1;
    case Vk2:
        return Key::Digit2;
    case Vk3:
        return Key::Digit3;
    case Vk4:
        return Key::Digit4;
    case Vk5:
        return Key::Digit5;
    case Vk6:
        return Key::Digit6;
    case Vk7:
        return Key::Digit7;
    case Vk8:
        return Key::Digit8;
    case Vk9:
        return Key::Digit9;
    case VkF1:
        return Key::F1;
    case VkF2:
        return Key::F2;
    case VkF3:
        return Key::F3;
    case VkF4:
        return Key::F4;
    case VkF5:
        return Key::F5;
    case VkF6:
        return Key::F6;
    case VkF7:
        return Key::F7;
    case VkF8:
        return Key::F8;
    case VkF9:
        return Key::F9;
    case VkF10:
        return Key::F10;
    case VkF11:
        return Key::F11;
    case VkF12:
        return Key::F12;
    case VkEscape:
        return Key::Escape;
    case VkReturn:
        return Key::Enter;
    case VkTab:
        return Key::Tab;
    case VkDelete:
        return Key::Backspace;
    case VkSpace:
        return Key::Space;
    case VkHelp:
        return Key::Insert;
    case VkForwardDelete:
        return Key::Delete;
    case VkHome:
        return Key::Home;
    case VkEnd:
        return Key::End;
    case VkPageUp:
        return Key::PageUp;
    case VkPageDown:
        return Key::PageDown;
    case VkLeftArrow:
        return Key::Left;
    case VkRightArrow:
        return Key::Right;
    case VkUpArrow:
        return Key::Up;
    case VkDownArrow:
        return Key::Down;
    case VkShift:
        return Key::LeftShift;
    case VkRightShift:
        return Key::RightShift;
    case VkControl:
        return Key::LeftControl;
    case VkRightControl:
        return Key::RightControl;
    case VkOption:
        return Key::LeftAlt;
    case VkRightOption:
        return Key::RightAlt;
    case VkCommand:
        return Key::LeftSuper;
    case VkRightCommand:
        return Key::RightSuper;
    case VkMinus:
        return Key::Minus;
    case VkEqual:
        return Key::Equals;
    case VkLeftBracket:
        return Key::LeftBracket;
    case VkRightBracket:
        return Key::RightBracket;
    case VkBackslash:
        return Key::Backslash;
    case VkSemicolon:
        return Key::Semicolon;
    case VkQuote:
        return Key::Apostrophe;
    case VkComma:
        return Key::Comma;
    case VkPeriod:
        return Key::Period;
    case VkSlash:
        return Key::Slash;
    case VkGrave:
        return Key::Grave;
    case VkCapsLock:
        return Key::CapsLock;
    case VkKeypad0:
        return Key::Numpad0;
    case VkKeypad1:
        return Key::Numpad1;
    case VkKeypad2:
        return Key::Numpad2;
    case VkKeypad3:
        return Key::Numpad3;
    case VkKeypad4:
        return Key::Numpad4;
    case VkKeypad5:
        return Key::Numpad5;
    case VkKeypad6:
        return Key::Numpad6;
    case VkKeypad7:
        return Key::Numpad7;
    case VkKeypad8:
        return Key::Numpad8;
    case VkKeypad9:
        return Key::Numpad9;
    case VkKeypadPlus:
        return Key::NumpadAdd;
    case VkKeypadMinus:
        return Key::NumpadSubtract;
    case VkKeypadMultiply:
        return Key::NumpadMultiply;
    case VkKeypadDivide:
        return Key::NumpadDivide;
    case VkKeypadDecimal:
        return Key::NumpadDecimal;
    case VkKeypadEnter:
        return Key::NumpadEnter;
    default:
        return Key::None;
    }
}

uint32 ModifierEvents(uint16 keyCode, uint64 modifierFlags,
                      input::InputEvent (&out)[MaxModifierEvents]) noexcept {
    if (keyCode == VkCapsLock) {
        out[0] = input::InputEvent::KeyDown(Key::CapsLock);
        out[1] = input::InputEvent::KeyUp(Key::CapsLock);
        return MaxModifierEvents;
    }

    for (const ModifierKey& modifier : ModifierKeys) {
        if (modifier.keyCode == keyCode) {
            out[0] = (modifierFlags & modifier.deviceMask) != 0
                         ? input::InputEvent::KeyDown(modifier.key)
                         : input::InputEvent::KeyUp(modifier.key);
            return 1;
        }
    }

    return 0;
}

bool MouseButtonFromNumber(int64 buttonNumber, input::MouseButton& button) noexcept {
    switch (buttonNumber) {
    case LeftButtonNumber:
        button = input::MouseButton::Left;
        return true;
    case RightButtonNumber:
        button = input::MouseButton::Right;
        return true;
    case MiddleButtonNumber:
        button = input::MouseButton::Middle;
        return true;
    case FirstSideButtonNumber:
        button = input::MouseButton::X1;
        return true;
    case SecondSideButtonNumber:
        button = input::MouseButton::X2;
        return true;
    default:
        return false;
    }
}

PixelPoint ViewPointToPixels(double x, double y, double viewHeightPoints,
                             double backingScale) noexcept {
    PixelPoint point;
    point.x = static_cast<float>(x * backingScale);
    point.y = static_cast<float>((viewHeightPoints - y) * backingScale);
    return point;
}

uint32 PointsToPixels(double points, double backingScale) noexcept {
    const double pixels = points * backingScale;
    return pixels <= 0.0 ? 0u : static_cast<uint32>(pixels + RoundingHalf);
}

bool IsTextCodePoint(uint32 codePoint) noexcept {
    if (codePoint < FirstPrintableCodePoint || codePoint == DeleteCodePoint) {
        return false;
    }

    return codePoint < FunctionKeyFirstCodePoint || codePoint > FunctionKeyLastCodePoint;
}

} // namespace rocks::platform::mac
