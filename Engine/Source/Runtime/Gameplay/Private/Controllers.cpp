#include <Core/Diagnostics/Log.h>
#include <Gameplay/Controller.generated.h>
#include <Gameplay/GameDirector.generated.h>
#include <Gameplay/GameDirector.h>
#include <Gameplay/GameState.generated.h>
#include <Gameplay/GameState.h>
#include <Gameplay/GameplayWorldSubsystem.h>
#include <Gameplay/Pawn.generated.h>
#include <Gameplay/Pawn.h>
#include <Gameplay/PlayerController.generated.h>
#include <Gameplay/PlayerController.h>
#include <World/Actor.generated.h>
#include <World/Camera/CameraComponent.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/SceneComponent.h>

// Controller·AiController·PlayerController·CameraDirector·Pawn이다(Docs/Design/EngineRuntime 20절,
// S12).
namespace rocks {

namespace {

// 이 World의 살아 있는(파괴 대기가 아닌) Actor인지다.
bool IsAlive(World& world, ObjectHandle actor) noexcept {
    auto state = world.GetActorState(actor);
    return state && state.Value() != ActorState::PendingDestroy;
}

// actor의 root SceneComponent 월드 Transform이다(root가 없으면 NotFound).
Result<Transform> RootWorldTransform(World& world, ObjectHandle actor) noexcept {
    auto root =
        world.With<Actor>(actor, [](Actor& value) noexcept { return value.RootComponent(); });
    if (!root) {
        return Result<Transform>::Failure(root.Error());
    }

    if (root.Value().IsNull()) {
        return Result<Transform>::Failure(ErrorCode::NotFound);
    }

    return world.With<SceneComponent>(
        root.Value(), [](SceneComponent& scene) noexcept { return scene.TryWorldTransform(); });
}

// root 회전(없으면 0)이다.
Rotator<double> RootRotation(World& world, ObjectHandle actor) noexcept {
    auto transform = RootWorldTransform(world, actor);
    if (!transform) {
        return Rotator<double>();
    }

    auto rotator = Rotator<double>::TryFromQuaternion(transform.Value().rotation);
    return rotator ? rotator.Value() : Rotator<double>();
}

} // namespace

const char* ControllerStateName(ControllerState state) noexcept {
    switch (state) {
    case ControllerState::Inactive:
        return "Inactive";
    case ControllerState::Playing:
        return "Playing";
    case ControllerState::Spectating:
        return "Spectating";
    }

    return "Unknown";
}

// --- Controller ---

Controller::Controller(Allocator& allocator) noexcept : Actor(allocator) {
}

Controller::~Controller() noexcept = default;

Result<void> Controller::Possess(ObjectHandle pawn) noexcept {
    World& world = GetWorld();
    // 권한 확인(UE Possess — 서버·standalone만).
    auto* gameplay = world.GetSubsystem<GameplayWorldSubsystem>();
    if (gameplay != nullptr && !gameplay->HasAuthority()) {
        Log::Warning(LogGameplay, "Possess denied in world {World}: no authority ({Code})",
                     world.Name(), DiagPossessDeniedNoAuthority);
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto state = world.GetActorState(pawn);
    if (!state) {
        return Result<void>::Failure(state.Error());
    }

    if (state.Value() == ActorState::PendingDestroy ||
        !world.With<Pawn>(pawn, [](Pawn&) noexcept {})) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const ObjectHandle previous = pawn_;
    if (auto possessed = OnPossess(pawn); !possessed) {
        return possessed;
    }

    if (pawn_ != previous) {
        OnPossessedPawnChanged(previous, pawn_);
    }

    return Result<void>::Success();
}

Result<void> Controller::OnPossess(ObjectHandle pawn) noexcept {
    // UE OnPossess(Controller.cpp:495–540): 기존 Pawn → 대상의 기존 Controller → PossessedBy →
    // SetPawn → ControlRotation → Restart.
    World& world = GetWorld();
    if (!pawn_.IsNull() && pawn_ != pawn) {
        UnPossess();
    }

    auto holder = world.With<Pawn>(pawn, [](Pawn& value) noexcept { return value.controller_; });
    if (holder && !holder.Value().IsNull()) {
        if (holder.Value() == Handle()) {
            UnPossess();
        } else {
            auto released = world.With<Controller>(
                holder.Value(), [](Controller& other) noexcept { other.UnPossess(); });
            (void)released;
        }
    }

    const ObjectHandle self = Handle();
    const ObjectHandle playerState = playerState_;
    auto linked = world.With<Pawn>(
        pawn, [self, playerState](Pawn& value) noexcept { value.PossessedBy(self, playerState); });
    if (!linked) {
        return linked;
    }

    SetPawn(pawn);
    if (!playerState_.IsNull()) {
        auto noted = world.With<PlayerState>(
            playerState_, [pawn](PlayerState& state) noexcept { state.pawn_ = pawn; });
        (void)noted;
    }

    controlRotation_ = RootRotation(world, pawn);
    auto restarted = world.With<Pawn>(pawn, [](Pawn& value) noexcept { value.Restart(); });
    (void)restarted;
    return Result<void>::Success();
}

void Controller::UnPossess() noexcept {
    if (pawn_.IsNull()) {
        return;
    }

    const ObjectHandle previous = pawn_;
    OnUnPossess();
    if (pawn_ != previous) {
        OnPossessedPawnChanged(previous, pawn_);
    }
}

void Controller::OnUnPossess() noexcept {
    // UE OnUnPossess(Controller.cpp:386–419): Pawn UnPossessed → SetPawn(null).
    World& world = GetWorld();
    auto released = world.With<Pawn>(pawn_, [](Pawn& value) noexcept { value.UnPossessed(); });
    (void)released;
    if (!playerState_.IsNull()) {
        auto noted = world.With<PlayerState>(playerState_,
                                             [](PlayerState& state) noexcept { state.pawn_ = {}; });
        (void)noted;
    }

    SetPawn({});
}

void Controller::SetPawn(ObjectHandle pawn) noexcept {
    if (pawn_ == pawn) {
        return;
    }

    // Pawn OnTick이 Controller OnTick 뒤다(UE AddPawnTickDependency). 한쪽이 OnTick을 켜지 않았으면
    // World가 InvalidArgument로 거절한다 — 걸 간선이 없으므로 무시한다(20.8절).
    World& world = GetWorld();
    if (!pawn_.IsNull()) {
        auto removed = world.RemoveTickPrerequisite(TickPoint{pawn_}, TickPoint{Handle()},
                                                    TickEdgeSource::Framework);
        (void)removed;
    }

    pawn_ = pawn;
    if (!pawn.IsNull()) {
        auto added = world.AddTickPrerequisite(TickPoint{pawn}, TickPoint{Handle()},
                                               TickEdgeSource::Framework);
        (void)added;
    }
}

void Controller::PawnPendingDestroy(ObjectHandle pawn) noexcept {
    // UE PawnPendingDestroy(Controller.cpp:421–440).
    if (pawn != pawn_) {
        return;
    }

    UnPossess();
    state_ = ControllerState::Inactive;
    if (playerState_.IsNull()) {
        auto destroyed = GetWorld().DestroyActor(Handle());
        (void)destroyed;
    }
}

Result<void> Controller::OnFrameworkPostAwake() noexcept {
    World& world = GetWorld();
    auto* gameplay = world.GetSubsystem<GameplayWorldSubsystem>();
    if (wantsPlayerState) {
        // PlayerState 생성(UE InitPlayerState — Owner = 이 Controller). 타입은 Director의
        // playerStateType(비면 PlayerState).
        TypeId type = ObjectTypeIdOf<PlayerState>();
        if (gameplay != nullptr && !gameplay->Director().IsNull()) {
            auto chosen =
                world.With<GameDirector>(gameplay->Director(), [](GameDirector& director) noexcept {
                    return director.PlayerStateType();
                });
            if (chosen && chosen.Value() != TypeId{}) {
                type = chosen.Value();
            }
        }

        SpawnParams params;
        params.owner = Handle();
        auto spawned = world.SpawnActorDeferred(type, params);
        if (!spawned || !world.With<PlayerState>(spawned.Value(), [](PlayerState&) noexcept {})) {
            if (spawned) {
                auto destroyed = world.DestroyActor(spawned.Value());
                (void)destroyed;
            }

            Log::Error(LogGameplay, "Controller could not create its PlayerState");
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto finished = world.FinishSpawning(spawned.Value()); !finished) {
            return finished;
        }

        playerState_ = spawned.Value();
    }

    if (gameplay != nullptr) {
        return gameplay->controllers_.TryPushBack(Handle());
    }

    return Result<void>::Success();
}

void Controller::OnFrameworkEndPlay(EndPlayReason) noexcept {
    // Logout → PlayerState 파괴 → UnPossess(Pawn은 남는다) → 등록 해제(20.3절).
    World& world = GetWorld();
    auto* gameplay = world.GetSubsystem<GameplayWorldSubsystem>();
    const ObjectHandle self = Handle();
    // UE AController::Destroyed는 PlayerState가 있을 때만 Logout한다(Controller.cpp:597).
    if (gameplay != nullptr && !gameplay->Director().IsNull() && !playerState_.IsNull()) {
        auto loggedOut =
            world.With<GameDirector>(gameplay->Director(), [self](GameDirector& director) noexcept {
                director.Logout(self);
            });
        (void)loggedOut;
    }

    if (!playerState_.IsNull()) {
        auto destroyed = world.DestroyActor(playerState_);
        (void)destroyed;
        playerState_ = {};
    }

    UnPossess();
    if (gameplay != nullptr) {
        for (usize index = 0; index < gameplay->controllers_.Size(); ++index) {
            if (gameplay->controllers_[index] == self) {
                auto removed = gameplay->controllers_.TryRemoveAt(index);
                (void)removed;
                break;
            }
        }
    }
}

// --- AiController ---

AiController::AiController(Allocator& allocator) noexcept : Controller(allocator) {
}

AiController::~AiController() noexcept = default;

Result<void> AiController::OnFrameworkPostAwake() noexcept {
    return Controller::OnFrameworkPostAwake();
}

void AiController::OnFrameworkEndPlay(EndPlayReason reason) noexcept {
    Controller::OnFrameworkEndPlay(reason);
}

// --- PlayerController ---

PlayerController::PlayerController(Allocator& allocator) noexcept
    : Controller(allocator), actionInput_(allocator), actionBindings_(allocator),
      haptics_(allocator), inputAssets_(allocator) {
    wantsPlayerState = true;
    // pause 중에도 Tick한다(UE PlayerController.cpp:204 — TickPlayerInput·Tick).
    PrimaryTick().tick = true;
    PrimaryTick().tickWhenPaused = true;
}

PlayerController::~PlayerController() noexcept = default;

void PlayerController::SetPlayer(ObjectHandle localPlayer, const input::InputState* source,
                                 uint32 gamepad, input::InputUserSettings* userSettings) noexcept {
    localPlayer_ = localPlayer;
    inputSource_ = source;
    gamepad_ = gamepad;
    actionInput_.SetUserSettings(userSettings);
    playerInput_ = input::InputState();
    gestures_.Reset();
}

void PlayerController::OnTick(const TickContext& context) noexcept {
    // ① TickPlayerInput → ② OnPlayerTick(pause 중 생략) → ③ OnControllerTick(UE
    // PlayerController.cpp:5504–5519, 5636–5685).
    playerInput_ = inputSource_ != nullptr ? *inputSource_ : input::InputState();
    // 제스처 인식은 입력 스냅숏 다음이다(UE UPlayerInput이 FGestureRecognizer를 부르는 자리).
    if (gestureRecognizerEnabled_) {
        gestures_.Update(playerInput_, static_cast<float>(context.unscaledDeltaSeconds));
    }

    // 입력 액션: 평가 → 전달 → 전달 뒤 정리(Docs/Design/InputActions 5.1절). pause 중에도 한다(막는
    // 것은 액션의 triggerWhenPaused다).
    input::ActionEvalContext actionContext;
    actionContext.deltaSeconds = context.unscaledDeltaSeconds;
    actionContext.timeDilation = GetWorld().Clock().TimeScale();
    actionContext.paused = context.paused;
    actionContext.hasPlayer = true;
    actionContext.gamepad = gamepad_;
    if (!cameraDirector_.IsNull()) {
        auto viewed = GetWorld().With<CameraDirector>(
            cameraDirector_, [&actionContext](CameraDirector& camera) noexcept {
                actionContext.fov = static_cast<float>(camera.View().fov);
            });
        (void)viewed;
    }

    // 평가기에서 직접 뺀 에셋 컨텍스트의 lease를 놓는다(5.4절).
    ReleaseRemovedInputAssets();
    auto evaluated = actionInput_.Evaluate(playerInput_, actionContext);
    if (!evaluated && !inputEvaluationFailing_) {
        Log::Warning(LogGameplay,
                     "Input action evaluation failed: {Error}; it evaluated with the previous "
                     "mappings or dropped event records ({Code})",
                     ErrorCodeName(evaluated.Error()), DiagInputEvaluationFailed);
    }

    inputEvaluationFailing_ = !evaluated;

    actionBindings_.Dispatch(actionInput_);
    actionInput_.FinishDispatch();

    // 진동 합성(Docs/Design/InputActions 6.3절 — UE ProcessForceFeedbackAndHaptics). 전달 뒤라 이번
    // 틱 콜백이 시작한 효과도 기여한다. pause 중 deltaSeconds는 unscaled delta다(TickWhenPaused).
    if (localPlayer_.IsNull()) {
        motorOutput_ = {};
    } else {
        input::HapticUpdateContext hapticContext;
        hapticContext.deltaSeconds = context.deltaSeconds;
        hapticContext.unscaledDeltaSeconds = context.unscaledDeltaSeconds;
        hapticContext.paused = context.paused;
        hapticContext.scale = hapticScale_;
        hapticContext.enabled = hapticsEnabled_;
        motorOutput_ = haptics_.Update(hapticContext);
    }

    if (!context.paused) {
        OnPlayerTick(context);
    }

    OnControllerTick(context);
}

Result<void> PlayerController::OnFrameworkPostAwake() noexcept {
    // Controller 부분(PlayerState·등록) → CameraDirector(Owner = 이 PlayerController) → Spectating
    // (UE PlayerController.cpp:1069–1090).
    if (auto base = Controller::OnFrameworkPostAwake(); !base) {
        return base;
    }

    World& world = GetWorld();
    SpawnParams params;
    params.owner = Handle();
    auto camera = world.SpawnActor<CameraDirector>(params);
    if (!camera) {
        return Result<void>::Failure(camera.Error());
    }

    const ObjectHandle self = Handle();
    auto targeted = world.With<CameraDirector>(
        camera.Value(), [self](CameraDirector& value) noexcept { value.SetViewTarget(self); });
    (void)targeted;
    cameraDirector_ = camera.Value();
    SetState(ControllerState::Spectating);
    return Result<void>::Success();
}

Result<void> PlayerController::OnPossess(ObjectHandle pawn) noexcept {
    if (auto base = Controller::OnPossess(pawn); !base) {
        return base;
    }

    SetState(ControllerState::Playing);
    auto targeted = GetWorld().With<CameraDirector>(
        cameraDirector_, [pawn](CameraDirector& camera) noexcept { camera.SetViewTarget(pawn); });
    (void)targeted;
    return Result<void>::Success();
}

void PlayerController::OnUnPossess() noexcept {
    Controller::OnUnPossess();
    const ObjectHandle self = Handle();
    auto targeted = GetWorld().With<CameraDirector>(
        cameraDirector_, [self](CameraDirector& camera) noexcept { camera.SetViewTarget(self); });
    (void)targeted;
}

void PlayerController::OnFrameworkEndPlay(EndPlayReason reason) noexcept {
    // CameraDirector 파괴 → (접속 끊김이면 Pawn 파괴 — UE PawnLeavingGame) → Controller 부분.
    World& world = GetWorld();
    if (!cameraDirector_.IsNull()) {
        auto destroyed = world.DestroyActor(cameraDirector_);
        (void)destroyed;
        cameraDirector_ = {};
    }

    if (localPlayer_.IsNull() && !PawnHandle().IsNull()) {
        auto destroyed = world.DestroyActor(PawnHandle());
        (void)destroyed;
    }

    Controller::OnFrameworkEndPlay(reason);
    // 에셋 컨텍스트 lease는 World 파괴 안에서 놓는다(AssetSystem 11.8절 6 — 5.4절).
    ReleaseInputAssets();
    ClearPlayer();
}

// --- CameraDirector ---

CameraDirector::CameraDirector(Allocator& allocator) noexcept : Actor(allocator) {
}

CameraDirector::~CameraDirector() noexcept = default;

void CameraDirector::UpdateCamera(double deltaSeconds) noexcept {
    World& world = GetWorld();
    // 대상이 없거나 사라졌으면 소유 PlayerController로 되돌린다.
    if (viewTarget_.IsNull() || !IsAlive(world, viewTarget_)) {
        viewTarget_ = Owner();
    }

    const ObjectHandle target = viewTarget_;
    // 켜진 첫 CameraComponent(UE CalcCamera의 bFindCameraComponentWhenViewTarget — Actor.cpp:3688).
    bool fromCamera = false;
    CameraViewInfo info;
    auto searched = world.With<Actor>(target, [&](Actor& actor) noexcept {
        for (ObjectHandle component : actor.Components()) {
            auto used =
                world.With<CameraComponent>(component, [&](CameraComponent& camera) noexcept {
                    if (!camera.IsEnabled() ||
                        camera.GetState() == ComponentState::PendingDestroy) {
                        return false;
                    }

                    camera.GetCameraView(deltaSeconds, info);
                    return true;
                });
            if (used && used.Value()) {
                fromCamera = true;
                return;
            }
        }
    });
    (void)searched;
    if (fromCamera) {
        view_.location = info.location;
        view_.fov = info.fieldOfView;
        view_.projection = info.projection;
        view_.orthoWidth = info.orthoWidth;
        view_.aspectRatio = info.aspectRatio;
        view_.constrainAspectRatio = info.constrainAspectRatio;
        auto pawn =
            info.usePawnControlRotation
                ? world.With<Pawn>(target,
                                   [](Pawn& value) noexcept { return value.GetControlRotation(); })
                : Result<Rotator<double>>::Failure(ErrorCode::NotFound);
        if (pawn) {
            view_.rotation = pawn.Value();
        } else if (auto rotator = Rotator<double>::TryFromQuaternion(info.rotation)) {
            view_.rotation = rotator.Value();
        }

        OnUpdateView(view_);
        return;
    }

    // 카메라 Component가 없으면 시야·투영은 기본이다.
    view_.fov = CameraView::DefaultFov;
    view_.projection = CameraProjection::Perspective;
    view_.orthoWidth = CameraComponent::DefaultOrthoWidth;
    view_.aspectRatio = CameraComponent::DefaultAspectRatio;
    view_.constrainAspectRatio = false;
    auto transform = RootWorldTransform(world, target);
    if (transform) {
        view_.location = transform.Value().translation;
    }

    // 회전: Controller면 controlRotation, Pawn이면 GetControlRotation(UE 기본 CalcCamera), 그 밖은
    // root 회전.
    auto controller =
        world.With<Controller>(target, [](Controller& value) noexcept -> Rotator<double> {
            return value.ControlRotation();
        });
    if (controller) {
        view_.rotation = controller.Value();
    } else if (auto pawn = world.With<Pawn>(
                   target, [](Pawn& value) noexcept { return value.GetControlRotation(); })) {
        view_.rotation = pawn.Value();
    } else if (transform) {
        auto rotator = Rotator<double>::TryFromQuaternion(transform.Value().rotation);
        if (rotator) {
            view_.rotation = rotator.Value();
        }
    }

    OnUpdateView(view_);
}

// --- Pawn ---

Pawn::Pawn(Allocator& allocator) noexcept : Actor(allocator) {
}

Pawn::~Pawn() noexcept = default;

Rotator<double> Pawn::GetControlRotation() noexcept {
    World& world = GetWorld();
    if (!controller_.IsNull()) {
        auto rotation =
            world.With<Controller>(controller_, [](Controller& value) noexcept -> Rotator<double> {
                return value.ControlRotation();
            });
        if (rotation) {
            return rotation.Value();
        }
    }

    return RootRotation(world, Handle());
}

void Pawn::PossessedBy(ObjectHandle controller, ObjectHandle playerState) noexcept {
    // UE PossessedBy(Pawn.cpp:671–711).
    const ObjectHandle previous = controller_;
    auto owned = SetOwner(controller);
    (void)owned;
    controller_ = controller;
    playerState_ = playerState;
    OnPossessed(controller);
    if (previous != controller) {
        OnControllerChanged(previous, controller);
    }
}

void Pawn::UnPossessed() noexcept {
    // UE UnPossessed(Pawn.cpp:713–751).
    const ObjectHandle previous = controller_;
    playerState_ = {};
    auto owned = SetOwner({});
    (void)owned;
    controller_ = {};
    OnUnpossessed();
    if (!previous.IsNull()) {
        OnControllerChanged(previous, {});
    }
}

void Pawn::Restart() noexcept {
    OnRestart();
}

Result<void> Pawn::OnFrameworkPostAwake() noexcept {
    World& world = GetWorld();
    auto* gameplay = world.GetSubsystem<GameplayWorldSubsystem>();
    placedInWorld_ = gameplay != nullptr && gameplay->IsPopulating();
    // AI 자동 Possess(UE SpawnDefaultController — Pawn.cpp:145–158, 384–406): Game·Tryout World의
    // 권한 있는 쪽만.
    const bool gameWorld = world.Kind() == WorldKind::Game || world.Kind() == WorldKind::Tryout;
    const bool wanted = autoPossessAi == AutoPossessAi::PlacedInWorldOrSpawned ||
                        (autoPossessAi == AutoPossessAi::PlacedInWorld && placedInWorld_) ||
                        (autoPossessAi == AutoPossessAi::Spawned && !placedInWorld_);
    if (gameplay == nullptr || !gameplay->HasAuthority() || !gameWorld || !wanted ||
        !controller_.IsNull()) {
        return Result<void>::Success();
    }

    TypeId type = aiControllerType;
    if (type == TypeId{} && !gameplay->Director().IsNull()) {
        auto chosen =
            world.With<GameDirector>(gameplay->Director(), [](GameDirector& director) noexcept {
                return director.AiControllerType();
            });
        if (chosen) {
            type = chosen.Value();
        }
    }

    if (type == TypeId{}) {
        type = ObjectTypeIdOf<AiController>();
    }

    auto spawned = world.SpawnActorDeferred(type);
    if (!spawned || !world.With<Controller>(spawned.Value(), [](Controller&) noexcept {})) {
        if (spawned) {
            auto destroyed = world.DestroyActor(spawned.Value());
            (void)destroyed;
        }

        Log::Error(LogGameplay, "Pawn could not create its AI controller");
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto finished = world.FinishSpawning(spawned.Value()); !finished) {
        return finished;
    }

    const ObjectHandle self = Handle();
    return world.With<Controller>(spawned.Value(),
                                  [self](Controller& ai) noexcept { return ai.Possess(self); });
}

void Pawn::OnFrameworkEndPlay(EndPlayReason) noexcept {
    // UE DetachFromControllerPendingDestroy(Pawn.cpp:550–562).
    if (controller_.IsNull()) {
        return;
    }

    const ObjectHandle self = Handle();
    auto notified =
        GetWorld().With<Controller>(controller_, [self](Controller& controller) noexcept {
            controller.PawnPendingDestroy(self);
        });
    (void)notified;
}

} // namespace rocks
