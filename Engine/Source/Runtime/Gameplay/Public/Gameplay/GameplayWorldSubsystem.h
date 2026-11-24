#pragma once

#include <Core/TypeKey.h>
#include <Gameplay/GameplayTypes.h>

namespace rocks {

// 게임 World와 게임플레이 프레임워크를 잇는 WorldSubsystem이다(Docs/Design/EngineRuntime 19.5절).
// GameInstance OpenWorld가 만드는 World에만 생긴다(World 종류만으로 기본 생성하지 않는다 —
// GameInstance 없이 만든 World에는 없다, 21.4절). GameDirector·GameState handle을 들고 World
// BeginPlay의 시작을 위임받는다: Director가 있으면 GameDirector::StartPlay(→ GameState
// HandleBeginPlay → World::DispatchBeginPlay), 없으면(Client·편집) 바로 DispatchBeginPlay다.
//
// 사용 예(Actor에서 GameState 찾기):
//   auto* gameplay = GetWorld().GetSubsystem<GameplayWorldSubsystem>();
//   if (gameplay != nullptr && !gameplay->GameStateHandle().IsNull()) { ... }
//
// 스레드: World의 게임 스레드 전용이다.
class ROCKS_GAMEPLAY_API GameplayWorldSubsystem final : public WorldSubsystem,
                                                        public BeginPlayDispatcher {
public:
    // allocator는 Controller 목록용이다(묶음 allocator — Type()이 넘긴다).
    explicit GameplayWorldSubsystem(Allocator& allocator) noexcept : controllers_(allocator) {
    }

    // World Subsystem 목록에 넣을 생성 정보다(GameInstance OpenWorld가 더한다). 묶음 allocator를
    // 생성자에 넘긴다.
    static WorldSubsystemType Type() noexcept;

    // BeginPlay dispatcher와 엔진 단계 CameraUpdate(PlayerController 등록 순서로 CameraDirector
    // 갱신 — 20.6절)를 건다.
    Result<void> Initialize(World& world) noexcept override;
    void Deinitialize(World& world) noexcept override;
    Result<void> StartPlay(World& world) noexcept override;

    // 권한이 있는지다(NetMode가 Client가 아니면 참 — Possess·AI 자동 Possess 판정, 20.8절).
    // OpenWorld가 정한다(GameInstance 없이 만든 World에는 이 Subsystem이 없다).
    bool HasAuthority() const noexcept {
        return hasAuthority_;
    }

    // OpenWorld가 맵 배치(populate)를 실행하는 중인지다(Pawn 배치 판정 — 20.8절).
    bool IsPopulating() const noexcept {
        return populating_;
    }

    // 이 World의 Controller·PlayerController·Pawn·PlayerState 수다(파괴 대기 제외 — 22.1절 통계).
    GameplayStats Stats(World& world) const noexcept;

    // 등록된 Controller다(PostAwake 순서, 파괴되면 빠진다).
    Span<const ObjectHandle> Controllers() const noexcept {
        return Span<const ObjectHandle>(controllers_.Data(), controllers_.Size());
    }

    // 이 World의 GameDirector다(없으면 null — Client·편집 World, 아직 Spawn 전).
    ObjectHandle Director() const noexcept {
        return director_;
    }

    // 이 World의 GameState다(없으면 null).
    ObjectHandle GameStateHandle() const noexcept {
        return gameState_;
    }

private:
    friend class Controller;
    friend class GameDirector;
    friend class GameInstance;
    friend class GameState;

    // 엔진 단계 CameraUpdate 콜백이다.
    static Result<void> UpdateCameras(World& world, const TickContext& context,
                                      void* user) noexcept;

    Array<ObjectHandle> controllers_;
    ObjectHandle director_;
    ObjectHandle gameState_;
    bool hasAuthority_ = true;
    bool populating_ = false;
};

} // namespace rocks

// 다른 모듈(게임)이 GetSubsystem<GameplayWorldSubsystem>()으로 찾으므로 모듈 사이에 같은 키를
// 쓴다(World/Subsystem.h SubsystemKey 설명).
ROCKS_DECLARE_NATIVE_TYPE_KEY(rocks::GameplayWorldSubsystem, ROCKS_GAMEPLAY_API)
