#pragma once

#include <Base/Types.h>

// Mac backend 안에서 함께 쓰는 메인 스레드 호스트 상태다(Docs/Design/MacPlatform.ko.md 3절). C++만
// 들어 있어 .mm 파일 어디서나 include한다.
namespace rocks::platform::mac {

// RunCocoaMainThreadHost가 NSApplication run loop를 돌고 있는지다(게임 스레드 본문이 도는 동안
// true). false면 Cocoa 창을 만들 수 없다(콘솔 도구·시험 실행기).
bool IsCocoaHostRunning() noexcept;

// 게임 스레드의 프레임 autorelease pool을 비우고 새로 연다. Window::PumpEvents가 프레임마다 부른다
// (autorelease 객체가 프레임마다 쌓이지 않게). pool은 LIFO라 창마다 두지 않고 게임 스레드에 하나만
// 두며, 호스트가 본문이 끝날 때 닫는다. 호스트가 없거나 메인 스레드에서 부르면 아무것도 하지
// 않는다.
void CycleFrameAutoreleasePool() noexcept;

} // namespace rocks::platform::mac
