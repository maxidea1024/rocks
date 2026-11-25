#include <Core/Assert.h>
#include <InputActions/TriggerCombiner.h>

// 트리거 결합이다(Docs/Design/InputActions 3.3절 — UE InputAction.cpp:73-129).
namespace rocks::input {

TriggerState TriggerCombiner::Evaluate(Span<const InputTrigger> triggers,
                                       Span<InputTriggerState> states, const InputValue& value,
                                       const TriggerContext& context) noexcept {
    ROCKS_ASSERT(triggers.Size() == states.Size());
    const usize count = triggers.Size() < states.Size() ? triggers.Size() : states.Size();
    for (usize index = 0; index < count; ++index) {
        const InputTrigger& trigger = triggers[index];
        InputTriggerState& state = states[index];
        evaluatedTriggers_ = true;
        const TriggerState current = UpdateInputTrigger(trigger, state, value, context);
        // 직전 값은 결합기가 갱신한다(UE Trigger->LastValue = ModifiedValue).
        state.lastValue = value;
        switch (KindOf(trigger.type)) {
        case TriggerKind::Explicit:
            foundExplicit_ = true;
            anyExplicitTriggered_ = anyExplicitTriggered_ || current == TriggerState::Triggered;
            foundActive_ = foundActive_ || current != TriggerState::None;
            break;
        case TriggerKind::Implicit:
            allImplicitsTriggered_ = allImplicitsTriggered_ && current == TriggerState::Triggered;
            foundActive_ = foundActive_ || current != TriggerState::None;
            break;
        case TriggerKind::Blocker:
            // Blocker는 active를 세우지 않는다(UE :106-108).
            break;
        }

        blocking_ = blocking_ || IsTriggerBlocking(trigger.type, current);
    }

    return State();
}

TriggerState TriggerCombiner::State() const noexcept {
    if (!evaluatedTriggers_) {
        return noTriggerState_;
    }

    if (blocking_) {
        return TriggerState::None;
    }

    if ((!foundExplicit_ || anyExplicitTriggered_) && allImplicitsTriggered_) {
        return TriggerState::Triggered;
    }

    return foundActive_ ? TriggerState::Ongoing : TriggerState::None;
}

bool TriggerCombiner::operator<(const TriggerCombiner& other) const noexcept {
    const auto mine = static_cast<uint8>(State());
    const auto theirs = static_cast<uint8>(other.State());
    if (mine != theirs) {
        return mine < theirs;
    }

    return !evaluatedTriggers_ && other.evaluatedTriggers_;
}

} // namespace rocks::input
