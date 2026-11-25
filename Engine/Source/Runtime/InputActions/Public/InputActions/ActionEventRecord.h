#pragma once

#include <Base/Guid.h>
#include <InputActions/ActionEvent.h>
#include <InputActions/InputValue.h>

namespace rocks::input {

// 평가 한 번에 이벤트가 난 액션 하나다(Docs/Design/InputActions 4.3절 5 — 전달 5.1절이 쓴다).
struct ActionEventRecord {
    Guid action;
    ActionEvent events = ActionEvent::None;
    InputValue value;
    double elapsedSeconds = 0.0;
    double triggeredSeconds = 0.0;
};

} // namespace rocks::input
