#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <InputActions/InputValue.h>
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

// RepeatedTap의 "다음 탭 마감 없음"이다(어떤 시계 값보다 과거 — UE RepeatTime 0이 늘 과거인 것과
// 같다).
inline constexpr double RepeatedTapNoDeadline = NumericLimits<double>::Lowest();

// 트리거 하나의 실행 중 상태다(평가기가 바인딩·액션·주입마다 둔다 — 3절).
struct InputTriggerState {
    InputValue lastValue;
    // 시간 트리거의 누적 시간(초)이다(UE HeldDuration).
    double heldDuration = 0.0;
    // Hold: 이미 발사했다(UE bTriggered).
    bool holdTriggered = false;
    // RepeatedTap: 누적 시계(초 — UE는 벽시계, Rocks는 dilation 없는 평가 dt 합 — IAD3), 다음 탭
    // 마감, 진행.
    double clock = 0.0;
    double repeatDeadline = RepeatedTapNoDeadline;
    uint32 tapsSinceTrigger = 0;
    bool awaitingRepeatTap = false;
    // Pulse: 발사 수.
    uint32 pulseCount = 0;
};

// 트리거 평가 문맥이다. deltaSeconds는 dilation을 뺀 dt, timeDilation은 시간 트리거가
// affectedByTimeDilation일 때 곱한다(4.5절). chordStates는 액션 번호마다 이번 평가의 바인딩 수준
// 결합 상태다(3.2절). ChordAction·ChordBlocker는 chordStates[chordAction]을 읽고, 범위 밖이면
// None이다(UE 대상 instance data 없음과 같다).
struct TriggerContext {
    double deltaSeconds = 0.0;
    double timeDilation = 1.0;
    Span<const TriggerState> chordStates = {};
};

// 트리거 하나를 갱신하고 상태를 돌려준다(UE UpdateState). 직전 값은 TriggerCombiner가 갱신한다.
ROCKS_INPUTACTIONS_API TriggerState UpdateInputTrigger(const InputTrigger& trigger,
                                                       InputTriggerState& state,
                                                       const InputValue& value,
                                                       const TriggerContext& context) noexcept;

// 그 상태가 결합을 막는지다(ChordAction은 Triggered가 아니면, ChordBlocker는 Triggered면 — UE
// IsBlocking).
ROCKS_INPUTACTIONS_API bool IsTriggerBlocking(InputTriggerType type, TriggerState state) noexcept;

} // namespace rocks::input
