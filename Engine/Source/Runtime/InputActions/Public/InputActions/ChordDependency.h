#pragma once

#include <Base/Guid.h>

namespace rocks::input {

// chord 짝이다(UE FDependentChordTracker — Docs/Design/InputActions 4.4절). source는 ChordAction
// 트리거를 가진 액션(예: Ctrl+S의 Save), target은 그 대상 액션(Ctrl)이다. source가 Triggered로
// 전달된 평가에서는 target의 전달을 막는다.
struct ChordDependency {
    Guid source;
    Guid target;
};

} // namespace rocks::input
