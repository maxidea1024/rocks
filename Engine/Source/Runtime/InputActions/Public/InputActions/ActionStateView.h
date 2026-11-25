#pragma once

#include <InputActions/ActionEvent.h>
#include <InputActions/InputValue.h>
#include <InputActions/TriggerTypes.h>

namespace rocks::input {

// 액션 하나의 상태 보기다(UE FInputActionInstance — Docs/Design/InputActions 4.3절 5). found가
// 거짓이면(표에 없는 액션) 나머지는 기본값이다.
struct ActionStateView {
    InputValue value;
    TriggerState state = TriggerState::None;
    // 마지막 평가의 이벤트다.
    ActionEvent events = ActionEvent::None;
    // 상태가 None이 아닌 동안 누적한 시간과 Triggered 이벤트 동안 누적한 시간(초)이다.
    double elapsedSeconds = 0.0;
    double triggeredSeconds = 0.0;
    bool found = false;
};

} // namespace rocks::input
