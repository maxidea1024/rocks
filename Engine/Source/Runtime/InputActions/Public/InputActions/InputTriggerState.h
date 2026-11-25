#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>
#include <InputActions/InputValue.h>

namespace rocks::input {

// RepeatedTap의 "다음 탭 마감 없음"이다(어떤 시계 값보다 과거 — UE RepeatTime 0이 늘 과거인 것과
// 같다).
inline constexpr double RepeatedTapNoDeadline = NumericLimits<double>::Lowest();

// 트리거 하나의 실행 중 상태다(평가기가 바인딩·액션·주입마다 둔다 — 3절).
struct InputTriggerState {
    InputValue lastValue;
    // 시간 트리거의 누적 시간(초)이다(UE HeldDuration).
    double heldDuration = 0.0;
    // Hold: 이미 발사했다(UE bTriggered).
    bool holdTriggered = false;
    // RepeatedTap: 누적 시계(초 — UE는 벽시계, Rocks는 dilation 없는 평가 dt 합 — IAD3), 다음 탭
    // 마감, 진행.
    double clock = 0.0;
    double repeatDeadline = RepeatedTapNoDeadline;
    uint32 tapsSinceTrigger = 0;
    bool awaitingRepeatTap = false;
    // Pulse: 발사 수.
    uint32 pulseCount = 0;
};

} // namespace rocks::input
