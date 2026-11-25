#include <InputActions/PlayerActionInput.h>

// 평가 한 번이다(Docs/Design/InputActions 4.3절 — UE EnhancedPlayerInput.cpp:206-281
// ProcessActionMappingEvent, :488-717 PrepareInputDelegatesForEvaluation, :439-480
// EvaluateInputDelegates의 정리, :1021-1066 FlushPressedKeys).
namespace rocks::input {

namespace {

// 디지털 키인지다(UE FKey::IsDigital = !IsAnalog — 키보드·마우스 버튼·패드 버튼·터치, Flush 대상).
bool EvaluateIsDigital(InputKey key) noexcept {
    return key.kind == InputKeyKind::Keyboard || key.kind == InputKeyKind::MouseButton ||
           key.kind == InputKeyKind::GamepadButton || key.kind == InputKeyKind::Touch;
}

// 값의 성분 수다(UE NumComponents = max(1, ValueType)).
uint32 EvaluateComponents(InputValueType type) noexcept {
    const auto components = static_cast<uint32>(type);
    return components == 0 ? 1 : components;
}

double EvaluateAbs(double value) noexcept {
    return value < 0.0 ? -value : value;
}

// 이번 평가 이벤트의 공개 이벤트가 끝(None·Canceled·Completed)인지다(UE ConvertInternalTriggerEvent
// — Started·Triggered가 한 평가면 Triggered).
bool EvaluateEndedEvent(ActionEvent events) noexcept {
    return !HasAnyEvent(events,
                        ActionEvent::Triggered | ActionEvent::Started | ActionEvent::Ongoing);
}

} // namespace

bool PlayerActionInput::LastDown(InputKey key) const noexcept {
    const uint32 slot = InputKeySlot(key);
    return slot < InputKeySlotCount &&
           (lastDownKeys_[slot / KeyBitsPerWord] & (uint64{1} << (slot % KeyBitsPerWord))) != 0;
}

void PlayerActionInput::ResolveChords(Span<InputTrigger> triggers, const Array<ActionEntry>& table,
                                      usize mapped) noexcept {
    for (InputTrigger& trigger : triggers) {
        trigger.chordAction = NoChordAction;
        if (trigger.type != InputTriggerType::ChordAction &&
            trigger.type != InputTriggerType::ChordBlocker) {
            continue;
        }

        for (usize index = 0; index < mapped && index < table.Size(); ++index) {
            if (table[index].id == trigger.chordActionId) {
                trigger.chordAction = static_cast<uint32>(index);
                break;
            }
        }
    }
}

usize PlayerActionInput::FindAction(const Guid& actionId) const noexcept {
    for (usize index = 0; index < actions_.Size(); ++index) {
        if (actions_[index].id == actionId) {
            return index;
        }
    }

    return actions_.Size();
}

void PlayerActionInput::ProcessMapping(ActionEntry& action, usize actionIndex,
                                       const InputValue& raw, Span<const InputTrigger> triggers,
                                       Span<InputTriggerState> triggerStates,
                                       Span<const InputModifier> modifiers,
                                       Span<InputModifierState> modifierStates,
                                       const ModifierContext& modifierContext,
                                       const TriggerContext& triggerContext) noexcept {
    const InputValueType type = action.settings.valueType;
    // 이번 평가 첫 이벤트면 액션 값을 0으로 둔다(UE :222-226).
    if (!action.hadEvent) {
        action.hadEvent = true;
        action.value = InputValue(type, Vec3d());
    }

    InputValue modified(type, raw.value);
    for (usize index = 0; index < modifiers.Size(); ++index) {
        modified =
            ApplyInputModifier(modifiers[index], modifierStates[index], modified, modifierContext);
    }

    TriggerCombiner combiner;
    combiner.Evaluate(triggers, triggerStates, modified, triggerContext);
    combiner.SetStateForNoTriggers(modified.IsNonZero() ? TriggerState::Triggered
                                                        : TriggerState::None);
    combiner.SetMappingTriggerApplied(!triggers.IsEmpty());

    // 정확히 0이 아닐 때만 누적한다(UE :249).
    if (modified.MagnitudeSquared() != 0.0) {
        const uint32 components = EvaluateComponents(type);
        double merged[3] = {action.value.value.x, action.value.value.y, action.value.value.z};
        const double incoming[3] = {modified.value.x, modified.value.y, modified.value.z};
        for (uint32 component = 0; component < components; ++component) {
            if (action.settings.accumulation == ActionAccumulation::Cumulative) {
                merged[component] += incoming[component];
            } else if (EvaluateAbs(incoming[component]) >= EvaluateAbs(merged[component])) {
                merged[component] = incoming[component];
            }
        }

        action.value = InputValue(type, Vec3d(merged[0], merged[1], merged[2]));
    }

    // 가장 진전된 결합을 남긴다(UE FMath::Max — :281, 같으면 새 것).
    if (!(combiner < action.combiner)) {
        action.combiner = combiner;
    }

    if (actionIndex < mappedActions_) {
        chordStates_[actionIndex] = action.combiner.State();
    }
}

void PlayerActionInput::EvaluateInjections(const ModifierContext& modifierContext,
                                           const TriggerContext& triggerContext) noexcept {
    for (ActionEntry& action : actions_) {
        action.injectedThis = false;
    }

    for (const InjectionEntry& injection : injections_) {
        if (!injection.pending.IsEmpty() || injection.continuous) {
            const usize index = FindAction(injection.action);
            if (index < actions_.Size()) {
                actions_[index].injectedThis = true;
            }
        }
    }

    // 이번에 다시 주입하지 않은 액션은 값 0으로 놓는다(트리거·모디파이어 없이 — UE :623-646).
    for (usize index = 0; index < actions_.Size(); ++index) {
        ActionEntry& action = actions_[index];
        if (action.injectedLast && !action.injectedThis) {
            ProcessMapping(action, index, InputValue(), {}, {}, {}, {}, modifierContext,
                           triggerContext);
        }
    }

    // 이번 주입: 넣은 순서대로 각각 처리한다(UE :649-664).
    for (InjectionEntry& injection : injections_) {
        const usize index = FindAction(injection.action);
        if (index >= actions_.Size()) {
            injection.pending.Clear();
            continue;
        }

        ActionEntry& action = actions_[index];
        for (const InputValue& value : injection.pending) {
            ProcessMapping(action, index, value, injection.triggers.View(),
                           injection.triggerStates.View(), injection.modifiers.View(),
                           injection.modifierStates.View(), modifierContext, triggerContext);
        }

        if (injection.continuous) {
            ProcessMapping(action, index, injection.continuousValue, injection.triggers.View(),
                           injection.triggerStates.View(), injection.modifiers.View(),
                           injection.modifierStates.View(), modifierContext, triggerContext);
        }

        injection.pending.Clear();
    }

    for (ActionEntry& action : actions_) {
        action.injectedLast = action.injectedThis;
    }
}

void PlayerActionInput::RecordDownKeys(const InputState& state, uint32 gamepad) noexcept {
    for (uint64& word : lastDownKeys_) {
        word = 0;
    }

    // 직전 눌림은 눌려 있음 또는 이번에 눌림이다(UE KeyDownPrevious = bDownPrevious || IE_Pressed —
    // 한 평가 탭의 다음 평가가 놓기로 평가된다).
    auto record = [&](InputKey key) noexcept {
        const uint32 slot = InputKeySlot(key);
        const KeySample sample = ReadKey(state, gamepad, key);
        if (slot < InputKeySlotCount && (sample.down || sample.pressed)) {
            lastDownKeys_[slot / KeyBitsPerWord] |= uint64{1} << (slot % KeyBitsPerWord);
        }
    };

    for (uint32 code = 1; code < KeyCount; ++code) {
        record(InputKey{InputKeyKind::Keyboard, static_cast<uint16>(code)});
    }

    for (uint32 code = 0; code < MouseButtonCount; ++code) {
        record(InputKey{InputKeyKind::MouseButton, static_cast<uint16>(code)});
    }

    for (uint32 code = 0; code < MouseAxisKeyCount; ++code) {
        record(InputKey{InputKeyKind::MouseAxis, static_cast<uint16>(code)});
    }

    for (uint32 code = 0; code < GamepadButtonCount; ++code) {
        record(InputKey{InputKeyKind::GamepadButton, static_cast<uint16>(code)});
    }

    for (uint32 code = 0; code < GamepadAxisKeyCount; ++code) {
        record(InputKey{InputKeyKind::GamepadAxis, static_cast<uint16>(code)});
    }

    for (uint32 code = 0; code < MaxTouches; ++code) {
        record(InputKey{InputKeyKind::Touch, static_cast<uint16>(code)});
    }
}

Result<void> PlayerActionInput::Evaluate(const InputState& state,
                                         const ActionEvalContext& context) noexcept {
    if (dispatchPending_) {
        FinishDispatch();
    }

    Result<void> outcome = ApplyPendingChanges(state, context.gamepad);
    events_.Clear();
    for (ActionEntry& action : actions_) {
        action.hadEvent = false;
        action.combiner = TriggerCombiner{};
    }

    for (TriggerState& chord : chordStates_) {
        chord = TriggerState::None;
    }

    const ModifierContext modifierContext{context.deltaSeconds, context.fov, context.hasPlayer};
    const TriggerContext triggerContext{context.deltaSeconds, context.timeDilation,
                                        chordStates_.View()};

    // ---- 바인딩 루프(4.3절 2) ----
    for (BindingEntry& binding : bindings_) {
        ActionEntry& action = actions_[binding.action];
        const KeySample sample = ReadKey(state, context.gamepad, binding.key);
        // 무시 중: 눌려 있으면 건너뛰고, 놓였으면 풀고 이번부터 평가한다(UE :575-586).
        if (binding.ignored) {
            // 놓임이 왔으면 같은 평가의 다시 누름도 평가한다(UE InputKey가 IE_Released에서 바로
            // 푼다).
            if (sample.down && !sample.released) {
                continue;
            }

            binding.ignored = false;
        }

        const bool down = sample.down || sample.pressed;
        const bool downLast = LastDown(binding.key);
        if (!down && !downLast && !binding.alwaysTick) {
            continue;
        }

        InputValue raw = sample.raw;
        // 터치(UE bCorrectTouchBoolActionKeys): Boolean은 눌림 1·0, 축은 닿아 있지 않으면 0.
        if (binding.key.IsTouch()) {
            if (action.settings.valueType == InputValueType::Boolean) {
                raw = InputValue::FromBool(down);
            } else if (!sample.down) {
                raw = InputValue(raw.type, Vec3d());
            }
        }

        ProcessMapping(action, binding.action, raw, binding.triggers.View(),
                       binding.triggerStates.View(), binding.modifiers.View(),
                       binding.modifierStates.View(), modifierContext, triggerContext);
    }

    // ---- 주입(4.3절 3) ----
    EvaluateInjections(modifierContext, triggerContext);

    // ---- 액션 마무리(4.3절 4 — UE :671-717) ----
    for (ActionEntry& action : actions_) {
        TriggerState next = TriggerState::None;
        if (action.hadEvent) {
            const InputValue raw = action.value;
            for (usize index = 0; index < action.settings.modifiers.Size(); ++index) {
                action.value =
                    ApplyInputModifier(action.settings.modifiers[index],
                                       action.modifierStates[index], action.value, modifierContext);
            }

            // 트리거 없는 액션의 상태는 값이 바뀌었을 때만 다시 정한다(UE :687-690).
            if (!(action.value.value == raw.value)) {
                action.combiner.SetStateForNoTriggers(
                    action.value.IsNonZero() ? TriggerState::Triggered : TriggerState::None);
            }

            const TriggerState previous = action.combiner.State();
            next =
                action.combiner.Evaluate(action.settings.triggers.View(),
                                         action.triggerStates.View(), action.value, triggerContext);
            if (action.combiner.MappingTriggerApplied() &&
                static_cast<uint8>(previous) < static_cast<uint8>(next)) {
                next = previous;
            }

            if (context.paused && !action.settings.triggerWhenPaused) {
                next = TriggerState::None;
            }
        }

        action.events = TransitionEvent(action.state, next);
        action.state = next;
        action.unmappedFinished = action.unmapped;
        if (next != TriggerState::None) {
            action.elapsedSeconds += context.deltaSeconds;
        }

        if (HasAnyEvent(action.events, ActionEvent::Triggered)) {
            action.triggeredSeconds += context.deltaSeconds;
        }

        if (action.events != ActionEvent::None) {
            ActionEventRecord record;
            record.action = action.id;
            record.events = action.events;
            record.value = action.value;
            record.elapsedSeconds = action.elapsedSeconds;
            record.triggeredSeconds = action.triggeredSeconds;
            if (auto pushed = events_.TryPushBack(record); !pushed && outcome) {
                outcome = Move(pushed);
            }
        }
    }

    RecordDownKeys(state, context.gamepad);
    dispatchPending_ = true;
    return outcome;
}

void PlayerActionInput::FinishDispatch() noexcept {
    dispatchPending_ = false;
    // 끝난 액션의 시간을 0으로 둔다(UE :455-466).
    for (ActionEntry& action : actions_) {
        if (EvaluateEndedEvent(action.events)) {
            action.elapsedSeconds = 0.0;
        }

        if (!HasAnyEvent(action.events, ActionEvent::Triggered)) {
            action.triggeredSeconds = 0.0;
        }
    }

    // 사라진 액션과 끝난 주입 전용 액션을 지운다(뒤쪽 칸이라 바인딩 색인은 그대로다 — 4.2절 8).
    // 주입이 걸린 사라진 액션은 주입 전용으로 바꾼다(주입이 이어진다).
    auto injecting = [this](const Guid& id) noexcept {
        for (const InjectionEntry& injection : injections_) {
            if (injection.action == id && (injection.continuous || !injection.pending.IsEmpty())) {
                return true;
            }
        }

        return false;
    };

    for (usize index = actions_.Size(); index > mappedActions_; --index) {
        ActionEntry& action = actions_[index - 1];
        const bool injected = action.injectedLast || injecting(action.id);
        if (action.unmapped && injected) {
            action.unmapped = false;
            action.unmappedFinished = false;
            action.injectedOnly = true;
            continue;
        }

        bool remove = action.unmapped && action.unmappedFinished;
        if (action.injectedOnly && !injected && action.state == TriggerState::None) {
            remove = true;
        }

        if (remove) {
            (void)actions_.TryRemoveAt(index - 1);
        }
    }

    // 쓰지 않는 주입 항목(넣을 값·연속 주입이 없고 액션이 표에 없거나 놓인 것)을 지운다(해제만).
    for (usize index = injections_.Size(); index > 0; --index) {
        const InjectionEntry& injection = injections_[index - 1];
        if (injection.continuous || !injection.pending.IsEmpty()) {
            continue;
        }

        const usize action = FindAction(injection.action);
        if (action >= actions_.Size() || !actions_[action].injectedLast) {
            (void)injections_.TryRemoveAt(index - 1);
        }
    }
}

ActionStateView PlayerActionInput::ActionState(const Guid& actionId) const noexcept {
    const usize index = FindAction(actionId);
    if (index >= actions_.Size()) {
        return ActionStateView{};
    }

    const ActionEntry& action = actions_[index];
    ActionStateView view;
    view.value = action.value;
    view.state = action.state;
    view.events = action.events;
    view.elapsedSeconds = action.elapsedSeconds;
    view.triggeredSeconds = action.triggeredSeconds;
    view.found = true;
    return view;
}

void PlayerActionInput::Flush() noexcept {
    // 무시와 함께 트리거·모디파이어 상태를 처음으로 둔다(UE FlushPressedKeys가 키 상태를 0으로 둬서
    // 놓을 때 Released·Tap이 늦게 나지 않는 것과 같은 결과 — 4.3절).
    for (BindingEntry& binding : bindings_) {
        if (EvaluateIsDigital(binding.key) && LastDown(binding.key)) {
            binding.ignored = true;
            for (InputTriggerState& state : binding.triggerStates) {
                state = InputTriggerState{};
            }

            for (InputModifierState& state : binding.modifierStates) {
                state = InputModifierState{};
            }
        }
    }
}

} // namespace rocks::input
