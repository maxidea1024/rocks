#pragma once

#include <InputActions/Haptics/DynamicHaptic.h>

namespace rocks::input {

// 진행 중인 동적 진동 하나다(UE FDynamicForceFeedbackAction — Docs/Design/InputActions 6.2절).
struct ActiveDynamicHaptic {
    DynamicHapticHandle handle = InvalidDynamicHapticHandle;
    DynamicHaptic details;
    // 시작부터 흐른 게임 시간(초)이다(pause 중에는 멈춘다).
    double elapsed = 0.0;
};

} // namespace rocks::input
