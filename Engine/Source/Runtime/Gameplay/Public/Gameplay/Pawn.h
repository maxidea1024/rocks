#pragma once

#include <Core/Math/Rotator.h>
#include <Gameplay/GameplayTypes.h>
#include <Meta/Annotations.h>
#include <World/Actor.h>

namespace rocks {

// AI Controller를 자동으로 붙일 때다(UE EAutoPossessAI, Docs/Design/EngineRuntime 20.4절).
enum class AutoPossessAi : uint8 {
    Disabled,
    // 맵 배치(OpenWorld populate 중 Spawn)일 때만.
    PlacedInWorld,
    // 게임 중 Spawn일 때만.
    Spawned,
    PlacedInWorldOrSpawned
};

// Controller가 조종하는 Actor다(UE APawn, 20.4절). Controller가 Possess하면 PossessedBy로 연결되고
// UnPossess·파괴로 풀린다. 파괴되면 Controller에 알려(PawnPendingDestroy) Controller는 Inactive가
// 되고 PlayerState는 남는다. autoPossessAi가 맞으면 OnFrameworkPostAwake에서 AiController를 만들어
// 스스로 Possess된다(UE SpawnDefaultController — Game·Tryout World의 권한 있는 쪽만).
//
// 사용 예:
//   class Runner final : public Pawn {
//       ROCKS_NATIVE_TYPE(Runner, Pawn, 0x..., 0x...)
//   public:
//       explicit Runner(Allocator& allocator) noexcept : Pawn(allocator) {
//           autoPossessAi = AutoPossessAi::Disabled;
//       }
//       void OnRestart() noexcept override { /* 다시 태어난 상태로 */ }
//   };
//
// 스레드: World의 게임 스레드 전용이다. 객체 포인터는 World::With 범위에서만 쓴다.
ROCKS_CLASS(WorldOnly)
class ROCKS_GAMEPLAY_API Pawn : public Actor {
    ROCKS_GENERATED_BODY()

public:
    explicit Pawn(Allocator& allocator) noexcept;
    ~Pawn() noexcept override;

    // 조종하는 Controller다(없으면 null).
    ObjectHandle ControllerHandle() const noexcept {
        return controller_;
    }

    // 조종하는 Controller의 PlayerState다(없으면 null).
    ObjectHandle PlayerStateHandle() const noexcept {
        return playerState_;
    }

    // 맵 배치(OpenWorld populate 중 Spawn)였는지다. 전환 정리는 배치한 Pawn을 지우지
    // 않는다(21.3절).
    bool IsPlacedInWorld() const noexcept {
        return placedInWorld_;
    }

    // 바라보는 회전이다: Controller가 있으면 그 controlRotation(UE도 위임), 없으면 root 회전(root도
    // 없으면 0).
    Rotator<double> GetControlRotation() noexcept;

    // UE APawn::GetViewRotation: control rotation이다(관전 분기는 없다 — 26.2절).
    bool TryGetViewRotation(Rotator<double>& out) noexcept override {
        out = GetControlRotation();
        return true;
    }

    // 연결을 건다(Controller::OnPossess가 부른다 — UE PossessedBy): Owner = controller →
    // controller· PlayerState 연결 → OnPossessed → 바뀌었으면 OnControllerChanged.
    void PossessedBy(ObjectHandle controller, ObjectHandle playerState) noexcept;

    // 연결을 푼다(Controller::OnUnPossess가 부른다 — UE UnPossessed): PlayerState·Owner·controller
    // 해제 → OnUnpossessed → OnControllerChanged.
    void UnPossessed() noexcept;

    // Possess 마지막 단계다(UE DispatchRestart): OnRestart를 부른다.
    void Restart() noexcept;

protected:
    // --- 사용자 hook ---
    virtual void OnPossessed(ObjectHandle) noexcept {
    }

    virtual void OnUnpossessed() noexcept {
    }

    virtual void OnControllerChanged(ObjectHandle, ObjectHandle) noexcept {
    }

    virtual void OnRestart() noexcept {
    }

    // 배치 여부를 정하고 autoPossessAi가 맞으면 AiController를 만들어 Possess된다.
    Result<void> OnFrameworkPostAwake() noexcept final;
    // Controller에 알린다(UE DetachFromControllerPendingDestroy).
    void OnFrameworkEndPlay(EndPlayReason reason) noexcept final;

    ROCKS_NATIVE() AutoPossessAi autoPossessAi = AutoPossessAi::PlacedInWorld;
    // 자동으로 만들 AiController 타입이다(비면 GameDirector의 aiControllerType → AiController).
    ROCKS_NATIVE() TypeId aiControllerType;

private:
    friend class Controller;

    ROCKS_NATIVE() ObjectHandle controller_;
    ROCKS_NATIVE() ObjectHandle playerState_;
    ROCKS_NATIVE() bool placedInWorld_ = false;
};

} // namespace rocks
