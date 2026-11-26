#pragma once

#include <Core/Memory/Allocator.h>

namespace sandbox {

// 시작 화면(스플래시) 창 연기 시험이다(Docs/Design/SplashScreen 14절 시험 10). 실제 Windows
// presenter로 Show(최소 표시 0, Custom 칸 하나) → Shown → SetText·Suspend·Resume → Hide를
// SplashSelfTestRounds번 되풀이해 창 클래스 등록·해제와 스플래시 스레드 수명을 실제 OS에서
// 확인한다. 창을 띄우므로 대화형 데스크톱이 필요하다(PlatformWindow 창 시험과 같은 관례로 모듈
// 시험이 아니라 Sandbox에 둔다).
//
// 종료 코드: 0 = 통과, 1 = 실패, 2 = 판정 불가(데스크톱 세션이 떨어졌거나, 이 플랫폼에 구현이
// 없거나, 창을 만들 수 없음 — 원인을 로그에 남긴다).
int RunSplashSelfTest(rocks::Allocator& allocator) noexcept;

} // namespace sandbox
