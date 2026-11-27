#pragma once

#include <Base/Types.h>

namespace rocks::test {

// 창 펌프 시험용 OS 메시지 조작이다(Docs/Design/UiToolkit 19.2절 펌프 수준 대기 — 시험이 OS 헤더를
// 직접 쓰지 않게 한다). 창 메시지 큐가 스레드에 묶인 플랫폼(Windows)만 뜻이 있고, 다른 플랫폼은
// 지원하지 않음(false)이다.
//
// 사용 예:
//   if (!test::SupportsWindowMessages()) { return; }
//   REQUIRE(test::PostWindowDoubleClick(window->NativeHandle().window, 10, 12));
//   platform::PumpMessages();

// 이 플랫폼이 아래 조작을 지원하는지다.
bool SupportsWindowMessages() noexcept;

// nativeWindow(Windows HWND)에 왼쪽 단추 두 번 누르기 메시지를 게시한다(client 좌표). 게시하지
// 못하면 false다.
bool PostWindowDoubleClick(void* nativeWindow, int32 x, int32 y) noexcept;

// 이 스레드의 메시지 큐에 빈 메시지(깨우기)가 있는지 지우지 않고 본다.
bool HasThreadWakeMessage() noexcept;

// nativeWindow에 DPI 변경 메시지(Windows WM_DPICHANGED)를 동기로 보낸다. 제안 사각형은 창의 지금
// 왼쪽 위에서 width × height(물리 픽셀)다. 보내지 못하면 false다.
bool SendWindowDpiChanged(void* nativeWindow, uint32 dpi, int32 width, int32 height) noexcept;

} // namespace rocks::test
