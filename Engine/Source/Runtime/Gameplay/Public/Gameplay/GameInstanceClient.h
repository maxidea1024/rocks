#pragma once

#include <Gameplay/GameInstance.h>

namespace rocks {

// GameInstance를 엔진 수명에 잇는 EngineClient 구현이다(Docs/Design/EngineRuntime 18.2절 표). 실행
// 파일이 EngineConfig::client에 넘긴다.
//   OnEngineStart(Initializing): GameInstance 생성 → OnInit → GameInstance Subsystem 초기화 →
//     OnPostInit → 첫 LocalPlayer(DedicatedServer가 아니면, 입력 장치 0). 실패하면 한 단계를
//     역순으로 정리하고 그 오류를 돌려준다(엔진 초기화 실패).
//   OnEngineReady(Running): 기본 맵 OpenWorld → OnStart. 실패하면 Engine이 정상 종료 순서로
//   정리한다. OnEngineTick: 미룬 OpenWorld를 실행한다(실패는 기록만 — 현재 World는 없음).
//   OnEngineShutdown: 현재 World EndPlay(Quit) → OnShutdown → LocalPlayer 역순 제거 → GameInstance
//     Subsystem 정리 → GameInstance 루트 반환. World 파괴는 그 뒤 Engine이 한다.
//
// 사용 예:
//   GameInstanceConfig game;
//   game.defaultMap.name = "Arena";
//   game.defaultMap.populate = &PopulateArena;
//   GameInstanceClient client(allocator, game);
//   EngineConfig config;
//   config.client = &client;
//   engine.Initialize(config);                     // 여기서 Arena World가 BeginPlay까지 열린다
//   client.WithInstance([](GameInstance& game) noexcept { return game.LocalPlayerCount(); });
//
// 스레드: 엔진 게임 스레드 전용이다. client는 Engine보다 오래 살아야 한다.
class ROCKS_GAMEPLAY_API GameInstanceClient final : public EngineClient {
public:
    // allocator(GameInstance Subsystem 묶음)와 config는 client보다 오래 살아야 한다.
    GameInstanceClient(Allocator& allocator, const GameInstanceConfig& config) noexcept;
    GameInstanceClient(const GameInstanceClient&) = delete;
    GameInstanceClient& operator=(const GameInstanceClient&) = delete;
    // OnEngineShutdown(또는 시작 실패 되돌림)이 GameInstance를 반환한 뒤여야 한다(아니면 Fatal).
    ~GameInstanceClient();

    Result<void> OnEngineStart(Engine& engine) noexcept override;
    Result<void> OnEngineReady(Engine& engine) noexcept override;
    void OnEngineTick(Engine& engine) noexcept override;
    void OnEngineShutdown(Engine& engine) noexcept override;
    void OnWorldDestroyed(Engine& engine, World& world) noexcept override;

    // GameInstance handle이다(엔진 Registry, 시작 전·종료 뒤에는 null).
    ObjectHandle Instance() const noexcept {
        return instance_.Handle();
    }

    // GameInstance(또는 T 파생)를 빌려 function을 부른다(World::With와 같은 반환 규칙). 시작
    // 전·종료 뒤에는 StaleHandle이다.
    template <class T = GameInstance, class Function>
    auto WithInstance(Function&& function) noexcept {
        if (engine_ == nullptr) {
            using Returned = decltype(function(DeclVal<T&>()));
            using Output = typename detail::WithResult<Returned>::Type;
            return Output::Failure(ErrorCode::StaleHandle);
        }

        return detail::WithEngineObject<T>(engine_->Objects(), instance_.Handle(),
                                           static_cast<Function&&>(function));
    }

private:
    const GameInstanceConfig& config_;
    // GameInstance Subsystem 묶음·조회 창 context다(Engine보다 오래 살아야 해서 client가 소유).
    detail::GameInstanceServices services_;
    Engine* engine_ = nullptr;
    RootRef<GameInstance> instance_;
};

} // namespace rocks
