#include <InputActions/InputContextCollect.h>
#include <InputActions/PlayerActionInput.h>

// 평가기의 컨텍스트 목록이다(Docs/Design/InputActions 4.1절 — UE
// EnhancedInputSubsystemInterface.cpp:251-433).
namespace rocks::input {

PlayerActionInput::PlayerActionInput(Allocator& allocator) noexcept
    : allocator_(&allocator), contexts_(allocator), actions_(allocator), bindings_(allocator),
      chordStates_(allocator), injections_(allocator), events_(allocator),
      chordDependencies_(allocator), profile_(allocator) {
}

usize PlayerActionInput::FindContext(const Guid& contextId) const noexcept {
    for (usize index = 0; index < contexts_.Size(); ++index) {
        if (contexts_[index].bundle.id == contextId) {
            return index;
        }
    }

    return contexts_.Size();
}

bool PlayerActionInput::HasContext(const Guid& contextId) const noexcept {
    return FindContext(contextId) < contexts_.Size();
}

Result<void> PlayerActionInput::RequestRebuild(const ContextOptions& options) noexcept {
    rebuildPending_ = true;
    // 예약된 요청들의 무시 옵션은 AND로 합친다(UE cpp:433).
    pendingIgnorePressed_ = pendingIgnorePressed_ && options.ignorePressedKeysUntilRelease;
    // 즉시 다시 만들기가 실패해도 등록·제거는 반영되었으므로 성공이다. 예약이 남아 다음
    // ApplyPendingChanges가 다시 시도하고 그 결과로 실패를 알린다(4.1절).
    if (options.forceImmediately) {
        (void)Rebuild(nullptr, 0);
    }

    return Result<void>::Success();
}

Result<void> PlayerActionInput::AddContext(InputContextBundle&& bundle, int32 priority,
                                           const ContextOptions& options) noexcept {
    if (auto validated = ValidateInputContextBundle(bundle); !validated) {
        return validated;
    }

    const usize existing = FindContext(bundle.id);
    if (existing < contexts_.Size()) {
        ContextEntry& entry = contexts_[existing];
        // 같은 id: 데이터는 그대로다(UE cpp:273-285).
        if (entry.bundle.data.registrationTracking ==
            ContextRegistrationTracking::CountRegistrations) {
            ++entry.registrations;
            return Result<void>::Success();
        }

        entry.priority = priority;
        return RequestRebuild(options);
    }

    if (auto reserved = contexts_.TryEnsureCapacity(contexts_.Size() + 1); !reserved) {
        return reserved;
    }

    ContextEntry entry(*allocator_);
    entry.bundle = Move(bundle);
    entry.priority = priority;
    entry.order = nextOrder_++;
    (void)contexts_.TryPushBack(Move(entry));
    return RequestRebuild(options);
}

Result<void> PlayerActionInput::UpdateContext(InputContextBundle&& bundle,
                                              bool forceImmediately) noexcept {
    const usize existing = FindContext(bundle.id);
    if (existing >= contexts_.Size()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (auto validated = ValidateInputContextBundle(bundle); !validated) {
        return validated;
    }

    contexts_[existing].bundle = Move(bundle);
    ContextOptions options;
    options.forceImmediately = forceImmediately;
    return RequestRebuild(options);
}

Result<void> PlayerActionInput::RemoveContext(const Guid& contextId,
                                              const ContextOptions& options) noexcept {
    const usize existing = FindContext(contextId);
    if (existing >= contexts_.Size()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    ContextEntry& entry = contexts_[existing];
    if (entry.bundle.data.registrationTracking == ContextRegistrationTracking::CountRegistrations &&
        entry.registrations > 1) {
        --entry.registrations;
        return Result<void>::Success();
    }

    // 안정 제거다(남은 순서 값은 그대로 — 정렬은 순서 값으로 한다).
    (void)contexts_.TryRemoveAt(existing);
    return RequestRebuild(options);
}

void PlayerActionInput::ClearContexts() noexcept {
    contexts_.Clear();
    (void)RequestRebuild(ContextOptions{});
}

Result<void> PlayerActionInput::SetKeyProfile(StringView profileId) noexcept {
    if (profile_.View() == profileId) {
        return Result<void>::Success();
    }

    if (auto assigned = profile_.TryAssign(profileId); !assigned) {
        return assigned;
    }

    return RequestRebuild(ContextOptions{});
}

Result<void> PlayerActionInput::ApplyPendingChanges(const InputState& state,
                                                    uint32 gamepad) noexcept {
    if (!rebuildPending_) {
        return Result<void>::Success();
    }

    return Rebuild(&state, gamepad);
}

InputBindingView PlayerActionInput::Binding(usize index) const noexcept {
    if (index >= bindings_.Size()) {
        return InputBindingView{};
    }

    const BindingEntry& binding = bindings_[index];
    InputBindingView view;
    view.action = actions_[binding.action].id;
    view.context = binding.context;
    view.key = binding.key;
    view.triggers = binding.triggers.View();
    view.modifiers = binding.modifiers.View();
    view.ignored = binding.ignored;
    return view;
}

Guid PlayerActionInput::ActionId(usize index) const noexcept {
    return index < actions_.Size() ? actions_[index].id : Guid{};
}

} // namespace rocks::input
