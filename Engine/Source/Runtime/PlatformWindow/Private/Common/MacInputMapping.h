#pragma once

#include <Input/InputEvent.h>

// macOS 입력 변환의 순수 함수다(Docs/Design/MacPlatform.ko.md 5절). Cocoa 없이 도는 C++라 모든
// 플랫폼에서 컴파일하고, Windows에서도 EngineTests로 시험한다. Mac backend(Private/Mac)가 NSEvent
// 값을 꺼내 이 함수들에 넘긴다.
namespace rocks::platform::mac {

// flagsChanged 한 번이 만드는 이벤트 수의 상한이다(CapsLock은 down과 up 둘).
inline constexpr uint32 MaxModifierEvents = 2;

// NSEvent modifierFlags의 장치 의존 비트다(IOKit IOLLEvent.h NX_DEVICE*KEYMASK와 같은 값 — UE
// MacApplication.cpp도 이름 대신 비트 값을 쓴다). 왼쪽·오른쪽 modifier를 가른다.
inline constexpr uint64 DeviceLeftControlMask = 0x00000001;
inline constexpr uint64 DeviceLeftShiftMask = 0x00000002;
inline constexpr uint64 DeviceRightShiftMask = 0x00000004;
inline constexpr uint64 DeviceLeftCommandMask = 0x00000008;
inline constexpr uint64 DeviceRightCommandMask = 0x00000010;
inline constexpr uint64 DeviceLeftOptionMask = 0x00000020;
inline constexpr uint64 DeviceRightOptionMask = 0x00000040;
inline constexpr uint64 DeviceRightControlMask = 0x00002000;

// 함수 키 사적 영역(NSUpArrowFunctionKey 등 U+F700–U+F8FF)이다. 문자 입력에서 뺀다.
inline constexpr uint32 FunctionKeyFirstCodePoint = 0xF700;
inline constexpr uint32 FunctionKeyLastCodePoint = 0xF8FF;

// 하드웨어 키코드(kVK_*, Carbon HIToolbox/Events.h — 물리 위치)를 input::Key로 바꾼다. Windows
// 스캔코드 표와 같은 물리 배치 의미다. 표에 없는 키(Function·볼륨·F13 이상 등)는 Key::None이다.
input::Key KeyFromVirtualKeyCode(uint16 keyCode) noexcept;

// flagsChanged 이벤트의 keyCode와 바뀐 뒤 modifierFlags로 modifier 키 이벤트를 out에 쓰고 그 수를
// 돌려준다.
// - Shift·Control·Option·Command: 그 키의 장치 의존 비트가 켜져 있으면 KeyDown, 꺼져 있으면 KeyUp
//   하나다(왼쪽·오른쪽은 keyCode가 가른다).
// - CapsLock: flagsChanged가 물리 down·up이 아니라 토글 상태를 알리므로 KeyDown과 KeyUp 둘을
// 만든다.
// - 그 밖(Function 등): 0개.
uint32 ModifierEvents(uint16 keyCode, uint64 modifierFlags,
                      input::InputEvent (&out)[MaxModifierEvents]) noexcept;

// NSEvent buttonNumber(0 왼쪽, 1 오른쪽, 2 가운데, 3·4 옆 버튼)를 input::MouseButton으로 바꾼다. 그
// 밖의 번호는 false다.
bool MouseButtonFromNumber(int64 buttonNumber, input::MouseButton& button) noexcept;

// client 영역 픽셀 좌표(좌상단 원점)다.
struct PixelPoint {
    float x = 0.0f;
    float y = 0.0f;
};

// view 좌표(점, 좌하단 원점)를 client 픽셀(좌상단 원점)로 바꾼다. viewHeightPoints는 view 높이(점),
// backingScale은 창의 backingScaleFactor(Retina 2.0)다. Windows client 픽셀과 같은 의미다.
PixelPoint ViewPointToPixels(double x, double y, double viewHeightPoints,
                             double backingScale) noexcept;

// 점 크기를 픽셀로 바꾼다(반올림). 창 크기·client 크기에 쓴다.
uint32 PointsToPixels(double points, double backingScale) noexcept;

// [NSEvent characters]의 code point를 Text 이벤트로 넣을지다. 제어 문자(0x20 미만·0x7F)와 함수 키
// 사적 영역(U+F700–U+F8FF)은 키 이벤트로만 처리하므로 뺀다.
bool IsTextCodePoint(uint32 codePoint) noexcept;

} // namespace rocks::platform::mac
