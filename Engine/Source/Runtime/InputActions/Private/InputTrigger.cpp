#include <InputActions/InputTrigger.h>

// 트리거다(Docs/Design/InputActions 3.2절 — UE InputTriggers.cpp).
namespace rocks::input {

namespace {

// |value|² ≥ thr²다(UE IsActuated — InputTriggers.h:143).
bool IsTriggerActuated(const InputTrigger& trigger, const InputValue& value) noexcept {
    const double threshold = static_cast<double>(trigger.actuationThreshold);
    return value.MagnitudeSquared() >= threshold * threshold;
}

// 시간 트리거의 이번 평가 dt다(UE CalculateHeldDuration — cpp:140-148).
double TriggerTimedDelta(const InputTrigger& trigger, const TriggerContext& context) noexcept {
    return trigger.affectedByTimeDilation ? context.deltaSeconds * context.timeDilation
                                          : context.deltaSeconds;
}

// UE UInputTriggerTimedBase::UpdateState(cpp:114-131).
TriggerState TriggerTimedBase(const InputTrigger& trigger, InputTriggerState& state,
                              const InputValue& value, const TriggerContext& context) noexcept {
    if (IsTriggerActuated(trigger, value)) {
        state.heldDuration += TriggerTimedDelta(trigger, context);
        return TriggerState::Ongoing;
    }

    state.heldDuration = 0.0;
    return TriggerState::None;
}

TriggerState UpdateRepeatedTap(const InputTrigger& trigger, InputTriggerState& state,
                               const InputValue& value, const TriggerContext& context) noexcept {
    // UE는 FPlatformTime::Seconds()다(cpp:270). Rocks는 매 평가 dt를 더한 시계다(IAD3). 벽시계처럼
    // dilation을 받지 않는다.
    state.clock += context.deltaSeconds;
    const double lastHeld = state.heldDuration;
    TriggerState result = TriggerTimedBase(trigger, state, value, context);
    const bool singleTap = IsTriggerActuated(trigger, state.lastValue) &&
                           result == TriggerState::None &&
                           lastHeld < static_cast<double>(trigger.tapReleaseTime);
    const bool withinRepeat = state.clock <= state.repeatDeadline;
    // 탭 사이를 Ongoing으로 이어 둔다(UE cpp:279-288).
    if (state.tapsSinceTrigger > 0 || state.awaitingRepeatTap || singleTap) {
        result = TriggerState::Ongoing;
    }

    if (!withinRepeat && (state.tapsSinceTrigger > 0 || state.awaitingRepeatTap)) {
        result = TriggerState::None;
    } else if (state.heldDuration >= static_cast<double>(trigger.tapReleaseTime)) {
        result = TriggerState::None;
    } else if (singleTap && withinRepeat) {
        ++state.tapsSinceTrigger;
        const uint32 needed = trigger.tapCount > 0 ? trigger.tapCount - 1 : 0;
        result = state.tapsSinceTrigger >= needed ? TriggerState::Triggered : TriggerState::Ongoing;
    }

    if (singleTap) {
        state.repeatDeadline = state.clock + static_cast<double>(trigger.repeatDelay);
        state.awaitingRepeatTap = true;
    }

    if (result == TriggerState::None || result == TriggerState::Triggered) {
        state.tapsSinceTrigger = 0;
        state.awaitingRepeatTap = false;
    }

    return result;
}

} // namespace

TriggerKind KindOf(InputTriggerType type) noexcept {
    switch (type) {
    case InputTriggerType::ChordAction:
        return TriggerKind::Implicit;
    case InputTriggerType::ChordBlocker:
        return TriggerKind::Blocker;
    default:
        return TriggerKind::Explicit;
    }
}

bool AlwaysTicks(InputTriggerType type) noexcept {
    return type == InputTriggerType::RepeatedTap;
}

bool IsTriggerBlocking(InputTriggerType type, TriggerState state) noexcept {
    switch (type) {
    case InputTriggerType::ChordAction:
        return state != TriggerState::Triggered;
    case InputTriggerType::ChordBlocker:
        return state == TriggerState::Triggered;
    default:
        return false;
    }
}

TriggerState UpdateInputTrigger(const InputTrigger& trigger, InputTriggerState& state,
                                const InputValue& value, const TriggerContext& context) noexcept {
    switch (trigger.type) {
    case InputTriggerType::Down:
        return IsTriggerActuated(trigger, value) ? TriggerState::Triggered : TriggerState::None;
    case InputTriggerType::Pressed:
        return IsTriggerActuated(trigger, value) && !IsTriggerActuated(trigger, state.lastValue)
                   ? TriggerState::Triggered
                   : TriggerState::None;
    case InputTriggerType::Released:
        if (IsTriggerActuated(trigger, value)) {
            return TriggerState::Ongoing;
        }

        return IsTriggerActuated(trigger, state.lastValue) ? TriggerState::Triggered
                                                           : TriggerState::None;
    case InputTriggerType::Hold: {
        const TriggerState base = TriggerTimedBase(trigger, state, value, context);
        const bool first = !state.holdTriggered;
        state.holdTriggered = state.heldDuration >= static_cast<double>(trigger.holdTime);
        if (state.holdTriggered) {
            return first || !trigger.oneShot ? TriggerState::Triggered : TriggerState::None;
        }

        return base;
    }

    case InputTriggerType::HoldAndRelease: {
        // 놓는 평가에서도 이번 dt를 더한 시간으로 본다(UE cpp:212-214).
        const double tickHeld = state.heldDuration + TriggerTimedDelta(trigger, context);
        TriggerState base = TriggerTimedBase(trigger, state, value, context);
        if (tickHeld >= static_cast<double>(trigger.holdAndReleaseTime) &&
            base == TriggerState::None) {
            base = TriggerState::Triggered;
        }

        return base;
    }

    case InputTriggerType::Tap: {
        const double lastHeld = state.heldDuration;
        TriggerState base = TriggerTimedBase(trigger, state, value, context);
        if (IsTriggerActuated(trigger, state.lastValue) && base == TriggerState::None &&
            lastHeld < static_cast<double>(trigger.tapReleaseTime)) {
            base = TriggerState::Triggered;
        } else if (state.heldDuration >= static_cast<double>(trigger.tapReleaseTime)) {
            base = TriggerState::None;
        }

        return base;
    }

    case InputTriggerType::RepeatedTap:
        return UpdateRepeatedTap(trigger, state, value, context);
    case InputTriggerType::Pulse: {
        TriggerState base = TriggerTimedBase(trigger, state, value, context);
        if (base != TriggerState::Ongoing) {
            state.pulseCount = 0;
            return base;
        }

        if (trigger.triggerLimit != 0 && state.pulseCount >= trigger.triggerLimit) {
            return TriggerState::None;
        }

        const uint32 step = trigger.triggerOnStart ? state.pulseCount : state.pulseCount + 1;
        if (state.heldDuration >
            static_cast<double>(trigger.pulseInterval) * static_cast<double>(step)) {
            ++state.pulseCount;
            return TriggerState::Triggered;
        }

        return base;
    }

    case InputTriggerType::ChordAction:
    case InputTriggerType::ChordBlocker:
        // 대상 액션의 결합 상태를 따른다(UE cpp:393-398). 대상이 없으면 None이다.
        return trigger.chordAction < context.chordStates.Size()
                   ? context.chordStates[trigger.chordAction]
                   : TriggerState::None;
    }

    return TriggerState::None;
}

} // namespace rocks::input
