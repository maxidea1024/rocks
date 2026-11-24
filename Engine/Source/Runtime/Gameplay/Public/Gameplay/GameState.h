#pragma once

#include <Core/Text/String.h>
#include <Gameplay/GameplayTypes.h>
#include <Meta/Annotations.h>
#include <World/Actor.h>

namespace rocks {

// 경기 상태다(UE AGameStateBase, Docs/Design/EngineRuntime 19.4절). GameDirector가 자기
// PreAwake에서 Spawn한다(배치하지 않는다). 모든 참여자에게 복제할 상태(네트워크 단계)를 둘 자리이며
// 지금은 Director 타입, PlayerState 목록(추가 순서), BeginPlay·경기 시작 여부를 가진다.
//
// 사용 예(파생 — GameDirector 생성자에서 gameStateType으로 지정):
//   class ArenaState final : public GameState {
//       ROCKS_NATIVE_TYPE(ArenaState, GameState, 0x..., 0x...)
//   public:
//       explicit ArenaState(Allocator& allocator) noexcept : GameState(allocator) {}
//   };
//
// 스레드: World의 게임 스레드 전용이다. 객체 포인터는 World::With 범위에서만 쓴다.
ROCKS_CLASS(WorldOnly)
class ROCKS_GAMEPLAY_API GameState : public Actor {
    ROCKS_GENERATED_BODY()

public:
    explicit GameState(Allocator& allocator) noexcept;
    ~GameState() noexcept override;

    // 이 GameState를 만든 GameDirector의 타입이다(UE GameModeClass — 정보용).
    TypeId DirectorType() const noexcept {
        return directorType_;
    }

    // PlayerState handle 목록이다(추가 순서, UE PlayerArray). 추가·제거 뒤 다시 얻는다.
    Span<const ObjectHandle> PlayerStates() const noexcept {
        return Span<const ObjectHandle>(playerStates_.Data(), playerStates_.Size());
    }

    // HandleBeginPlay가 시작했는지다(UE bReplicatedHasBegunPlay).
    bool HasBegunPlay() const noexcept {
        return hasBegunPlay_;
    }

    // BeginPlay가 끝나 경기가 시작했는지다(UE NotifyMatchStarted 뒤).
    bool HasMatchStarted() const noexcept {
        return matchStarted_;
    }

    // PlayerState를 목록 끝에 더한다. 이미 있으면 아무것도 하지 않는다(UE AddUnique). 오류: 할당
    // 실패.
    Result<void> AddPlayerState(ObjectHandle playerState) noexcept;

    // PlayerState를 뺀다. 없으면 아무것도 하지 않는다(UE Remove). 순서를 지킨다.
    void RemovePlayerState(ObjectHandle playerState) noexcept;

    // World BeginPlay를 시작한다(UE HandleBeginPlay): HasBegunPlay →
    // World::DispatchBeginPlay(BeginPlay 전에 Spawn한 Actor 시작) → HasMatchStarted.
    // GameDirector::StartPlay가 부른다. 파생은 재정의해도 GameState::HandleBeginPlay를 불러야
    // 한다(아니면 World BeginPlay가 NotReady로 실패한다). 실패하면 HasBegunPlay를 되돌리고 그
    // 오류를 돌려준다. 이미 시작했으면 WrongContext다(상태 불변).
    virtual Result<void> HandleBeginPlay() noexcept;

protected:
    // GameplayWorldSubsystem에 등록하고 이미 있는 PlayerState를 Spawn 순번으로 모은다(UE
    // GameStateBase PostInitializeComponents).
    Result<void> OnFrameworkPostAwake() noexcept final;
    void OnFrameworkEndPlay(EndPlayReason reason) noexcept final;

private:
    friend class GameDirector;

    ROCKS_NATIVE() TypeId directorType_;
    ROCKS_NATIVE() Array<ObjectHandle> playerStates_;
    ROCKS_NATIVE() bool hasBegunPlay_ = false;
    ROCKS_NATIVE() bool matchStarted_ = false;
};

// 참여자 한 명의 상태다(UE APlayerState, 19.6절). Controller가 만들고(S12) Pawn이 바뀌거나 죽어도
// 유지되며 Controller와 함께 파괴된다. 만들어질 때 GameDirector가 번호(PlayerId, 1부터)를 매기고
// GameState 목록에 들어가며, 파괴될 때 빠진다.
//
// 사용 예:
//   auto name = world.With<PlayerState>(state, [](PlayerState& player) noexcept {
//       return player.PlayerId();
//   });
//
// 스레드: World의 게임 스레드 전용이다. 객체 포인터는 World::With 범위에서만 쓴다.
ROCKS_CLASS(WorldOnly)
class ROCKS_GAMEPLAY_API PlayerState : public Actor {
    ROCKS_GENERATED_BODY()

public:
    explicit PlayerState(Allocator& allocator) noexcept;
    ~PlayerState() noexcept override;

    // GameDirector가 매긴 번호다(1부터, Director 없는 World에서는 0).
    int32 PlayerId() const noexcept {
        return playerId_;
    }

    StringView PlayerName() const noexcept {
        return playerName_.View();
    }

    // 표시 이름을 바꾼다(UE SetPlayerName). 오류: 할당 실패(이름은 그대로).
    Result<void> SetPlayerName(StringView name) noexcept {
        return playerName_.TryAssign(name);
    }

    bool IsSpectator() const noexcept {
        return isSpectator_;
    }

    void SetIsSpectator(bool spectator) noexcept {
        isSpectator_ = spectator;
    }

    bool IsBot() const noexcept {
        return isBot_;
    }

    void SetIsBot(bool bot) noexcept {
        isBot_ = bot;
    }

    // 지금 조종하는 Pawn이다(없으면 null — Possess가 고친다, S12).
    ObjectHandle PawnHandle() const noexcept {
        return pawn_;
    }

protected:
    // Director가 있으면 PlayerId를 받고, GameState가 있으면 목록에 들어간다(UE PlayerState
    // PostInitializeComponents).
    Result<void> OnFrameworkPostAwake() noexcept final;
    // GameState 목록에서 빠진다(UE PlayerState Destroyed → RemovePlayerState).
    void OnFrameworkEndPlay(EndPlayReason reason) noexcept final;

private:
    friend class Controller;

    ROCKS_NATIVE() String playerName_;
    ROCKS_NATIVE() ObjectHandle pawn_;
    ROCKS_NATIVE() int32 playerId_ = 0;
    ROCKS_NATIVE() bool isSpectator_ = false;
    ROCKS_NATIVE() bool isBot_ = false;
};

} // namespace rocks
