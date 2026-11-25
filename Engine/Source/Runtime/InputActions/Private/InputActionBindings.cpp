#include <InputActions/InputActionBindings.h>

// 액션 이벤트 전달이다(Docs/Design/InputActions 5.1절 — UE EnhancedPlayerInput.cpp:720-925
// EvaluateInputComponentDelegates).
namespace rocks::input {

namespace {

bool BindingsContain(Span<const Guid> values, const Guid& value) noexcept {
    for (const Guid& item : values) {
        if (item == value) {
            return true;
        }
    }

    return false;
}

const ActionEventRecord* BindingsFindEvent(Span<const ActionEventRecord> events,
                                           const Guid& action) noexcept {
    for (const ActionEventRecord& record : events) {
        if (record.action == action) {
            return &record;
        }
    }

    return nullptr;
}

} // namespace

InputActionBindings::InputActionBindings(Allocator& allocator) noexcept
    : allocator_(&allocator), bindings_(allocator), values_(allocator), runList_(allocator),
      triggeredActions_(allocator), consumedActions_(allocator) {
}

Result<ActionBindingHandle> InputActionBindings::BindAction(const Guid& actionId,
                                                            ActionEvent events, Callback&& callback,
                                                            bool consumes) noexcept {
    if (!callback || events == ActionEvent::None) {
        return Result<ActionBindingHandle>::Failure(ErrorCode::InvalidArgument);
    }

    // 전달 작업 배열까지 먼저 잡는다(실패면 그대로 — 전달은 할당하지 않는다).
    const usize count = bindings_.Size() + 1;
    if (auto reserved = bindings_.TryEnsureCapacity(count); !reserved) {
        return Result<ActionBindingHandle>::Failure(reserved.Error());
    }

    if (auto reserved = runList_.TryEnsureCapacity(count); !reserved) {
        return Result<ActionBindingHandle>::Failure(reserved.Error());
    }

    if (auto reserved = triggeredActions_.TryEnsureCapacity(count); !reserved) {
        return Result<ActionBindingHandle>::Failure(reserved.Error());
    }

    if (auto reserved = consumedActions_.TryEnsureCapacity(count); !reserved) {
        return Result<ActionBindingHandle>::Failure(reserved.Error());
    }

    auto binding = UniquePtr<EventBinding>::TryMake(*allocator_);
    if (!binding) {
        return Result<ActionBindingHandle>::Failure(binding.Error());
    }

    EventBinding& entry = *binding.Value();
    entry.id = nextId_++;
    entry.action = actionId;
    entry.events = events;
    entry.callback = Move(callback);
    entry.consumes = consumes;
    const ActionBindingHandle handle{entry.id};
    (void)bindings_.TryPushBack(Move(binding.Value()));
    return Result<ActionBindingHandle>::Success(handle);
}

Result<ActionValueHandle> InputActionBindings::BindActionValue(const Guid& actionId) noexcept {
    ValueBinding binding;
    binding.id = nextId_++;
    binding.action = actionId;
    if (auto pushed = values_.TryPushBack(binding); !pushed) {
        return Result<ActionValueHandle>::Failure(pushed.Error());
    }

    return Result<ActionValueHandle>::Success(ActionValueHandle{binding.id});
}

InputValue InputActionBindings::ActionValue(ActionValueHandle handle) const noexcept {
    for (const ValueBinding& binding : values_) {
        if (binding.id == handle.id) {
            return binding.value;
        }
    }

    return InputValue();
}

void InputActionBindings::RemoveBinding(ActionBindingHandle handle) noexcept {
    for (usize index = 0; index < bindings_.Size(); ++index) {
        if (bindings_[index]->id == handle.id) {
            if (dispatching_) {
                bindings_[index]->removed = true;
            } else {
                (void)bindings_.TryRemoveAt(index);
            }

            return;
        }
    }
}

void InputActionBindings::RemoveValueBinding(ActionValueHandle handle) noexcept {
    for (usize index = 0; index < values_.Size(); ++index) {
        if (values_[index].id == handle.id) {
            (void)values_.TryRemoveAt(index);
            return;
        }
    }
}

void InputActionBindings::ClearBindings() noexcept {
    if (dispatching_) {
        for (UniquePtr<EventBinding>& binding : bindings_) {
            binding->removed = true;
        }
    } else {
        bindings_.Clear();
    }

    values_.Clear();
}

usize InputActionBindings::BindingCount() const noexcept {
    usize count = 0;
    for (const UniquePtr<EventBinding>& binding : bindings_) {
        count += binding->removed ? 0 : 1;
    }

    return count;
}

void InputActionBindings::PurgeRemoved() noexcept {
    for (usize index = bindings_.Size(); index > 0; --index) {
        if (bindings_[index - 1]->removed) {
            (void)bindings_.TryRemoveAt(index - 1);
        }
    }
}

void InputActionBindings::Dispatch(const PlayerActionInput& input) noexcept {
    if (dispatching_) {
        return;
    }

    dispatching_ = true;
    const Span<const ActionEventRecord> events = input.Events();
    runList_.Clear();
    triggeredActions_.Clear();
    consumedActions_.Clear();
    // 실행 목록: 등록 순, Started가 겹치면 앞에 끼워 넣는다(UE EmplaceAt(0) — :801-808).
    for (usize index = 0; index < bindings_.Size(); ++index) {
        const EventBinding& binding = *bindings_[index];
        const ActionEventRecord* record = BindingsFindEvent(events, binding.action);
        if (binding.removed || record == nullptr) {
            continue;
        }

        const ActionEvent matched = record->events & binding.events;
        if (matched == ActionEvent::None) {
            continue;
        }

        if (HasAnyEvent(matched, ActionEvent::Started)) {
            (void)runList_.TryInsert(index, 0);
        } else {
            (void)runList_.TryPushBack(index);
        }

        // Triggered로 전달되는 액션을 실행 전에 모은다(chord 억제 — UE :808-812).
        if (HasAnyEvent(matched, ActionEvent::Triggered) &&
            !BindingsContain(triggeredActions_.View(), binding.action)) {
            (void)triggeredActions_.TryPushBack(binding.action);
        }
    }

    // 실행 목록은 지금 길이까지만 돈다(콜백이 더한 바인딩은 다음 전달부터). chord 짝은 바인딩마다
    // 다시 읽는다(콜백의 즉시 다시 만들기가 목록을 바꾼다 — UE도 delegate마다
    // DependentChordActions를 본다).
    const usize runCount = runList_.Size();
    for (usize at = 0; at < runCount; ++at) {
        EventBinding& binding = *bindings_[runList_[at]];
        const Guid action = binding.action;
        // 앞선 소비 바인딩이 이 액션을 막았다(UE :838-844).
        if (BindingsContain(consumedActions_.View(), action)) {
            continue;
        }

        // chord 억제: 이 액션을 대상으로 하는 chord 액션이 Triggered로 전달되면 막는다(4.4절).
        bool suppressed = false;
        for (const ChordDependency& chord : input.ChordDependencies()) {
            suppressed = suppressed || (chord.target == action &&
                                        BindingsContain(triggeredActions_.View(), chord.source));
        }

        if (suppressed) {
            continue;
        }

        // 기록은 다시 찾는다(앞 콜백이 바꿨을 수 있다 — UE :865). 없으면 실행만 건너뛰고 소비는
        // 건다.
        const bool consumes = binding.consumes;
        const ActionEventRecord* record = BindingsFindEvent(input.Events(), action);
        if (record != nullptr) {
            ActionEventData data;
            data.action = action;
            data.events = record->events & binding.events;
            data.value = record->value;
            data.elapsedSeconds = record->elapsedSeconds;
            data.triggeredSeconds = record->triggeredSeconds;
            binding.callback(data);
        }

        // 소비는 억제되지 않은 바인딩만 건다(UE :863·900-905 — bCanTrigger 블록 안).
        if (consumes && !BindingsContain(consumedActions_.View(), action)) {
            (void)consumedActions_.TryPushBack(action);
        }
    }

    // 값 바인딩: 그 액션의 현재 값, 표에 없으면 0(UE :911-925).
    for (ValueBinding& binding : values_) {
        const ActionStateView state = input.ActionState(binding.action);
        // 표에 없으면 0이고 타입은 마지막 값의 것이다(UE는 액션 타입의 0 — :925).
        binding.value = state.found ? state.value : InputValue(binding.value.type, Vec3d());
    }

    dispatching_ = false;
    PurgeRemoved();
}

} // namespace rocks::input
