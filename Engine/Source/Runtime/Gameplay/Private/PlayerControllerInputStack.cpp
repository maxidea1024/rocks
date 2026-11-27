#include <Core/Diagnostics/Log.h>
#include <Gameplay/Components/InputComponent.generated.h>
#include <Gameplay/Components/InputComponent.h>
#include <Gameplay/GameplayTypes.h>
#include <Gameplay/Pawn.generated.h>
#include <Gameplay/Pawn.h>
#include <Gameplay/PlayerController.generated.h>
#include <Gameplay/PlayerController.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/World.h>

// PlayerController의 입력 스택이다(Docs/Design/InputActions 5.5절 — IA8, UE
// APlayerController::BuildInputStack·PushInputComponent·PopInputComponent,
// UPlayerInput::ProcessInputStack, AActor::EnableInput·DisableInput, APawn::PawnClientRestart).
namespace rocks {

namespace {

// component가 이 World의 살아 있는 InputComponent면 그 priority다. 아니면 그 오류(파괴 대기는
// Destroyed)다.
Result<int32> InputPriorityOf(World& world, ObjectHandle component) noexcept {
    auto state = world.GetComponentState(component);
    if (!state) {
        return Result<int32>::Failure(state.Error());
    }

    if (state.Value() == ComponentState::PendingDestroy) {
        return Result<int32>::Failure(ErrorCode::Destroyed);
    }

    return world.With<InputComponent>(
        component, [](InputComponent& input) noexcept { return input.Priority(); });
}

// component가 InputComponent면 값 바인딩을 0으로 한다(아니면 아무 일도 하지 않는다).
void ClearInputValues(World& world, ObjectHandle component) noexcept {
    auto cleared = world.With<InputComponent>(
        component, [](InputComponent& input) noexcept { input.Bindings().ClearValues(); });
    (void)cleared;
}

} // namespace

Result<void> PlayerController::PushInputComponent(ObjectHandle component) noexcept {
    World& world = GetWorld();
    auto priority = InputPriorityOf(world, component);
    if (!priority) {
        return Result<void>::Failure(priority.Error());
    }

    // 자리를 먼저 잡는다(실패면 그대로).
    if (auto reserved = inputStack_.TryEnsureCapacity(inputStack_.Size() + 1); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < inputStack_.Size(); ++index) {
        if (inputStack_[index] == component) {
            (void)inputStack_.TryRemoveAt(index);
            break;
        }
    }

    // 뒤에서부터 priority가 같거나 작은 첫 항목 바로 뒤다(UE :6041-6058). 쓸 수 없는 항목(파괴
    // 대기·회수 — 다시 살아나지 않는다)은 뺀다(UE도 IsValid가 아니면 뺀다).
    usize insertAt = 0;
    for (usize index = inputStack_.Size(); index > 0; --index) {
        auto existing = InputPriorityOf(world, inputStack_[index - 1]);
        if (!existing) {
            (void)inputStack_.TryRemoveAt(index - 1);
            continue;
        }

        if (existing.Value() <= priority.Value()) {
            insertAt = index;
            break;
        }
    }

    // 칸을 잡았으므로 실패하지 않는다.
    (void)inputStack_.TryInsert(component, insertAt);
    return Result<void>::Success();
}

bool PlayerController::PopInputComponent(ObjectHandle component) noexcept {
    for (usize index = 0; index < inputStack_.Size(); ++index) {
        if (inputStack_[index] == component) {
            (void)inputStack_.TryRemoveAt(index);
            ClearInputValues(GetWorld(), component);
            return true;
        }
    }

    return false;
}

bool PlayerController::IsInputComponentInStack(ObjectHandle component) const noexcept {
    for (const ObjectHandle entry : inputStack_) {
        if (entry == component) {
            return true;
        }
    }

    return false;
}

Result<bool> PlayerController::IsOwnPawn(ObjectHandle actor) noexcept {
    World& world = GetWorld();
    auto state = world.GetActorState(actor);
    if (!state) {
        return Result<bool>::Failure(state.Error());
    }

    if (state.Value() == ActorState::PendingDestroy) {
        return Result<bool>::Failure(ErrorCode::Destroyed);
    }

    auto controller =
        world.With<Pawn>(actor, [](Pawn& pawn) noexcept { return pawn.ControllerHandle(); });
    if (!controller) {
        // Pawn이 아니다.
        return Result<bool>::Success(false);
    }

    if (controller.Value() != Handle()) {
        // UE는 Error 로그 뒤 무시한다(Pawn.cpp:1171-1174).
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<bool>::Success(true);
}

bool PlayerController::CopyActorComponents(ObjectHandle actor) noexcept {
    actorComponents_.Clear();
    bool complete = true;
    auto copied = GetWorld().With<Actor>(actor, [this, &complete](Actor& value) noexcept {
        const Span<const ObjectHandle> components = value.Components();
        if (!actorComponents_.TryEnsureCapacity(components.Size())) {
            complete = false;
            return;
        }

        for (const ObjectHandle component : components) {
            (void)actorComponents_.TryPushBack(component);
        }
    });
    // Actor가 아니면 빈 목록이다(할당 실패만 거짓).
    (void)copied;
    return complete;
}

Result<ObjectHandle> PlayerController::FindLiveInputComponent(ObjectHandle actor) noexcept {
    if (!CopyActorComponents(actor)) {
        return Result<ObjectHandle>::Failure(ErrorCode::OutOfMemory);
    }

    World& world = GetWorld();
    for (const ObjectHandle component : actorComponents_) {
        if (InputPriorityOf(world, component)) {
            return Result<ObjectHandle>::Success(component);
        }
    }

    return Result<ObjectHandle>::Success(ObjectHandle{});
}

Result<ObjectHandle> PlayerController::EnableInput(ObjectHandle actor) noexcept {
    if (actor == Handle()) {
        // 자기 층을 켠다(UE APlayerController::EnableInput — bInputEnabled).
        inputEnabled_ = true;
        return Result<ObjectHandle>::Success(ObjectHandle{});
    }

    auto own = IsOwnPawn(actor);
    if (!own) {
        return Result<ObjectHandle>::Failure(own.Error());
    }

    World& world = GetWorld();
    if (own.Value()) {
        // 조종하는 Pawn은 입력만 켠다(UE APawn::EnableInput).
        return world.With<Pawn>(actor, [](Pawn& pawn) noexcept {
            pawn.inputEnabled_ = true;
            return pawn.PlayerInputComponent();
        });
    }

    auto found = FindLiveInputComponent(actor);
    if (!found) {
        return found;
    }

    ObjectHandle component = found.Value();
    if (component.IsNull()) {
        auto added = world.AddComponent<InputComponent>(actor);
        if (!added) {
            return Result<ObjectHandle>::Failure(added.Error());
        }

        component = added.Value();
    }

    if (auto pushed = PushInputComponent(component); !pushed) {
        return Result<ObjectHandle>::Failure(pushed.Error());
    }

    return Result<ObjectHandle>::Success(component);
}

Result<void> PlayerController::DisableInput(ObjectHandle actor) noexcept {
    if (actor == Handle()) {
        // 자기 층을 끈다(UE APlayerController::DisableInput). 빠진 층은 Pop처럼 값 0이다.
        inputEnabled_ = false;
        actionBindings_.ClearValues();
        return Result<void>::Success();
    }

    auto own = IsOwnPawn(actor);
    if (!own) {
        return Result<void>::Failure(own.Error());
    }

    World& world = GetWorld();
    if (own.Value()) {
        // 조종하는 Pawn은 입력만 끈다(UE APawn::DisableInput). 빠진 층은 Pop처럼 값 0이다.
        auto disabled =
            world.With<Pawn>(actor, [](Pawn& pawn) noexcept { pawn.inputEnabled_ = false; });
        if (!disabled) {
            return disabled;
        }

        if (CopyActorComponents(actor)) {
            for (const ObjectHandle component : actorComponents_) {
                ClearInputValues(world, component);
            }
        }

        return Result<void>::Success();
    }

    // 그 Actor 소유의 InputComponent를 모두 뺀다(뒤에서부터 — index가 밀리지 않게).
    for (usize index = inputStack_.Size(); index > 0; --index) {
        const ObjectHandle component = inputStack_[index - 1];
        auto owner = world.With<Component>(
            component, [](Component& value) noexcept { return value.GetOwner(); });
        if (owner && owner.Value() == actor) {
            (void)PopInputComponent(component);
        }
    }

    return Result<void>::Success();
}

void PlayerController::SetupPawnInput(ObjectHandle pawn) noexcept {
    World& world = GetWorld();
    auto existing =
        world.With<Pawn>(pawn, [](Pawn& value) noexcept { return value.PlayerInputComponent(); });
    if (!existing || !existing.Value().IsNull()) {
        return;
    }

    auto added = world.AddComponent<InputComponent>(pawn);
    if (!added) {
        Log::Error(LogGameplay,
                   "Player input component could not be created: {Error}; the pawn has no player "
                   "input until it is possessed again ({Code})",
                   ErrorCodeName(added.Error()), DiagPlayerInputComponentFailed);
        return;
    }

    const ObjectHandle component = added.Value();
    auto setup = world.With<Pawn>(pawn, [&world, component](Pawn& value) noexcept {
        value.playerInputComponent_ = component;
        auto hooked =
            world.With<InputComponent>(component, [&value](InputComponent& input) noexcept {
                value.OnSetupPlayerInput(input);
            });
        (void)hooked;
    });
    (void)setup;
}

bool PlayerController::AppendPawnInputComponents(ObjectHandle pawn) noexcept {
    World& world = GetWorld();
    auto player =
        world.With<Pawn>(pawn, [](Pawn& value) noexcept { return value.PlayerInputComponent(); });
    const ObjectHandle playerComponent = player ? player.Value() : ObjectHandle{};
    bool complete = CopyActorComponents(pawn);
    auto append = [this, &world, &complete](ObjectHandle component) noexcept {
        if (component.IsNull() || IsInputComponentInStack(component)) {
            return;
        }

        auto priority = InputPriorityOf(world, component);
        if (priority && !inputLayers_.TryPushBack(
                            InputLayer{component, priority.Value(), InputLayerSource::Pawn})) {
            complete = false;
        }
    };

    // 플레이어 입력 컴포넌트가 먼저다(같은 priority에서 가장 아래 — UE "last dibs", :2683-2686).
    append(playerComponent);
    for (const ObjectHandle component : actorComponents_) {
        if (component != playerComponent) {
            append(component);
        }
    }

    return complete;
}

bool PlayerController::BuildInputLayers() noexcept {
    World& world = GetWorld();
    inputLayers_.Clear();
    bool complete = true;
    // 1. 조종하는 Pawn의 InputComponent들(입력이 켜져 있으면 — UE :2670-2696).
    const ObjectHandle pawn = PawnHandle();
    if (!pawn.IsNull()) {
        auto enabled =
            world.With<Pawn>(pawn, [](Pawn& value) noexcept { return value.IsInputEnabled(); });
        auto state = world.GetActorState(pawn);
        if (enabled && enabled.Value() && state && state.Value() != ActorState::PendingDestroy) {
            complete = AppendPawnInputComponents(pawn);
        }
    }

    // 2. PlayerController 층(켜져 있으면 — UE :2711-2714).
    if (inputEnabled_ &&
        !inputLayers_.TryPushBack(
            InputLayer{ObjectHandle{}, ControllerLayerPriority, InputLayerSource::Controller})) {
        complete = false;
    }

    // 3. Push한 것. 쓸 수 없는 항목(파괴 대기·회수)은 뺀다(UE :2716-2726 — IsValid가 아니면 뺀다).
    for (usize index = 0; index < inputStack_.Size();) {
        auto priority = InputPriorityOf(world, inputStack_[index]);
        if (!priority) {
            (void)inputStack_.TryRemoveAt(index);
            continue;
        }

        if (!inputLayers_.TryPushBack(
                InputLayer{inputStack_[index], priority.Value(), InputLayerSource::Stack})) {
            complete = false;
        }

        ++index;
    }

    // 4. priority 오름차순 안정 정렬(삽입 정렬 — UE Algo::StableSort, :2730-2739).
    for (usize index = 1; index < inputLayers_.Size(); ++index) {
        const InputLayer layer = inputLayers_[index];
        usize at = index;
        while (at > 0 && inputLayers_[at - 1].priority > layer.priority) {
            inputLayers_[at] = inputLayers_[at - 1];
            --at;
        }

        inputLayers_[at] = layer;
    }

    return complete;
}

bool PlayerController::IsInputLayerLive(const InputLayer& layer) noexcept {
    if (layer.source == InputLayerSource::Controller) {
        return inputEnabled_;
    }

    World& world = GetWorld();
    if (!InputPriorityOf(world, layer.component)) {
        return false;
    }

    if (layer.source == InputLayerSource::Stack) {
        return IsInputComponentInStack(layer.component);
    }

    // Pawn 층: 아직 조종하는 Pawn의 것이고 그 Pawn의 입력이 켜져 있어야 한다.
    const ObjectHandle pawn = PawnHandle();
    auto owner = world.With<Component>(layer.component,
                                       [](Component& value) noexcept { return value.GetOwner(); });
    if (pawn.IsNull() || !owner || owner.Value() != pawn) {
        return false;
    }

    auto enabled =
        world.With<Pawn>(pawn, [](Pawn& value) noexcept { return value.IsInputEnabled(); });
    return enabled && enabled.Value();
}

void PlayerController::DispatchInputStack() noexcept {
    const bool complete = BuildInputLayers();
    if (!complete && !inputStackFailing_) {
        Log::Error(LogGameplay,
                   "Input stack is missing layers after an allocation failure; it dispatched the "
                   "layers it holds ({Code})",
                   DiagInputStackIncomplete);
    }

    inputStackFailing_ = !complete;
    World& world = GetWorld();
    stackConsumed_.Clear();
    // 위층(끝)부터 전달한다(UE ProcessInputStack — PlayerInput.cpp:1500-1527). 앞 층의 콜백이 바꾼
    // 상태(Pop·파괴·UnPossess·입력 끄기)는 층마다 다시 본다(UE EvaluateInputDelegates :1503-1507).
    usize remaining = inputLayers_.Size();
    while (remaining > 0) {
        const InputLayer layer = inputLayers_[--remaining];
        if (!IsInputLayerLive(layer)) {
            continue;
        }

        bool blocks = false;
        if (layer.source == InputLayerSource::Controller) {
            (void)stackConsumed_.TryEnsureCapacity(stackConsumed_.Size() +
                                                   actionBindings_.BindingCount());
            actionBindings_.Dispatch(actionInput_, stackConsumed_);
        } else {
            auto dispatched =
                world.With<InputComponent>(layer.component, [this](InputComponent& input) noexcept {
                    (void)stackConsumed_.TryEnsureCapacity(stackConsumed_.Size() +
                                                           input.Bindings().BindingCount());
                    input.Bindings().Dispatch(actionInput_, stackConsumed_);
                    return input.BlocksInput();
                });
            blocks = dispatched && dispatched.Value();
        }

        if (!blocks) {
            continue;
        }

        // 막힌 아래층은 전달하지 않고 값 바인딩을 0으로 한다(UE EvaluateBlockedInputComponent).
        while (remaining > 0) {
            const InputLayer lower = inputLayers_[--remaining];
            if (lower.source == InputLayerSource::Controller) {
                actionBindings_.ClearValues();
            } else {
                ClearInputValues(world, lower.component);
            }
        }
    }
}

} // namespace rocks
