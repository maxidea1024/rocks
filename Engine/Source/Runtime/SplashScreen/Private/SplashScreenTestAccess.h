#pragma once

#include "SplashPresenter.h"

#include <SplashScreen/SplashScreen.h>

// 시험 접근점이다(PolicyChecks test-access — 모듈 시험만 include한다). 가짜 presenter로 Show를 불러
// OS 창 없이 스레드 계약(Hide 대기·release·상태)을 시험한다(Docs/Design/SplashScreen 14절 시험 8).
namespace rocks::splash {

class SplashScreenTestAccess {
public:
    static Result<SplashScreen> Show(Allocator& allocator, const SplashShowDesc& desc,
                                     const SplashPresenterFactory& factory) noexcept {
        return SplashScreen::ShowWith(allocator, desc, factory);
    }
};

} // namespace rocks::splash
