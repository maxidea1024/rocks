#pragma once

#include <Base/Types.h>
#include <InputActions/InputTriggerState.h>
#include <InputActions/InputValue.h>
#include <InputActions/TriggerContext.h>
#include <InputActions/TriggerTypes.h>

namespace rocks::input {

// 트리거 종류다(UE UInputTrigger 파생 — 3.2절, Combo는 5.8 deprecated라 없다).
enum class InputTriggerType : uint8 {
    Down,
    Pressed,
    Released,
    Hold,
    HoldAndRelease,
    Tap,
    RepeatedTap,
    Pulse,
    ChordAction,
    ChordBlocker
};

inline constexpr uint32 InputTriggerTypeCount = 10;

// UE 기본값이다(InputTriggers.h).
inline constexpr float DefaultActuationThreshold = 0.5f;
inline constexpr float DefaultHoldTime = 1.0f;
inline constexpr float DefaultHoldAndReleaseTime = 0.5f;
inline constexpr float DefaultTapReleaseTime = 0.2f;
inline constexpr float DefaultRepeatDelay = 0.5f;
inline constexpr uint32 DefaultRepeatedTapCount = 2;
inline constexpr float DefaultPulseInterval = 1.0f;
// 평가기가 Explicit 트리거 없는 바인딩에 ChordBlocker와 함께 넣는 Down의 임계(UE SMALL_NUMBER)다.
inline constexpr float ChordBlockerDownThreshold = 1.0e-8f;

// 트리거 하나다(종류 + 매개변수). chordAction은 ChordAction·ChordBlocker의 대상 액션
// 번호다(평가기가 액션 표의 색인으로 쓴다 — IA2).
struct InputTrigger {
    InputTriggerType type = InputTriggerType::Down;
    float actuationThreshold = DefaultActuationThreshold;
    // 시간 트리거(Hold·HoldAndRelease·Tap·RepeatedTap·Pulse).
    bool affectedByTimeDilation = false;
    // Hold·HoldAndRelease.
    float holdTime = DefaultHoldTime;
    bool oneShot = false;
    // Tap·RepeatedTap.
    float tapReleaseTime = DefaultTapReleaseTime;
    // RepeatedTap.
    float repeatDelay = DefaultRepeatDelay;
    uint32 tapCount = DefaultRepeatedTapCount;
    // Pulse.
    bool triggerOnStart = true;
    float pulseInterval = DefaultPulseInterval;
    uint32 triggerLimit = 0;
    // ChordAction·ChordBlocker: 대상 액션 번호(TriggerContext::chordStates 색인).
    uint32 chordAction = 0;

    // HoldAndRelease의 UE 기본 holdTime(0.5)으로 만든다.
    static InputTrigger HoldAndReleaseDefault() noexcept {
        InputTrigger trigger;
        trigger.type = InputTriggerType::HoldAndRelease;
        trigger.holdTime = DefaultHoldAndReleaseTime;
        return trigger;
    }
};

// 결합 역할이다(ChordAction은 Implicit, ChordBlocker는 Blocker, 나머지 Explicit — UE).
ROCKS_INPUTACTIONS_API TriggerKind KindOf(InputTriggerType type) noexcept;

// 매 평가 돌아야 하는지다(RepeatedTap — UE bShouldAlwaysTick).
ROCKS_INPUTACTIONS_API bool AlwaysTicks(InputTriggerType type) noexcept;

// 트리거 하나를 갱신하고 상태를 돌려준다(UE UpdateState). 직전 값은 TriggerCombiner가 갱신한다.
ROCKS_INPUTACTIONS_API TriggerState UpdateInputTrigger(const InputTrigger& trigger,
                                                       InputTriggerState& state,
                                                       const InputValue& value,
                                                       const TriggerContext& context) noexcept;

// 그 상태가 결합을 막는지다(ChordAction은 Triggered가 아니면, ChordBlocker는 Triggered면 — UE
// IsBlocking).
ROCKS_INPUTACTIONS_API bool IsTriggerBlocking(InputTriggerType type, TriggerState state) noexcept;

} // namespace rocks::input
