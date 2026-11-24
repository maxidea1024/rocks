#include "../Private/Common/MacInputMapping.h"

#include <TestSupport/Test.h>

using namespace rocks;
namespace mac_input_mapping_tests_file {

using input::InputEventType;
using input::Key;
namespace mac = platform::mac;

// macOS 입력 변환은 Cocoa 없이 도는 순수 함수라 모든 플랫폼에서
// 시험한다(Docs/Design/MacPlatform.ko.md 5절·7절).

// 물리 배치 대표 키: ANSI 문자(코드가 알파벳 순이 아니다)·숫자 열(5와 6이 뒤바뀐
// 코드)·편집·방향·숫자 패드·Help(= Insert). 표에 없는 코드는 None이다.
TEST(MacInputMapping_VirtualKeyCodesFollowPhysicalLayout) {
    struct Case {
        uint16 code;
        Key key;
    };
    const Case cases[] = {
        {0x00, Key::A},           {0x01, Key::S},         {0x0D, Key::W},
        {0x02, Key::D},           {0x0C, Key::Q},         {0x06, Key::Z},
        {0x12, Key::Digit1},      {0x17, Key::Digit5},    {0x16, Key::Digit6},
        {0x1D, Key::Digit0},      {0x31, Key::Space},     {0x24, Key::Enter},
        {0x33, Key::Backspace},   {0x75, Key::Delete},    {0x35, Key::Escape},
        {0x30, Key::Tab},         {0x32, Key::Grave},     {0x27, Key::Apostrophe},
        {0x72, Key::Insert},      {0x73, Key::Home},      {0x77, Key::End},
        {0x7B, Key::Left},        {0x7E, Key::Up},        {0x7A, Key::F1},
        {0x6F, Key::F12},         {0x52, Key::Numpad0},   {0x5C, Key::Numpad9},
        {0x4C, Key::NumpadEnter}, {0x38, Key::LeftShift}, {0x36, Key::RightSuper},
        {0x39, Key::CapsLock},
    };
    for (const Case& value : cases) {
        CHECK(mac::KeyFromVirtualKeyCode(value.code) == value.key);
    }

    // Function(0x3F)·볼륨(0x48)·F13(0x69)·쓰지 않는 코드는 None이다.
    const uint16 unmapped[] = {0x3F, 0x48, 0x69, 0x0A, 0xFF};
    for (uint16 code : unmapped) {
        CHECK(mac::KeyFromVirtualKeyCode(code) == Key::None);
    }
}

// 서로 다른 kVK 코드가 같은 Key로 겹치지 않는다(None 제외).
TEST(MacInputMapping_VirtualKeyCodesAreOneToOne) {
    constexpr uint32 KeyCodeCount = 0x80;
    bool seen[static_cast<uint32>(Key::Count)] = {};
    for (uint32 code = 0; code < KeyCodeCount; ++code) {
        const Key key = mac::KeyFromVirtualKeyCode(static_cast<uint16>(code));
        if (key == Key::None) {
            continue;
        }

        CHECK(!seen[static_cast<uint32>(key)]);
        seen[static_cast<uint32>(key)] = true;
    }
}

// modifier: 그 키의 장치 비트가 켜졌으면 down, 꺼졌으면 up 하나. 다른 쪽 비트는 보지 않는다.
TEST(MacInputMapping_ModifierEventsUseTheDeviceBitOfTheChangedKey) {
    input::InputEvent events[mac::MaxModifierEvents];
    CHECK(mac::ModifierEvents(0x38, mac::DeviceLeftShiftMask, events) == 1);
    CHECK(events[0].type == InputEventType::KeyDown && events[0].key == Key::LeftShift);

    // 오른쪽 Shift만 눌린 채 왼쪽을 뗐다.
    CHECK(mac::ModifierEvents(0x38, mac::DeviceRightShiftMask, events) == 1);
    CHECK(events[0].type == InputEventType::KeyUp && events[0].key == Key::LeftShift);

    CHECK(mac::ModifierEvents(0x3E, mac::DeviceRightControlMask, events) == 1);
    CHECK(events[0].type == InputEventType::KeyDown && events[0].key == Key::RightControl);
    CHECK(mac::ModifierEvents(0x3A, mac::DeviceLeftOptionMask, events) == 1);
    CHECK(events[0].type == InputEventType::KeyDown && events[0].key == Key::LeftAlt);
    CHECK(mac::ModifierEvents(0x37, 0, events) == 1);
    CHECK(events[0].type == InputEventType::KeyUp && events[0].key == Key::LeftSuper);
}

// CapsLock은 토글 상태라 flagsChanged마다 down과 up 둘이다. Function 등은 0개.
TEST(MacInputMapping_CapsLockMakesAPressAndOthersNothing) {
    input::InputEvent events[mac::MaxModifierEvents];
    const uint64 capsLockStates[] = {0, uint64{1} << 16};
    for (uint64 flags : capsLockStates) {
        CHECK(mac::ModifierEvents(0x39, flags, events) == 2);
        CHECK(events[0].type == InputEventType::KeyDown && events[0].key == Key::CapsLock);
        CHECK(events[1].type == InputEventType::KeyUp && events[1].key == Key::CapsLock);
    }

    CHECK(mac::ModifierEvents(0x3F, ~uint64{0}, events) == 0);
    CHECK(mac::ModifierEvents(0x00, ~uint64{0}, events) == 0);
}

TEST(MacInputMapping_MouseButtonNumbers) {
    input::MouseButton button = input::MouseButton::Count;
    CHECK(mac::MouseButtonFromNumber(0, button) && button == input::MouseButton::Left);
    CHECK(mac::MouseButtonFromNumber(1, button) && button == input::MouseButton::Right);
    CHECK(mac::MouseButtonFromNumber(2, button) && button == input::MouseButton::Middle);
    CHECK(mac::MouseButtonFromNumber(3, button) && button == input::MouseButton::X1);
    CHECK(mac::MouseButtonFromNumber(4, button) && button == input::MouseButton::X2);
    CHECK(!mac::MouseButtonFromNumber(5, button));
    CHECK(!mac::MouseButtonFromNumber(-1, button));
}

// view 좌하단 원점(점)을 client 좌상단 원점(픽셀)으로: Retina 2배, Y 뒤집기.
TEST(MacInputMapping_ViewPointsBecomeTopLeftPixels) {
    const mac::PixelPoint topLeft = mac::ViewPointToPixels(0.0, 360.0, 360.0, 2.0);
    CHECK(topLeft.x == 0.0f && topLeft.y == 0.0f);
    const mac::PixelPoint bottomRight = mac::ViewPointToPixels(640.0, 0.0, 360.0, 2.0);
    CHECK(bottomRight.x == 1280.0f && bottomRight.y == 720.0f);
    const mac::PixelPoint middle = mac::ViewPointToPixels(10.5, 100.0, 360.0, 1.0);
    CHECK(middle.x == 10.5f && middle.y == 260.0f);
    CHECK(mac::PointsToPixels(640.0, 2.0) == 1280u);
    CHECK(mac::PointsToPixels(640.25, 2.0) == 1281u);
    CHECK(mac::PointsToPixels(0.0, 2.0) == 0u);
    CHECK(mac::PointsToPixels(-3.0, 2.0) == 0u);
}

// 문자 입력: 제어 문자·DEL·함수 키 사적 영역은 빼고, 한글·이모지는 넣는다.
TEST(MacInputMapping_TextCodePoints) {
    CHECK(mac::IsTextCodePoint('a'));
    CHECK(mac::IsTextCodePoint(' '));
    CHECK(mac::IsTextCodePoint(0xAC00));
    CHECK(mac::IsTextCodePoint(0x1F600));
    CHECK(mac::IsTextCodePoint(0xF6FF));
    CHECK(mac::IsTextCodePoint(0xF900));
    CHECK(!mac::IsTextCodePoint(0x0D));
    CHECK(!mac::IsTextCodePoint(0x1B));
    CHECK(!mac::IsTextCodePoint(0x7F));
    CHECK(!mac::IsTextCodePoint(0xF700));
    CHECK(!mac::IsTextCodePoint(0xF8FF));
}

} // namespace mac_input_mapping_tests_file
