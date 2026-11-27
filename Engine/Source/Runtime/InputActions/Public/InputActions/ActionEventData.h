#pragma once

#include <Base/Guid.h>
#include <InputActions/ActionEvent.h>
#include <InputActions/InputValue.h>
#include <Meta/Annotations.h>

namespace rocks::input {

// 바인딩 콜백·함수가 받는 액션 이벤트다(UE FInputActionInstance — Docs/Design/InputActions 5.1절).
// events는 (이번 이벤트 & 바인딩 마스크)다. 리플렉션 struct다(함수 바인딩의 호출 frame 칸 — 강한
// 참조를 담지 않는다).
ROCKS_STRUCT()
struct ActionEventData {
    ROCKS_GENERATED_BODY()

    // 액션 id다.
    ROCKS_PROPERTY() Guid action;
    // 이번 평가의 이벤트 & 바인딩 마스크다.
    ROCKS_PROPERTY() ActionEvent events = ActionEvent::None;
    // 액션의 이번 값이다.
    ROCKS_PROPERTY() InputValue value;
    // 액션이 시작된 뒤 지난 시간(초)이다.
    ROCKS_PROPERTY() double elapsedSeconds = 0.0;
    // Triggered로 지난 시간(초)이다.
    ROCKS_PROPERTY() double triggeredSeconds = 0.0;
};

} // namespace rocks::input
