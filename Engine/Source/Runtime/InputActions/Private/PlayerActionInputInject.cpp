#include <InputActions/InputActionSettings.generated.h>
#include <InputActions/InputContextCollect.h>
#include <InputActions/InputModifier.generated.h>
#include <InputActions/InputTrigger.generated.h>
#include <InputActions/PlayerActionInput.h>
#include <Object/ValueCopy.h>

// 주입 입력이다(Docs/Design/InputActions 4.3절 3 — UE EnhancedPlayerInput.cpp:283-291
// InjectInputForAction, EnhancedInputSubsystemInterface.cpp ContinuousInjection).
namespace rocks::input {

namespace {

// 트리거 종류 열·모디파이어 종류 열이 같은지다(주입 상태의 열쇠).
bool InjectSameShape(const Array<InputTrigger>& triggers, const Array<InputModifier>& modifiers,
                     Span<const InputTrigger> otherTriggers,
                     Span<const InputModifier> otherModifiers) noexcept {
    if (triggers.Size() != otherTriggers.Size() || modifiers.Size() != otherModifiers.Size()) {
        return false;
    }

    for (usize index = 0; index < triggers.Size(); ++index) {
        if (triggers[index].type != otherTriggers[index].type) {
            return false;
        }
    }

    for (usize index = 0; index < modifiers.Size(); ++index) {
        if (modifiers[index].kind != otherModifiers[index].kind) {
            return false;
        }
    }

    return true;
}

Result<void> InjectCopyElements(Array<InputTrigger>& triggers, Array<InputModifier>& modifiers,
                                Span<const InputTrigger> sourceTriggers,
                                Span<const InputModifier> sourceModifiers,
                                Allocator& allocator) noexcept {
    if (auto reserved = triggers.TryReserve(sourceTriggers.Size()); !reserved) {
        return reserved;
    }

    if (auto reserved = modifiers.TryReserve(sourceModifiers.Size()); !reserved) {
        return reserved;
    }

    for (const InputTrigger& trigger : sourceTriggers) {
        (void)triggers.TryPushBack(trigger);
    }

    for (const InputModifier& modifier : sourceModifiers) {
        InputModifier copy(allocator);
        if (auto copied = copy.TryCopyFrom(modifier); !copied) {
            return copied;
        }

        (void)modifiers.TryPushBack(Move(copy));
    }

    return Result<void>::Success();
}

} // namespace

Result<void> PlayerActionInput::Inject(const InputActionData& action, const InputValue& value,
                                       Span<const InputTrigger> triggers,
                                       Span<const InputModifier> modifiers,
                                       bool continuous) noexcept {
    if (action.id == Guid{}) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto validated = ValidateInputActionSettings(action.settings); !validated) {
        return validated;
    }

    if (auto validated = ValidateInputElements(triggers, modifiers); !validated) {
        return validated;
    }

    Allocator& allocator = *allocator_;
    // 새 칸은 따로 다 만든 뒤 넣는다(실패면 상태 그대로).
    const bool hasAction = FindAction(action.id) < actions_.Size();
    usize found = injections_.Size();
    for (usize index = 0; index < injections_.Size(); ++index) {
        const InjectionEntry& entry = injections_[index];
        if (entry.action == action.id &&
            InjectSameShape(entry.triggers, entry.modifiers, triggers, modifiers)) {
            found = index;
            break;
        }
    }

    ActionEntry newAction(allocator);
    if (!hasAction) {
        newAction.id = action.id;
        newAction.injectedOnly = true;
        if (auto copied = detail::TryCopyValue(newAction.settings, action.settings, allocator);
            !copied) {
            return copied;
        }

        ResolveChords(newAction.settings.triggers.View(), actions_, mappedActions_);

        if (auto sized = newAction.triggerStates.TryResize(newAction.settings.triggers.Size());
            !sized) {
            return sized;
        }

        if (auto sized = newAction.modifierStates.TryResize(newAction.settings.modifiers.Size());
            !sized) {
            return sized;
        }

        newAction.value = InputValue(newAction.settings.valueType, Vec3d());
        if (auto reserved = actions_.TryEnsureCapacity(actions_.Size() + 1); !reserved) {
            return reserved;
        }

        // 새 액션만큼 이벤트 목록도 잡는다(평가 무할당). 전달 중에는 Events()가 돌고 있어 잡지
        // 않는다.
        if (!dispatchPending_) {
            if (auto reserved = events_.TryEnsureCapacity(actions_.Size() + 1); !reserved) {
                return reserved;
            }
        }
    }

    // 매개변수는 마지막으로 넘긴 값이다(같은 종류 열이면 상태는 그대로 — 4.3절 3).
    InjectionEntry entry(allocator);
    entry.action = action.id;
    if (auto copied =
            InjectCopyElements(entry.triggers, entry.modifiers, triggers, modifiers, allocator);
        !copied) {
        return copied;
    }

    ResolveChords(entry.triggers.View(), actions_, mappedActions_);

    if (found == injections_.Size()) {
        if (auto sized = entry.triggerStates.TryResize(entry.triggers.Size()); !sized) {
            return sized;
        }

        if (auto sized = entry.modifierStates.TryResize(entry.modifiers.Size()); !sized) {
            return sized;
        }

        if (!continuous) {
            if (auto pushed = entry.pending.TryPushBack(value); !pushed) {
                return pushed;
            }
        }

        if (auto reserved = injections_.TryEnsureCapacity(injections_.Size() + 1); !reserved) {
            return reserved;
        }
    } else if (!continuous) {
        if (auto reserved =
                injections_[found].pending.TryEnsureCapacity(injections_[found].pending.Size() + 1);
            !reserved) {
            return reserved;
        }
    }

    // ---- 할당은 끝났다: 반영(할당 없음) ----
    if (!hasAction) {
        (void)actions_.TryPushBack(Move(newAction));
    }

    if (continuous) {
        // 같은 액션의 이전 연속 주입을 대신한다.
        for (InjectionEntry& other : injections_) {
            if (other.action == action.id) {
                other.continuous = false;
            }
        }
    }

    InjectionEntry* target = nullptr;
    if (found == injections_.Size()) {
        (void)injections_.TryPushBack(Move(entry));
        target = &injections_[injections_.Size() - 1];
    } else {
        target = &injections_[found];
        target->triggers = Move(entry.triggers);
        target->modifiers = Move(entry.modifiers);
        if (!continuous) {
            (void)target->pending.TryPushBack(value);
        }
    }

    if (continuous) {
        target->continuous = true;
        target->continuousValue = value;
    }

    return Result<void>::Success();
}

Result<void> PlayerActionInput::InjectValue(const InputActionData& action, const InputValue& value,
                                            Span<const InputTrigger> triggers,
                                            Span<const InputModifier> modifiers) noexcept {
    return Inject(action, value, triggers, modifiers, false);
}

Result<void>
PlayerActionInput::StartContinuousInjection(const InputActionData& action, const InputValue& value,
                                            Span<const InputTrigger> triggers,
                                            Span<const InputModifier> modifiers) noexcept {
    return Inject(action, value, triggers, modifiers, true);
}

Result<void> PlayerActionInput::UpdateContinuousInjection(const Guid& actionId,
                                                          const InputValue& value) noexcept {
    for (InjectionEntry& entry : injections_) {
        if (entry.action == actionId && entry.continuous) {
            entry.continuousValue = value;
            return Result<void>::Success();
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> PlayerActionInput::StopContinuousInjection(const Guid& actionId) noexcept {
    for (InjectionEntry& entry : injections_) {
        if (entry.action == actionId && entry.continuous) {
            entry.continuous = false;
            return Result<void>::Success();
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

} // namespace rocks::input
