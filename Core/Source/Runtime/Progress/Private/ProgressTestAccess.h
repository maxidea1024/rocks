#pragma once

#include <Progress/ProgressHub.h>

// 시험 접근점이다(PolicyChecks test-access — 모듈 시험만 include한다). 닫힌 hub를 "켜기 전"으로
// 되돌려 NotReady 오류(Docs/Design/ProgressReporting 5.2절)를 한 프로세스 안에서 시험한다.
namespace rocks {

class ProgressTestAccess {
public:
    static void ReturnToUninitialized() noexcept {
        ProgressHub::Get().ReturnToUninitialized();
    }
};

} // namespace rocks
