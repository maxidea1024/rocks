#include <Base/InitializerList.h>
#include <Input/InputEvent.h>
#include <Input/InputState.h>
#include <InputActions/InputKey.h>
#include <InputActions/InputValue.h>
#include <TestSupport/Test.h>
#include <math.h>
#include <stdio.h>

// 입력 액션 값·키 시험이다(Docs/Design/InputActions 2.1·2.2절 — IA1).
using namespace rocks;
using namespace rocks::input;
namespace input_value_key_tests_file {

namespace {

constexpr double ValueTolerance = 1.0e-9;

bool NearValue(double actual, double expected) noexcept {
    return fabs(actual - expected) <= ValueTolerance;
}

template <usize Count>
void Frame(InputState& state, const InputEvent (&events)[Count]) noexcept {
    state.BeginFrame(Span<const InputEvent>(events, Count));
}

void Frame(InputState& state) noexcept {
    state.BeginFrame({});
}

struct ExpectedKeyName {
    InputKey key;
    const char* name;
};

// 키마다 UE EKeys 이름이다(InputCoreTypes.cpp). 이웃 항목이 바뀌면 잡힌다.
constexpr ExpectedKeyName ExpectedKeyNames[] = {
    {InputKey::FromKey(Key::A), "A"},
    {InputKey::FromKey(Key::B), "B"},
    {InputKey::FromKey(Key::C), "C"},
    {InputKey::FromKey(Key::D), "D"},
    {InputKey::FromKey(Key::E), "E"},
    {InputKey::FromKey(Key::F), "F"},
    {InputKey::FromKey(Key::G), "G"},
    {InputKey::FromKey(Key::H), "H"},
    {InputKey::FromKey(Key::I), "I"},
    {InputKey::FromKey(Key::J), "J"},
    {InputKey::FromKey(Key::K), "K"},
    {InputKey::FromKey(Key::L), "L"},
    {InputKey::FromKey(Key::M), "M"},
    {InputKey::FromKey(Key::N), "N"},
    {InputKey::FromKey(Key::O), "O"},
    {InputKey::FromKey(Key::P), "P"},
    {InputKey::FromKey(Key::Q), "Q"},
    {InputKey::FromKey(Key::R), "R"},
    {InputKey::FromKey(Key::S), "S"},
    {InputKey::FromKey(Key::T), "T"},
    {InputKey::FromKey(Key::U), "U"},
    {InputKey::FromKey(Key::V), "V"},
    {InputKey::FromKey(Key::W), "W"},
    {InputKey::FromKey(Key::X), "X"},
    {InputKey::FromKey(Key::Y), "Y"},
    {InputKey::FromKey(Key::Z), "Z"},
    {InputKey::FromKey(Key::Digit0), "Zero"},
    {InputKey::FromKey(Key::Digit1), "One"},
    {InputKey::FromKey(Key::Digit2), "Two"},
    {InputKey::FromKey(Key::Digit3), "Three"},
    {InputKey::FromKey(Key::Digit4), "Four"},
    {InputKey::FromKey(Key::Digit5), "Five"},
    {InputKey::FromKey(Key::Digit6), "Six"},
    {InputKey::FromKey(Key::Digit7), "Seven"},
    {InputKey::FromKey(Key::Digit8), "Eight"},
    {InputKey::FromKey(Key::Digit9), "Nine"},
    {InputKey::FromKey(Key::F1), "F1"},
    {InputKey::FromKey(Key::F2), "F2"},
    {InputKey::FromKey(Key::F3), "F3"},
    {InputKey::FromKey(Key::F4), "F4"},
    {InputKey::FromKey(Key::F5), "F5"},
    {InputKey::FromKey(Key::F6), "F6"},
    {InputKey::FromKey(Key::F7), "F7"},
    {InputKey::FromKey(Key::F8), "F8"},
    {InputKey::FromKey(Key::F9), "F9"},
    {InputKey::FromKey(Key::F10), "F10"},
    {InputKey::FromKey(Key::F11), "F11"},
    {InputKey::FromKey(Key::F12), "F12"},
    {InputKey::FromKey(Key::Escape), "Escape"},
    {InputKey::FromKey(Key::Enter), "Enter"},
    {InputKey::FromKey(Key::Tab), "Tab"},
    {InputKey::FromKey(Key::Backspace), "BackSpace"},
    {InputKey::FromKey(Key::Space), "SpaceBar"},
    {InputKey::FromKey(Key::Insert), "Insert"},
    {InputKey::FromKey(Key::Delete), "Delete"},
    {InputKey::FromKey(Key::Home), "Home"},
    {InputKey::FromKey(Key::End), "End"},
    {InputKey::FromKey(Key::PageUp), "PageUp"},
    {InputKey::FromKey(Key::PageDown), "PageDown"},
    {InputKey::FromKey(Key::Left), "Left"},
    {InputKey::FromKey(Key::Right), "Right"},
    {InputKey::FromKey(Key::Up), "Up"},
    {InputKey::FromKey(Key::Down), "Down"},
    {InputKey::FromKey(Key::LeftShift), "LeftShift"},
    {InputKey::FromKey(Key::RightShift), "RightShift"},
    {InputKey::FromKey(Key::LeftControl), "LeftControl"},
    {InputKey::FromKey(Key::RightControl), "RightControl"},
    {InputKey::FromKey(Key::LeftAlt), "LeftAlt"},
    {InputKey::FromKey(Key::RightAlt), "RightAlt"},
    {InputKey::FromKey(Key::LeftSuper), "LeftCommand"},
    {InputKey::FromKey(Key::RightSuper), "RightCommand"},
    {InputKey::FromKey(Key::Minus), "Hyphen"},
    {InputKey::FromKey(Key::Equals), "Equals"},
    {InputKey::FromKey(Key::LeftBracket), "LeftBracket"},
    {InputKey::FromKey(Key::RightBracket), "RightBracket"},
    {InputKey::FromKey(Key::Backslash), "Backslash"},
    {InputKey::FromKey(Key::Semicolon), "Semicolon"},
    {InputKey::FromKey(Key::Apostrophe), "Apostrophe"},
    {InputKey::FromKey(Key::Comma), "Comma"},
    {InputKey::FromKey(Key::Period), "Period"},
    {InputKey::FromKey(Key::Slash), "Slash"},
    {InputKey::FromKey(Key::Grave), "Tilde"},
    {InputKey::FromKey(Key::CapsLock), "CapsLock"},
    {InputKey::FromKey(Key::Numpad0), "NumPadZero"},
    {InputKey::FromKey(Key::Numpad1), "NumPadOne"},
    {InputKey::FromKey(Key::Numpad2), "NumPadTwo"},
    {InputKey::FromKey(Key::Numpad3), "NumPadThree"},
    {InputKey::FromKey(Key::Numpad4), "NumPadFour"},
    {InputKey::FromKey(Key::Numpad5), "NumPadFive"},
    {InputKey::FromKey(Key::Numpad6), "NumPadSix"},
    {InputKey::FromKey(Key::Numpad7), "NumPadSeven"},
    {InputKey::FromKey(Key::Numpad8), "NumPadEight"},
    {InputKey::FromKey(Key::Numpad9), "NumPadNine"},
    {InputKey::FromKey(Key::NumpadAdd), "Add"},
    {InputKey::FromKey(Key::NumpadSubtract), "Subtract"},
    {InputKey::FromKey(Key::NumpadMultiply), "Multiply"},
    {InputKey::FromKey(Key::NumpadDivide), "Divide"},
    {InputKey::FromKey(Key::NumpadDecimal), "Decimal"},
    {InputKey::FromKey(Key::NumpadEnter), "NumPadEnter"},
    {InputKey::FromKey(Key::Back), "Android_Back"},
    {InputKey::FromMouseButton(MouseButton::Left), "LeftMouseButton"},
    {InputKey::FromMouseButton(MouseButton::Right), "RightMouseButton"},
    {InputKey::FromMouseButton(MouseButton::Middle), "MiddleMouseButton"},
    {InputKey::FromMouseButton(MouseButton::X1), "ThumbMouseButton"},
    {InputKey::FromMouseButton(MouseButton::X2), "ThumbMouseButton2"},
    {InputKey::FromGamepadButton(GamepadButton::South), "Gamepad_FaceButton_Bottom"},
    {InputKey::FromGamepadButton(GamepadButton::East), "Gamepad_FaceButton_Right"},
    {InputKey::FromGamepadButton(GamepadButton::West), "Gamepad_FaceButton_Left"},
    {InputKey::FromGamepadButton(GamepadButton::North), "Gamepad_FaceButton_Top"},
    {InputKey::FromGamepadButton(GamepadButton::LeftShoulder), "Gamepad_LeftShoulder"},
    {InputKey::FromGamepadButton(GamepadButton::RightShoulder), "Gamepad_RightShoulder"},
    {InputKey::FromGamepadButton(GamepadButton::Back), "Gamepad_Special_Left"},
    {InputKey::FromGamepadButton(GamepadButton::Start), "Gamepad_Special_Right"},
    {InputKey::FromGamepadButton(GamepadButton::LeftThumb), "Gamepad_LeftThumbstick"},
    {InputKey::FromGamepadButton(GamepadButton::RightThumb), "Gamepad_RightThumbstick"},
    {InputKey::FromGamepadButton(GamepadButton::DPadUp), "Gamepad_DPad_Up"},
    {InputKey::FromGamepadButton(GamepadButton::DPadDown), "Gamepad_DPad_Down"},
    {InputKey::FromGamepadButton(GamepadButton::DPadLeft), "Gamepad_DPad_Left"},
    {InputKey::FromGamepadButton(GamepadButton::DPadRight), "Gamepad_DPad_Right"},
    {InputKey::FromMouseAxis(MouseAxisKey::MouseX), "MouseX"},
    {InputKey::FromMouseAxis(MouseAxisKey::MouseY), "MouseY"},
    {InputKey::FromMouseAxis(MouseAxisKey::MouseWheel), "MouseWheelAxis"},
    {InputKey::FromMouseAxis(MouseAxisKey::MouseXY), "Mouse2D"},
    {InputKey::FromGamepadAxis(GamepadAxisKey::LeftX), "Gamepad_LeftX"},
    {InputKey::FromGamepadAxis(GamepadAxisKey::LeftY), "Gamepad_LeftY"},
    {InputKey::FromGamepadAxis(GamepadAxisKey::RightX), "Gamepad_RightX"},
    {InputKey::FromGamepadAxis(GamepadAxisKey::RightY), "Gamepad_RightY"},
    {InputKey::FromGamepadAxis(GamepadAxisKey::LeftTrigger), "Gamepad_LeftTriggerAxis"},
    {InputKey::FromGamepadAxis(GamepadAxisKey::RightTrigger), "Gamepad_RightTriggerAxis"},
    {InputKey::FromGamepadAxis(GamepadAxisKey::LeftStick), "Gamepad_Left2D"},
    {InputKey::FromGamepadAxis(GamepadAxisKey::RightStick), "Gamepad_Right2D"},
    {InputKey::FromTouch(0), "Touch1"},
    {InputKey::FromTouch(1), "Touch2"},
    {InputKey::FromTouch(2), "Touch3"},
    {InputKey::FromTouch(3), "Touch4"},
    {InputKey::FromTouch(4), "Touch5"},
    {InputKey::FromTouch(5), "Touch6"},
    {InputKey::FromTouch(6), "Touch7"},
    {InputKey::FromTouch(7), "Touch8"},
    {InputKey::FromTouch(8), "Touch9"},
    {InputKey::FromTouch(9), "Touch10"},
};

} // namespace

// 2.1절: 타입 지정 생성은 쓰지 않는 성분만 0으로 하고 x는 그대로다(UE InputActionValue.h:49-65).
TEST(InputActionValue_TypedConstructionAndMath) {
    const InputValue boolean(InputValueType::Boolean, Vec3d(0.3, 4.0, 5.0));
    CHECK(NearValue(boolean.value.x, 0.3) && boolean.value.y == 0.0 && boolean.value.z == 0.0);
    CHECK(boolean.AsBool());
    const InputValue planar(InputValueType::Axis2D, Vec3d(3.0, 4.0, 5.0));
    CHECK(planar.value.z == 0.0 && NearValue(planar.Magnitude(), 5.0));
    CHECK(NearValue(planar.MagnitudeSquared(), 25.0));
    const InputValue spatial(InputValueType::Axis3D, Vec3d(1.0, 2.0, 2.0));
    CHECK(NearValue(spatial.Magnitude(), 3.0));
    // 1D·Boolean의 크기는 x(부호 유지)다.
    const InputValue negative(InputValueType::Axis1D, Vec3d(-0.75, 9.0, 0.0));
    CHECK(NearValue(negative.Magnitude(), -0.75) && NearValue(negative.MagnitudeSquared(), 0.5625));
    // ConvertTo는 같은 규칙이다.
    const InputValue narrowed = spatial.ConvertTo(InputValueType::Axis1D);
    CHECK(narrowed.type == InputValueType::Axis1D && narrowed.value.y == 0.0);
    // += 는 성분 합, 타입은 큰 쪽이다.
    InputValue sum(InputValueType::Axis1D, Vec3d(1.0, 0.0, 0.0));
    sum += InputValue(InputValueType::Axis2D, Vec3d(0.5, 2.0, 0.0));
    CHECK(sum.type == InputValueType::Axis2D && NearValue(sum.value.x, 1.5));
    CHECK(NearValue(sum.value.y, 2.0));
    // IsNonZero: 크기² ≥ 1e-8(허용 오차 1e-4).
    CHECK(!InputValue(InputValueType::Axis1D, Vec3d(5.0e-5, 0.0, 0.0)).IsNonZero());
    CHECK(InputValue(InputValueType::Axis1D, Vec3d(1.0e-4, 0.0, 0.0)).IsNonZero());
    CHECK(InputValue::FromBool(true).value.x == 1.0 && !InputValue::FromBool(false).AsBool());
}

// 2.2절: 키 읽기 — 디지털 edge, 한 프레임 눌렀다 뗀 키, 마우스 축 부호, 패드 원시 축, 터치.
TEST(InputActionKey_ReadsInputState) {
    InputState state;
    const InputKey space = InputKey::FromKey(Key::Space);
    Frame(state, {InputEvent::KeyDown(Key::Space)});
    KeySample sample = ReadKey(state, 0, space);
    CHECK(sample.down && sample.pressed && !sample.released && sample.raw.value.x == 1.0);
    CHECK(sample.raw.type == InputValueType::Boolean);
    Frame(state);
    sample = ReadKey(state, 0, space);
    CHECK(sample.down && !sample.pressed);
    Frame(state, {InputEvent::KeyUp(Key::Space)});
    sample = ReadKey(state, 0, space);
    CHECK(!sample.down && sample.released && sample.raw.value.x == 0.0);
    // 한 프레임에 눌렀다 뗐으면 눌림 값 1이다.
    Frame(state, {InputEvent::KeyDown(Key::E), InputEvent::KeyUp(Key::E)});
    sample = ReadKey(state, 0, InputKey::FromKey(Key::E));
    CHECK(!sample.down && sample.pressed && sample.released && sample.raw.value.x == 1.0);

    // 마우스 축: MouseY는 위로 +(화면 delta의 반대).
    Frame(state, {InputEvent::MouseMove(100.0f, 100.0f)});
    Frame(state, {InputEvent::MouseMove(110.0f, 90.0f), InputEvent::MouseWheel(2.0f)});
    CHECK(NearValue(ReadKey(state, 0, InputKey::FromMouseAxis(MouseAxisKey::MouseX)).raw.value.x,
                    10.0));
    CHECK(NearValue(ReadKey(state, 0, InputKey::FromMouseAxis(MouseAxisKey::MouseY)).raw.value.x,
                    10.0));
    const KeySample mouse2d = ReadKey(state, 0, InputKey::FromMouseAxis(MouseAxisKey::MouseXY));
    CHECK(mouse2d.raw.type == InputValueType::Axis2D && NearValue(mouse2d.raw.value.y, 10.0));
    CHECK(NearValue(
        ReadKey(state, 0, InputKey::FromMouseAxis(MouseAxisKey::MouseWheel)).raw.value.x, 2.0));
    Frame(state);
    CHECK(!ReadKey(state, 0, InputKey::FromMouseAxis(MouseAxisKey::MouseX)).down);

    // 패드 축은 dead zone 전 원시 값이다(UE와 같다 — 2.2절 IAD8).
    GamepadSnapshot pad;
    pad.connected = true;
    pad.axes[static_cast<uint32>(GamepadAxis::LeftX)] = 0.1f;
    pad.axes[static_cast<uint32>(GamepadAxis::LeftY)] = -0.2f;
    pad.axes[static_cast<uint32>(GamepadAxis::RightTrigger)] = 0.05f;
    pad.buttons = 1u << static_cast<uint32>(GamepadButton::South);
    Frame(state, {InputEvent::Gamepad(1, pad)});
    CHECK(state.GamepadAxisValue(1, GamepadAxis::LeftX) == 0.0f);
    const KeySample leftX = ReadKey(state, 1, InputKey::FromGamepadAxis(GamepadAxisKey::LeftX));
    CHECK(NearValue(leftX.raw.value.x, static_cast<double>(0.1f)) && leftX.down);
    const KeySample stick = ReadKey(state, 1, InputKey::FromGamepadAxis(GamepadAxisKey::LeftStick));
    CHECK(stick.raw.type == InputValueType::Axis2D &&
          NearValue(stick.raw.value.y, static_cast<double>(-0.2f)));
    CHECK(NearValue(
        ReadKey(state, 1, InputKey::FromGamepadAxis(GamepadAxisKey::RightTrigger)).raw.value.x,
        static_cast<double>(0.05f)));
    const KeySample south = ReadKey(state, 1, InputKey::FromGamepadButton(GamepadButton::South));
    CHECK(south.down && south.pressed);
    // 다른 패드 번호는 아무것도 읽지 않는다.
    CHECK(!ReadKey(state, 0, InputKey::FromGamepadButton(GamepadButton::South)).down);

    // 터치: 닿아 있으면 화면 좌표, 놓으면 0.
    Frame(state, {InputEvent::TouchBegan(2, 30.0f, 40.0f)});
    KeySample touch = ReadKey(state, 0, InputKey::FromTouch(2));
    CHECK(touch.down && touch.pressed && NearValue(touch.raw.value.x, 30.0));
    CHECK(NearValue(touch.raw.value.y, 40.0));
    Frame(state, {InputEvent::TouchEnded(2, 31.0f, 41.0f)});
    touch = ReadKey(state, 0, InputKey::FromTouch(2));
    CHECK(!touch.down && touch.released && touch.raw.value.x == 0.0);

    // 한 프레임에 닿았다 떨어진 터치: 눌림·놓임 둘 다, 값은 마지막 좌표다.
    Frame(state, {InputEvent::TouchBegan(3, 5.0f, 6.0f), InputEvent::TouchEnded(3, 7.0f, 8.0f)});
    touch = ReadKey(state, 0, InputKey::FromTouch(3));
    CHECK(!touch.down && touch.pressed && touch.released && NearValue(touch.raw.value.x, 7.0));

    // Enter는 숫자패드 Enter도 읽는다(UE EKeys::Enter 하나 — 2.2절). 놓임은 둘 다 놓였을 때다.
    const InputKey enter = InputKey::FromKey(Key::Enter);
    Frame(state, {InputEvent::KeyDown(Key::NumpadEnter)});
    sample = ReadKey(state, 0, enter);
    CHECK(sample.down && sample.pressed);
    CHECK(ReadKey(state, 0, InputKey::FromKey(Key::NumpadEnter)).down);
    Frame(state, {InputEvent::KeyDown(Key::Enter)});
    Frame(state, {InputEvent::KeyUp(Key::NumpadEnter)});
    sample = ReadKey(state, 0, enter);
    CHECK(sample.down && !sample.released);
    Frame(state, {InputEvent::KeyUp(Key::Enter)});
    sample = ReadKey(state, 0, enter);
    CHECK(!sample.down && sample.released);
    // 숫자패드 Enter 키 자체는 Enter를 읽지 않는다.
    Frame(state, {InputEvent::KeyDown(Key::Enter)});
    CHECK(!ReadKey(state, 0, InputKey::FromKey(Key::NumpadEnter)).down);

    // 잘못된 키는 빈 표본이다.
    CHECK(!ReadKey(state, 0, InputKey{}).down);
    CHECK(!ReadKey(state, 0, InputKey{InputKeyKind::Touch, MaxTouches}).down);
    // 범위 밖 손가락 번호는 잘리지 않고 잘못된 키다(65536 → Touch1이 아니다).
    CHECK(!InputKey::FromTouch(65536).IsValid() && !InputKey::FromTouch(MaxTouches).IsValid());
}

// 2.2절: 키 이름은 UE EKeys 이름이고 왕복한다(대소문자 무시). 모르는 이름은 NotFound다.
TEST(InputActionKey_NamesRoundTrip) {
    CHECK(InputKeyName(InputKey::FromKey(Key::Space)) == StringView("SpaceBar"));
    CHECK(InputKeyName(InputKey::FromKey(Key::Digit0)) == StringView("Zero"));
    CHECK(InputKeyName(InputKey::FromKey(Key::Grave)) == StringView("Tilde"));
    CHECK(InputKeyName(InputKey::FromMouseAxis(MouseAxisKey::MouseXY)) == StringView("Mouse2D"));
    CHECK(InputKeyName(InputKey::FromGamepadButton(GamepadButton::South)) ==
          StringView("Gamepad_FaceButton_Bottom"));
    CHECK(InputKeyName(InputKey::FromGamepadAxis(GamepadAxisKey::RightStick)) ==
          StringView("Gamepad_Right2D"));
    CHECK(InputKeyName(InputKey::FromTouch(0)) == StringView("Touch1"));
    CHECK(InputKeyName(InputKey{}) == StringView("None"));

    // 전체 기대표.
    for (const ExpectedKeyName& expected : ExpectedKeyNames) {
        if (InputKeyName(expected.key) != StringView(expected.name)) {
            printf("  key %u:%u is %.*s, expected %s\n", static_cast<unsigned>(expected.key.kind),
                   static_cast<unsigned>(expected.key.code),
                   static_cast<int>(InputKeyName(expected.key).ByteCount()),
                   InputKeyName(expected.key).Data(), expected.name);
            CHECK(false);
        }
    }

    CHECK(sizeof(ExpectedKeyNames) / sizeof(ExpectedKeyNames[0]) ==
          (KeyCount - 1) + MouseButtonCount + MouseAxisKeyCount + GamepadButtonCount +
              GamepadAxisKeyCount + MaxTouches);

    uint32 checked = 0;
    const auto roundTrip = [&](InputKey key) noexcept {
        auto parsed = ParseInputKey(InputKeyName(key));
        CHECK(parsed && parsed.Value() == key);
        ++checked;
    };
    for (uint32 code = 1; code < KeyCount; ++code) {
        roundTrip(InputKey{InputKeyKind::Keyboard, static_cast<uint16>(code)});
    }

    for (uint32 code = 0; code < MouseButtonCount; ++code) {
        roundTrip(InputKey{InputKeyKind::MouseButton, static_cast<uint16>(code)});
    }

    for (uint32 code = 0; code < MouseAxisKeyCount; ++code) {
        roundTrip(InputKey{InputKeyKind::MouseAxis, static_cast<uint16>(code)});
    }

    for (uint32 code = 0; code < GamepadButtonCount; ++code) {
        roundTrip(InputKey{InputKeyKind::GamepadButton, static_cast<uint16>(code)});
    }

    for (uint32 code = 0; code < GamepadAxisKeyCount; ++code) {
        roundTrip(InputKey{InputKeyKind::GamepadAxis, static_cast<uint16>(code)});
    }

    for (uint32 code = 0; code < MaxTouches; ++code) {
        roundTrip(InputKey{InputKeyKind::Touch, static_cast<uint16>(code)});
    }

    CHECK(checked == (KeyCount - 1) + MouseButtonCount + MouseAxisKeyCount + GamepadButtonCount +
                         GamepadAxisKeyCount + MaxTouches);
    auto lower = ParseInputKey("gamepad_faceButton_bottom");
    CHECK(lower && lower.Value() == InputKey::FromGamepadButton(GamepadButton::South));
    auto missing = ParseInputKey("Gamepad_LeftTrigger");
    CHECK(!missing && missing.Error() == ErrorCode::NotFound);
    CHECK(!ParseInputKey("None"));
    CHECK(!ParseInputKey(""));

    // 종류 판정.
    CHECK(InputKey::FromGamepadAxis(GamepadAxisKey::LeftX).IsAxis());
    CHECK(InputKey::FromGamepadButton(GamepadButton::Start).IsGamepad());
    CHECK(InputKey::FromTouch(3).IsTouch() && !InputKey::FromTouch(3).IsAxis());
    CHECK(InputKey::FromKey(Key::W).RawType() == InputValueType::Boolean);
    CHECK(InputKey::FromGamepadAxis(GamepadAxisKey::LeftStick).RawType() == InputValueType::Axis2D);
}

} // namespace input_value_key_tests_file
