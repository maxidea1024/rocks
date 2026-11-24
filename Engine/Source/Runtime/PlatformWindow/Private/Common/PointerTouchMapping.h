#pragma once

#include <Base/Types.h>

// Windows 포인터 메시지(WM_POINTER*)를 터치 동작으로 바꾸는 순수
// 함수다(Docs/Design/TouchInput.ko.md 4.1절). Win32 헤더 없이 값만 다뤄 모든 플랫폼에서 컴파일하고
// EngineTests로 시험한다. Win64 창이 메시지·플래그·압력을 꺼내 넘긴다.
namespace rocks::platform::pointer {

// WM_POINTER 메시지 값이다(WinUser.h).
inline constexpr uint32 PointerUpdateMessage = 0x0245;
inline constexpr uint32 PointerDownMessage = 0x0246;
inline constexpr uint32 PointerUpMessage = 0x0247;
inline constexpr uint32 PointerCaptureChangedMessage = 0x024C;

// POINTER_FLAG_CANCELED다. 뗌이 정상 끝이 아니라 취소다.
inline constexpr uint32 PointerFlagCanceled = 0x00008000;

// POINTER_INPUT_TYPE의 터치·펜 값이다(PT_TOUCH, PT_PEN). 둘 다 터치로 보낸다(UE 기본 — TD6).
inline constexpr uint32 PointerTypeTouch = 2;
inline constexpr uint32 PointerTypePen = 3;

// POINTER_TOUCH_INFO·POINTER_PEN_INFO pressure의 최대값이다(0–1024).
inline constexpr uint32 MaxPointerPressure = 1024;

enum class PointerTouchAction : uint8 {
    None,
    Began,
    Moved,
    Ended,
    Cancelled
};

// 메시지와 pointerFlags가 뜻하는 터치 동작이다. 모르는 메시지는 None이다. capture 상실은 그
// 포인터의 취소다(UE는 처리하지 않지만 눌린 채 남지 않게 더했다 — 4.1절).
PointerTouchAction TouchActionOf(uint32 message, uint32 pointerFlags) noexcept;

// 포인터 종류가 터치로 보낼 대상(터치·펜)인지다. 마우스(PT_MOUSE)는 WM_MOUSE* 경로를 쓴다.
bool IsTouchPointerType(uint32 pointerType) noexcept;

// 0–1024 압력을 0–1로 바꾼다(넘으면 1).
float NormalizePointerPressure(uint32 pressure) noexcept;

} // namespace rocks::platform::pointer
