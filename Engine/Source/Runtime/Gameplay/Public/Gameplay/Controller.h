#pragma once

#include <Core/Math/Rotator.h>
#include <Gameplay/GameplayTypes.h>
#include <Meta/Annotations.h>
#include <World/Actor.h>

namespace rocks {

// Controller 상태다(UE NAME_Inactive·Playing·Spectating, Docs/Design/EngineRuntime 20.3절).
enum class ControllerState : uint8 {
    // Pawn을 잃었다(파괴 등). PlayerState는 남는다.
    Inactive,
    Playing,
    // 아직 Pawn이 없다(PlayerController 시작 상태).
    Spectating
};

ROCKS_GAMEPLAY_API const char* ControllerStateName(ControllerState state) noexcept;

// Pawn을 조종하는 Actor다(UE AController, 20.3절). 배치하지 않는다. Possess로 Pawn을 얻고
// UnPossess·Pawn 파괴로 잃는다. wantsPlayerState면 OnFrameworkPostAwake에서 PlayerState를 만들고
// 파괴될 때 함께 없앤다. GameDirector가 있으면 파괴될 때 Logout을 부른다(EndPlay 전).
//
// 사용 예:
//   auto possessed = world.With<Controller>(controller, [pawn](Controller& value) noexcept {
//       return value.Possess(pawn);
//   });
//
// 스레드: World의 게임 스레드 전용이다. 객체 포인터는 World::With 범위에서만 쓴다.
ROCKS_CLASS(WorldOnly)
class ROCKS_GAMEPLAY_API Controller : public Actor {
    ROCKS_GENERATED_BODY()

public:
    explicit Controller(Allocator& allocator) noexcept;
    ~Controller() noexcept override;

    ObjectHandle PawnHandle() const noexcept {
        return pawn_;
    }

    ObjectHandle PlayerStateHandle() const noexcept {
        return playerState_;
    }

    ControllerState State() const noexcept {
        return state_;
    }

    // 바라보는 회전이다(도, UE ControlRotation). Pawn이 GetControlRotation으로 읽는다.
    const Rotator<double>& ControlRotation() const noexcept {
        return controlRotation_;
    }

    void SetControlRotation(const Rotator<double>& rotation) noexcept {
        controlRotation_ = rotation;
    }

    // pawn을 조종한다(UE Possess): OnPossess → Pawn이 바뀌었으면 OnPossessedPawnChanged. 오류:
    // 권한이 없으면(Client World) InvalidArgument, pawn이 이 World의 살아 있는 Pawn이 아니면
    // InvalidArgument(없는 handle은 StaleHandle).
    Result<void> Possess(ObjectHandle pawn) noexcept;

    // 조종을 그만둔다(UE UnPossess): Pawn이 없으면 아무것도 하지 않는다. Pawn이 바뀌었으면
    // OnPossessedPawnChanged.
    void UnPossess() noexcept;

    // Pawn이 파괴될 때 Pawn이 부른다(UE PawnPendingDestroy): UnPossess → Inactive → PlayerState가
    // 없으면 자신도 파괴한다.
    void PawnPendingDestroy(ObjectHandle pawn) noexcept;

protected:
    // Possess 본문이다(UE OnPossess): 기존 Pawn UnPossess → 대상의 기존 Controller UnPossess →
    // pawn.PossessedBy → SetPawn(Tick 간선) → controlRotation = Pawn 회전 → pawn.Restart. 재정의는
    // Controller::OnPossess를 불러야 한다.
    virtual Result<void> OnPossess(ObjectHandle pawn) noexcept;

    // UnPossess 본문이다(UE OnUnPossess): pawn.UnPossessed → SetPawn(null). 재정의는
    // Controller::OnUnPossess를 불러야 한다.
    virtual void OnUnPossess() noexcept;

    // 사용자 hook: 조종하는 Pawn이 바뀌었다.
    virtual void OnPossessedPawnChanged(ObjectHandle, ObjectHandle) noexcept {
    }

    // PlayerState 생성(wantsPlayerState) → GameplayWorldSubsystem 등록.
    // PlayerController·AiController가 이어 쓰고 닫는다.
    Result<void> OnFrameworkPostAwake() noexcept override;
    // Logout(Director가 있으면) → PlayerState 파괴 → UnPossess(Pawn은 남는다) → 등록 해제.
    void OnFrameworkEndPlay(EndPlayReason reason) noexcept override;

    void SetState(ControllerState state) noexcept {
        state_ = state;
    }

    // PlayerState를 만들지다(PlayerController는 참, AiController 기본은 거짓).
    ROCKS_NATIVE() bool wantsPlayerState = false;

private:
    // Pawn을 바꾸고 Pawn Tick → Controller Tick 간선을 옮긴다(UE SetPawn·AddPawnTickDependency).
    void SetPawn(ObjectHandle pawn) noexcept;

    ROCKS_NATIVE() ObjectHandle pawn_;
    ROCKS_NATIVE() ObjectHandle playerState_;
    ROCKS_NATIVE() Rotator<double> controlRotation_;
    ROCKS_NATIVE() ControllerState state_ = ControllerState::Spectating;
};

// AI용 Controller다(UE AAIController 최소 범위, 20.7절). 기본은 PlayerState를 만들지 않는다(파생
// 생성자에서 wantsPlayerState를 켤 수 있다). brain·경로·감지는 CR39·CM16이 더한다.
ROCKS_CLASS(WorldOnly)
class ROCKS_GAMEPLAY_API AiController : public Controller {
    ROCKS_GENERATED_BODY()

public:
    explicit AiController(Allocator& allocator) noexcept;
    ~AiController() noexcept override;

protected:
    Result<void> OnFrameworkPostAwake() noexcept final;
    void OnFrameworkEndPlay(EndPlayReason reason) noexcept final;
};

} // namespace rocks
