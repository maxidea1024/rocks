#pragma once

#include <Base/Guid.h>
#include <InputActions/ActionEvent.h>
#include <InputActions/InputValue.h>

namespace rocks::input {

// 바인딩 콜백이 받는 액션 이벤트다(UE FInputActionInstance — Docs/Design/InputActions 5.1절).
// events는 (이번 이벤트 & 바인딩 마스크)다.
struct ActionEventData {
    Guid action;
    ActionEvent events = ActionEvent::None;
    InputValue value;
    double elapsedSeconds = 0.0;
    double triggeredSeconds = 0.0;
};

} // namespace rocks::input
