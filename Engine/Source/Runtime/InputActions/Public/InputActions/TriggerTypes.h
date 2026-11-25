#pragma once

#include <Base/Types.h>

namespace rocks::input {

// 트리거 상태다(UE ETriggerState — Docs/Design/InputActions 3.2절). 순서가 진전 순서다.
enum class TriggerState : uint8 {
    None,
    Ongoing,
    Triggered
};

// 트리거 결합에서의 역할이다(UE ETriggerType).
enum class TriggerKind : uint8 {
    Explicit,
    Implicit,
    Blocker
};

} // namespace rocks::input
