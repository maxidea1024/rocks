#pragma once

#include <Base/Guid.h>
#include <Base/NumericLimits.h>
#include <Base/Types.h>
#include <InputActions/InputTriggerState.h>
#include <InputActions/InputValue.h>
#include <InputActions/TriggerContext.h>
#include <InputActions/TriggerTypes.h>
#include <Meta/Annotations.h>

namespace rocks::input {

// 트리거 종류다(UE UInputTrigger 파생 — 3.2절, Combo는 5.8 deprecated라 없다).
ROCKS_ENUM()
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
// chord 대상 색인 "없음"이다(액션 표 밖 — 대상 상태 None).
inline constexpr uint32 NoChordAction = NumericLimits<uint32>::Max();

// 트리거 하나다(종류 + 매개변수 — 3.2절). 매개변수는 그 종류에만 쓰인다. 리플렉션
// struct다(바인딩·액션 데이터 — 2.4절). chordActionId는 ChordAction·ChordBlocker의 대상 액션
// id(데이터)이고, chordAction은 평가기가 다시 만들 때 쓰는 액션 표 색인(실행 중 — 저장하지
// 않는다)이다.
ROCKS_STRUCT()
struct InputTrigger {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() InputTriggerType type = InputTriggerType::Down;
    ROCKS_PROPERTY() float actuationThreshold = DefaultActuationThreshold;
    // 시간 트리거(Hold·HoldAndRelease·Tap·RepeatedTap·Pulse).
    ROCKS_PROPERTY() bool affectedByTimeDilation = false;
    // Hold.
    ROCKS_PROPERTY() float holdTime = DefaultHoldTime;
    ROCKS_PROPERTY() bool oneShot = false;
    // HoldAndRelease(UE는 Hold와 다른 클래스라 기본값이 다르다 — 0.5).
    ROCKS_PROPERTY() float holdAndReleaseTime = DefaultHoldAndReleaseTime;
    // Tap·RepeatedTap.
    ROCKS_PROPERTY() float tapReleaseTime = DefaultTapReleaseTime;
    // RepeatedTap.
    ROCKS_PROPERTY() float repeatDelay = DefaultRepeatDelay;
    ROCKS_PROPERTY() uint32 tapCount = DefaultRepeatedTapCount;
    // Pulse.
    ROCKS_PROPERTY() bool triggerOnStart = true;
    ROCKS_PROPERTY() float pulseInterval = DefaultPulseInterval;
    ROCKS_PROPERTY() uint32 triggerLimit = 0;
    // ChordAction·ChordBlocker.
    ROCKS_PROPERTY() Guid chordActionId;
    // TriggerContext::chordStates 색인이다(평가기가 쓴다).
    ROCKS_NATIVE() uint32 chordAction = NoChordAction;
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
