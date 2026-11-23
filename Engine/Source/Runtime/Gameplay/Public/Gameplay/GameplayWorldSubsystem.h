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
    Result<void> Initialize(World& world) noexcept override;
    Result<void> StartPlay(World& world) noexcept override;

    // 이 World의 GameDirector다(없으면 null — Client·편집 World, 아직 Spawn 전).
    ObjectHandle Director() const noexcept {
        return director_;
    }

    // 이 World의 GameState다(없으면 null).
    ObjectHandle GameStateHandle() const noexcept {
        return gameState_;
    }

private:
    friend class GameDirector;
    friend class GameState;

    ObjectHandle director_;
    ObjectHandle gameState_;
};

} // namespace rocks

// 다른 모듈(게임)이 GetSubsystem<GameplayWorldSubsystem>()으로 찾으므로 모듈 사이에 같은 키를
// 쓴다(World/Subsystem.h SubsystemKey 설명).
ROCKS_DECLARE_NATIVE_TYPE_KEY(rocks::GameplayWorldSubsystem, ROCKS_GAMEPLAY_API)
