#pragma once

#include <Core/Text/String.h>
#include <Gameplay/GameplayTypes.h>
#include <Meta/Annotations.h>
#include <World/Actor.h>

namespace rocks {

// 플레이어 시작 위치 표지다(UE APlayerStart, Docs/Design/EngineRuntime 19.2절). 맵에 배치하고 root
// SceneComponent의 Transform만 가진다(PreAwake에서 만든다 — Spawn Transform이 여기에 붙는다).
// GameDirector::FindPlayerStart가 고른다.
//
// 사용 예(populate):
//   auto start = world.SpawnActor<PlayerStart>(SpawnParams::At(transform));
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS(WorldOnly)
class ROCKS_GAMEPLAY_API PlayerStart : public Actor {
    ROCKS_GENERATED_BODY()

public:
    explicit PlayerStart(Allocator& allocator) noexcept;
    ~PlayerStart() noexcept override;

    // 이름표다(UE PlayerStartTag). FindPlayerStart의 이름 일치에 쓴다.
    StringView PlayerStartTag() const noexcept {
        return tag_.View();
    }

    // 오류: 할당 실패(이름표는 그대로).
    Result<void> SetPlayerStartTag(StringView tag) noexcept {
        return tag_.TryAssign(tag);
    }

protected:
    // root SceneComponent를 만든다.
    Result<void> OnFrameworkPreAwake() noexcept final;

private:
    ROCKS_NATIVE() String tag_;
};

// 게임 규칙을 정하는 서버·standalone 전용 Actor다(UE AGameModeBase 범위, 19.2절). GameInstance
// OpenWorld가 배치 Actor보다 먼저 Spawn하고(배치하지 않는다), 자기 PreAwake에서 GameState를 만들며,
// World BeginPlay를 StartPlay로 시작한다. 만들 클래스(gameStateType·playerStateType)는 파생
// 생성자에서 정한다(비면 기본 클래스). Login·RestartPlayer·Logout은 S12가 더한다.
//
// 사용 예:
//   class ArenaDirector final : public GameDirector {
//       ROCKS_NATIVE_TYPE(ArenaDirector, GameDirector, 0x..., 0x...)
//   public:
//       explicit ArenaDirector(Allocator& allocator) noexcept : GameDirector(allocator) {
//           gameStateType = ObjectTypeIdOf<ArenaState>();
//       }
//       Result<void> InitGame(StringView map, StringView options) noexcept override {
//           StringView rounds;
//           if (FindMapOption(options, "rounds", rounds)) { ... }
//           return Result<void>::Success();
//       }
//   };
//
// 스레드: World의 게임 스레드 전용이다. 객체 포인터는 World::With 범위에서만 쓴다.
ROCKS_CLASS(WorldOnly)
class ROCKS_GAMEPLAY_API GameDirector : public Actor {
    ROCKS_GENERATED_BODY()

public:
    explicit GameDirector(Allocator& allocator) noexcept;
    ~GameDirector() noexcept override;

    // OpenWorld가 넘긴 맵 options다("?key=value" 형식 — FindMapOption). InitGame 전에 보관된다.
    StringView Options() const noexcept {
        return options_.View();
    }

    // 이 Director가 만든 GameState다(PreAwake 뒤, 없으면 null).
    ObjectHandle GameStateHandle() const noexcept {
        return gameState_;
    }

    // 만들 GameState·PlayerState 클래스다(비면 기본 클래스).
    TypeId GameStateType() const noexcept {
        return gameStateType;
    }

    TypeId PlayerStateType() const noexcept {
        return playerStateType;
    }

    // 다음 PlayerId를 준다(이 World에서 1부터 1씩).
    int32 AllocatePlayerId() noexcept {
        return nextPlayerId_++;
    }

    // FinishSpawning 전에 부른다(UE InitGame — GameState는 아직 없다). mapName은 연 맵 이름,
    // options는 Options()와 같다. 실패하면 OpenWorld가 실패한다. 기본은 아무것도 하지 않는다.
    virtual Result<void> InitGame(StringView mapName, StringView options) noexcept;

    // World BeginPlay의 시작이다(UE StartPlay — GameplayWorldSubsystem이 부른다). 기본은 GameState
    // HandleBeginPlay(→ World::DispatchBeginPlay)다. 파생은 재정의해도 GameDirector::StartPlay를
    // 불러야 한다(아니면 World BeginPlay가 NotReady로 실패한다).
    virtual Result<void> StartPlay() noexcept;

    // 시작 위치를 고른다(UE FindPlayerStart·ChoosePlayerStart): incomingName(비면 options의
    // "?start=")과 이름표가 같은 PlayerStart 중 Spawn 순번이 가장 작은 것 → 없으면 PlayerStart 중
    // Spawn 순번이 가장 작은 것 → 없으면 null(원점). 파괴 대기는 빼고, Level 배열 순서와 무관하다.
    // UE는 겹치지 않은 자리 중 무작위다 — 재현성을 위해 첫 번째로 둔다(무작위가 필요하면 재정의).
    // controller는 고르는 대상이다(S12, 기본 규칙은 쓰지 않는다).
    virtual ObjectHandle FindPlayerStart(ObjectHandle controller, StringView incomingName) noexcept;

protected:
    // GameState를 Spawn하고 InitGameState(Director 타입 기록)한다(UE PreInitializeComponents).
    Result<void> OnFrameworkPreAwake() noexcept final;
    // GameplayWorldSubsystem에 등록한다.
    Result<void> OnFrameworkPostAwake() noexcept final;
    void OnFrameworkEndPlay(EndPlayReason reason) noexcept final;

    ROCKS_NATIVE() TypeId gameStateType;
    ROCKS_NATIVE() TypeId playerStateType;

private:
    friend class GameInstance;
    friend class GameState;

    ROCKS_NATIVE() String options_;
    ROCKS_NATIVE() ObjectHandle gameState_;
    ROCKS_NATIVE() int32 nextPlayerId_ = 1;
    // PostAwake까지 마쳤다(아니면 EndPlay가 PreAwake의 GameState를 되돌린다).
    ROCKS_NATIVE() bool postAwake_ = false;
};

} // namespace rocks
