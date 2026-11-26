#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Span.h>
#include <InputActions/InputKey.h>
#include <InputActions/InputModifier.h>
#include <InputActions/InputTrigger.h>

namespace rocks::input {

// 평가기가 다시 만든 바인딩 하나의 보기다(진단·시험 — Docs/Design/InputActions 4.1절). triggers에는
// 복제된 액션 수준 chord, 넣은 Down·ChordBlocker가 들어 있다(4.2절 4·5). Span은 다음 다시
// 만들기까지 유효하다.
struct InputBindingView {
    Guid action;
    Guid context;
    InputKey key;
    Span<const InputTrigger> triggers;
    Span<const InputModifier> modifiers;
    // 키를 놓을 때까지 무시 중이다(4.2절 6).
    bool ignored = false;
};

} // namespace rocks::input
