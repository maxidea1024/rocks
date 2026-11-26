#pragma once

#include <Base/Types.h>

namespace rocks::input {

// 동적 진동의 handle이다(플레이어마다 1부터 는다 — Docs/Design/InputActions 6.2절).
using DynamicHapticHandle = uint64;

// 무효 handle이다(StartDynamicHaptic 실패·아직 시작하지 않음).
inline constexpr DynamicHapticHandle InvalidDynamicHapticHandle = 0;

// 멈출 때까지 계속되는 동적 진동의 duration이다(음수면 끝이 없다 — UE 기본값 -1).
inline constexpr float UnlimitedHapticDuration = -1.0f;

// 코드가 세기를 직접 정하는 진동이다(UE FDynamicForceFeedbackDetails·PlayDynamicForceFeedback).
// intensity를 affects* 모터에 최댓값으로 합친다. duration(초)이 음수면 StopDynamicHaptic까지
// 계속된다.
struct DynamicHaptic {
    float intensity = 1.0f;
    float duration = UnlimitedHapticDuration;
    bool affectsLeftLarge = true;
    bool affectsLeftSmall = true;
    bool affectsRightLarge = true;
    bool affectsRightSmall = true;
};

} // namespace rocks::input
