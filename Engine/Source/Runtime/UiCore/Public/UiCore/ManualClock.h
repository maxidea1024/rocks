#pragma once

#include <UiCore/FrameTime.h>

namespace rocks::ui {

// 손으로 올리는 시계다(시험·snapshot — Docs/Design/UiToolkit 20.3절). 읽어도 전진하지 않으므로
// 읽는 횟수가 결과를 바꾸지 않는다. Function()이 돌려준 delegate는 이 객체를 가리키므로, 시계를
// 넘긴 Application보다 오래 살아야 한다.
//
// 사용 예:
//   ManualClock clock;
//   app.SetClock(clock.Function());
//   clock.Advance(1.0 / 60);   // 한 프레임
//   app.BeginFrame();
class ManualClock {
public:
    explicit ManualClock(double start = 0) noexcept : now_(start) {
    }

    double Now() const noexcept {
        return now_;
    }

    // seconds만큼 올린다(음수·비유한 값은 무시한다 — 시계는 줄지 않는다).
    void Advance(double seconds) noexcept {
        if (seconds > 0 && seconds < NumericLimits<double>::Max()) {
            now_ += seconds;
        }
    }

    ClockFunction Function() const noexcept {
        return ClockFunction::CreateLambda([this]() noexcept { return now_; });
    }

private:
    double now_;
};

} // namespace rocks::ui
