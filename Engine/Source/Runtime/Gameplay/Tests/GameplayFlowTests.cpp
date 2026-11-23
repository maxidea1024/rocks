#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <Gameplay/Controller.generated.h>
#include <Gameplay/GameDirector.generated.h>
#include <Gameplay/GameDirector.h>
#include <Gameplay/GameInstance.generated.h>
#include <Gameplay/GameInstanceClient.h>
#include <Gameplay/GameState.generated.h>
#include <Gameplay/GameState.h>
#include <Gameplay/GameplayWorldSubsystem.h>
#include <Gameplay/LocalPlayer.generated.h>
#include <Gameplay/Pawn.generated.h>
#include <Gameplay/Pawn.h>
#include <Gameplay/PlayerController.generated.h>
#include <Gameplay/PlayerController.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/SceneComponent.h>
#include <stdio.h>
#include <string.h>

// 시험 실행기(Programs/FoundationTests/Private/Main.cpp)가 정의한다.
namespace rocks::test {
ModuleLoader& ProcessModules() noexcept;
} // namespace rocks::test

// 초기화·종료 흐름 시나리오다(Docs/Design/EngineRuntime 21절, S13). 14단계 표의 사건을 정적 문자열
// 로그로 남기고 단계마다 명시 호출·프레임 수로 진행한다(시각·경쟁에 기대지 않는다).
using namespace rocks;
namespace gameplay_flow_tests_file {

namespace {

constexpr uint32 MaxEvents = 128;
const char* events[MaxEvents] = {};
uint32 eventCount = 0;

void Record(const char* text) noexcept {
    if (eventCount < MaxEvents) {
        events[eventCount++] = text;
    }
}

void ResetEvents() noexcept {
    eventCount = 0;
}

void PrintEvents() noexcept {
    printf("  events:");
    for (uint32 index = 0; index < eventCount; ++index) {
        printf(" %s", events[index]);
    }

    printf("\n");
}

template <uint32 Count>
bool EventsAre(const char* const (&expected)[Count]) noexcept {
    bool same = eventCount == Count;
    for (uint32 index = 0; same && index < Count; ++index) {
        same = strcmp(events[index], expected[index]) == 0;
    }

    if (!same) {
        PrintEvents();
    }

    return same;
}

// 사건의 첫 위치다(없으면 MaxEvents).
uint32 IndexOfFirst(const char* text) noexcept {
    for (uint32 index = 0; index < eventCount; ++index) {
        if (strcmp(events[index], text) == 0) {
            return index;
        }
    }

    return MaxEvents;
}

// expected가 사건 로그에 이 순서로(사이에 다른 사건이 있어도) 나타나는지다.
template <uint32 Count>
bool EventsInOrder(const char* const (&expected)[Count]) noexcept {
    uint32 at = 0;
    for (uint32 index = 0; index < Count; ++index) {
        while (at < eventCount && strcmp(events[at], expected[index]) != 0) {
            ++at;
        }

        if (at == eventCount) {
            printf("  missing in order: %s\n", expected[index]);
            PrintEvents();
            return false;
        }

        ++at;
    }

    return true;
}

const char* ReasonEvent(EndPlayReason reason) noexcept {
    switch (reason) {
    case EndPlayReason::Destroyed:
        return "actor.destroy:Destroyed";
    case EndPlayReason::LevelTransition:
        return "actor.destroy:LevelTransition";
    case EndPlayReason::RemovedFromWorld:
        return "actor.destroy:RemovedFromWorld";
    case EndPlayReason::Quit:
        return "actor.destroy:Quit";
    case EndPlayReason::TryoutEnded:
        return "actor.destroy:TryoutEnded";
    }

    return "actor.destroy:?";
}

class FlowEngineSubsystem final : public EngineSubsystem {
public:
    Result<void> Initialize(Engine&) noexcept override {
        Record("es.init");
        return Result<void>::Success();
    }

    void Deinitialize(Engine&) noexcept override {
        Record("es.deinit");
    }
};

class FlowGameSubsystem final : public GameInstanceSubsystem {
public:
    Result<void> Initialize(GameInstance&) noexcept override {
        Record("gis.init");
        return Result<void>::Success();
    }

    void Deinitialize(GameInstance&) noexcept override {
        Record("gis.deinit");
    }
};

class FlowPlayerSubsystem final : public LocalPlayerSubsystem {
public:
    Result<void> Initialize(LocalPlayer&) noexcept override {
        Record("lps.init");
        return Result<void>::Success();
    }

    void Deinitialize(LocalPlayer&) noexcept override {
        Record("lps.deinit");
    }
};

class FlowWorldSubsystem final : public WorldSubsystem {
public:
    Result<void> Initialize(World&) noexcept override {
        Record("ws.init");
        return Result<void>::Success();
    }

    void PostInitialize(World&) noexcept override {
        Record("ws.postinit");
    }

    void OnWorldBeginPlay(World&) noexcept override {
        Record("ws.beginplay");
    }

    void OnWorldEndPlay(World&) noexcept override {
        Record("ws.endplay");
    }

    void Deinitialize(World&) noexcept override {
        Record("ws.deinit");
    }
};

class FlowGameInstance final : public GameInstance {
    ROCKS_NATIVE_TYPE(FlowGameInstance, GameInstance, 0x4b7e1d9a3c06f258ull, 0xe2c95a0f6d1b7384ull)

public:
    explicit FlowGameInstance(Allocator& allocator) noexcept : GameInstance(allocator) {
    }

protected:
    Result<void> OnInit() noexcept override {
        Record("gi.init");
        return Result<void>::Success();
    }

    Result<void> OnPostInit() noexcept override {
        Record("gi.postinit");
        return Result<void>::Success();
    }

    void OnStart() noexcept override {
        Record("gi.start");
    }

    void OnShutdown() noexcept override {
        Record("gi.shutdown");
    }
};

class FlowActor final : public Actor {
    ROCKS_NATIVE_TYPE(FlowActor, Actor, 0x93a0c6e2f15b4d87ull, 0x2d8f4b1e6a7c3059ull)

public:
    explicit FlowActor(Allocator& allocator) noexcept : Actor(allocator) {
    }

    Result<void> OnAwake() noexcept override {
        Record("actor.awake");
        return Result<void>::Success();
    }

    void OnStart() noexcept override {
        Record("actor.start");
    }

    void OnDestroy(EndPlayReason reason) noexcept override {
        Record(ReasonEvent(reason));
    }
};

// 생성자가 nextMode·nextTick을 읽는다(배치 Pawn은 AI 자동 Possess, Tick 없음).
class FlowPawn final : public Pawn {
    ROCKS_NATIVE_TYPE(FlowPawn, Pawn, 0x0e5d8b3f7a2c1964ull, 0xb64a9e2d1f0c8735ull)

public:
    explicit FlowPawn(Allocator& allocator) noexcept : Pawn(allocator) {
        autoPossessAi = nextMode;
        PrimaryTick().tick = nextTick;
    }

    Result<void> OnAwake() noexcept override {
        Record("pawn.awake");
        auto root = GetWorld().AddComponent<SceneComponent>(Handle());
        if (!root) {
            return Result<void>::Failure(root.Error());
        }

        return SetRootComponent(root.Value());
    }

    void OnStart() noexcept override {
        Record("pawn.start");
    }

    void OnTick(const TickContext&) noexcept override {
        Record("pawn.tick");
    }

    void OnDestroy(EndPlayReason) noexcept override {
        Record("pawn.destroy");
    }

    static inline AutoPossessAi nextMode = AutoPossessAi::Disabled;
    static inline bool nextTick = true;

protected:
    void OnPossessed(ObjectHandle) noexcept override {
        Record("pawn.possessed");
    }

    void OnRestart() noexcept override {
        Record("pawn.restart");
    }
};

class FlowAi final : public AiController {
    ROCKS_NATIVE_TYPE(FlowAi, AiController, 0x7c3f0a9e5b1d2846ull, 0x51e8b6d0c4a39f27ull)

public:
    explicit FlowAi(Allocator& allocator) noexcept : AiController(allocator) {
    }

    Result<void> OnAwake() noexcept override {
        Record("ai.awake");
        return Result<void>::Success();
    }
};

class FlowPlayerState final : public PlayerState {
    ROCKS_NATIVE_TYPE(FlowPlayerState, PlayerState, 0xa81d5c3e9f06b472ull, 0x3f7b2e0d8c5a1964ull)

public:
    explicit FlowPlayerState(Allocator& allocator) noexcept : PlayerState(allocator) {
    }

    Result<void> OnAwake() noexcept override {
        Record("ps.awake");
        return Result<void>::Success();
    }
};

class FlowPlayerController final : public PlayerController {
    ROCKS_NATIVE_TYPE(FlowPlayerController, PlayerController, 0x6e2a9d4c1b7f3058ull,
                      0xc0d48f3a2e9b6175ull)

public:
    explicit FlowPlayerController(Allocator& allocator) noexcept : PlayerController(allocator) {
    }

    Result<void> OnAwake() noexcept override {
        Record("pc.awake");
        return Result<void>::Success();
    }

    void OnDestroy(EndPlayReason) noexcept override {
        Record("pc.destroy");
    }

protected:
    void OnPlayerTick(const TickContext&) noexcept override {
        Record("pc.playertick");
    }
};

class FlowGameState final : public GameState {
    ROCKS_NATIVE_TYPE(FlowGameState, GameState, 0x1b9f6c2e4d8a3570ull, 0x87d2e5b0a3c16f49ull)

public:
    explicit FlowGameState(Allocator& allocator) noexcept : GameState(allocator) {
    }

    Result<void> OnAwake() noexcept override {
        Record("gamestate.awake");
        return Result<void>::Success();
    }

    Result<void> HandleBeginPlay() noexcept override {
        Record("gamestate.handlebeginplay");
        return GameState::HandleBeginPlay();
    }
};

class FlowDirector final : public GameDirector {
    ROCKS_NATIVE_TYPE(FlowDirector, GameDirector, 0xf4c08a2d6e3b1957ull, 0x2a6e9d1c5b0f8374ull)

public:
    explicit FlowDirector(Allocator& allocator) noexcept : GameDirector(allocator) {
        gameStateType = ObjectTypeIdOf<FlowGameState>();
        playerStateType = ObjectTypeIdOf<FlowPlayerState>();
        playerControllerType = ObjectTypeIdOf<FlowPlayerController>();
        defaultPawnType = ObjectTypeIdOf<FlowPawn>();
        aiControllerType = ObjectTypeIdOf<FlowAi>();
    }

    Result<void> InitGame(StringView mapName, StringView options) noexcept override {
        Record("director.initgame");
        return GameDirector::InitGame(mapName, options);
    }

    Result<void> OnAwake() noexcept override {
        Record("director.awake");
        return Result<void>::Success();
    }

    void PostLogin(ObjectHandle controller) noexcept override {
        Record("director.postlogin");
        GameDirector::PostLogin(controller);
    }

    Result<void> StartPlay() noexcept override {
        Record("director.startplay");
        return GameDirector::StartPlay();
    }

    void Logout(ObjectHandle controller) noexcept override {
        Record("director.logout");
        GameDirector::Logout(controller);
    }
};

template <class T>
Result<void> RegisterOne(TypeRegistry& types) noexcept {
    auto registered = RegisterNativeClass<T>(types);
    return registered ? Result<void>::Success() : Result<void>::Failure(registered.Error());
}

Result<void> RegisterFlowTypes(TypeRegistry& types) noexcept {
    Result<void> registered = RegisterOne<FlowGameInstance>(types);
    if (registered) {
        registered = RegisterOne<FlowActor>(types);
    }

    if (registered) {
        registered = RegisterOne<FlowPawn>(types);
    }

    if (registered) {
        registered = RegisterOne<FlowAi>(types);
    }

    if (registered) {
        registered = RegisterOne<FlowPlayerState>(types);
    }

    if (registered) {
        registered = RegisterOne<FlowPlayerController>(types);
    }

    if (registered) {
        registered = RegisterOne<FlowGameState>(types);
    }

    if (registered) {
        registered = RegisterOne<FlowDirector>(types);
    }

    return registered;
}

// 7단계 배치: PlayerStart(100, 0, 0) → 배치 Actor → AI가 붙는 배치 Pawn(Tick 없음).
Result<void> PopulateArena(World& world, void*) noexcept {
    const Transform start(Quatd::Identity(), Vec3d(100.0, 0.0, 0.0), Vec3d::One());
    auto placedStart = world.SpawnActor<PlayerStart>(SpawnParams::At(start));
    auto actor = world.SpawnActor<FlowActor>();
    FlowPawn::nextMode = AutoPossessAi::PlacedInWorld;
    FlowPawn::nextTick = false;
    auto pawn = world.SpawnActor<FlowPawn>();
    FlowPawn::nextMode = AutoPossessAi::Disabled;
    FlowPawn::nextTick = true;
    if (!placedStart || !actor || !pawn) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    return Result<void>::Success();
}

const EngineSubsystemType engineSubsystems[] = {
    MakeSubsystemType<EngineSubsystem, FlowEngineSubsystem>("FlowEngine")};
const GameInstanceSubsystemType gameSubsystems[] = {
    MakeSubsystemType<GameInstanceSubsystem, FlowGameSubsystem>("FlowGame")};
const LocalPlayerSubsystemType playerSubsystems[] = {
    MakeSubsystemType<LocalPlayerSubsystem, FlowPlayerSubsystem>("FlowPlayer")};
const WorldSubsystemType worldSubsystems[] = {
    MakeSubsystemType<WorldSubsystem, FlowWorldSubsystem>("FlowWorld")};

World& CurrentOf(GameInstanceClient& client) noexcept {
    auto world = client.WithInstance([](GameInstance& instance) noexcept {
        return reinterpret_cast<uptr>(instance.CurrentWorld());
    });
    return *reinterpret_cast<World*>(world.Value());
}

ObjectHandle FirstPlayerController(GameInstanceClient& client) noexcept {
    auto controller = client.WithInstance([](GameInstance& instance) noexcept {
        return instance.WithLocalPlayer(instance.LocalPlayerAt(0), [](LocalPlayer& local) noexcept {
            return local.PlayerControllerHandle();
        });
    });
    return controller ? controller.Value() : ObjectHandle{};
}

} // namespace

// 21.2절 14단계 시나리오 전체(standalone).
TEST(GameplayFlow_FourteenStepScenario) {
    SystemAllocator allocator;
    {
        GameInstanceConfig game;
        game.gameInstanceType = ObjectTypeIdOf<FlowGameInstance>();
        game.defaultDirectorType = ObjectTypeIdOf<FlowDirector>();
        game.subsystems = gameSubsystems;
        game.localPlayerSubsystems = playerSubsystems;
        game.worldSubsystems = worldSubsystems;
        game.timeStep = {1.0 / 60.0, 0.25, 8};
        game.defaultMap.name = "Arena";
        game.defaultMap.populate = &PopulateArena;
        GameInstanceClient client(allocator, game);
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.client = &client;
        config.subsystems = engineSubsystems;
        config.fixedFrameDeltaSeconds = 1.0 / 60.0;
        config.registerTypes = &RegisterFlowTypes;
        Engine engine(allocator);
        ResetEvents();

        // 1–10단계: Engine 초기화 → GameInstance → LocalPlayer → 기본 맵(World → Director·GameState
        // → 배치 → Login·Possess → BeginPlay).
        REQUIRE(engine.Initialize(config));
        const char* const started[] = {"es.init", // 1
                                       "gi.init",
                                       "gis.init",
                                       "gi.postinit", // 2
                                       "lps.init",    // 3
                                       "ws.init",
                                       "ws.postinit", // 4–5
                                       "director.initgame",
                                       "gamestate.awake",
                                       "director.awake", // 6
                                       "actor.awake",
                                       "pawn.awake",
                                       "ai.awake", // 7 — 배치·AI Possess
                                       "pawn.possessed",
                                       "pawn.restart",
                                       "pc.awake",
                                       "ps.awake", // 8 — Login
                                       "director.postlogin",
                                       "pawn.awake",
                                       "pawn.possessed",
                                       "pawn.restart", // 9
                                       "ws.beginplay",
                                       "director.startplay",
                                       "gamestate.handlebeginplay", // 10
                                       "actor.start",
                                       "pawn.start",
                                       "pawn.start",
                                       "gi.start"};
        CHECK(EventsAre(started));
        World& arena = CurrentOf(client);
        auto* gameplay = arena.GetSubsystem<GameplayWorldSubsystem>();
        REQUIRE(gameplay != nullptr);
        auto matchStarted =
            arena.With<GameState>(gameplay->GameStateHandle(), [](GameState& state) noexcept {
                return state.HasMatchStarted() && state.PlayerStates().Size() == 1;
            });
        CHECK(matchStarted.Value());
        const ObjectHandle controller = FirstPlayerController(client);
        REQUIRE(!controller.IsNull());
        auto pawn = arena.With<Controller>(
            controller, [](Controller& value) noexcept { return value.PawnHandle(); });
        REQUIRE(pawn && !pawn.Value().IsNull());

        // 11단계: 프레임 — PlayerTick → Pawn Tick → 카메라(Pawn 위치 = PlayerStart).
        ResetEvents();
        REQUIRE(engine.RunFrame());
        const char* const frame[] = {"pc.playertick", "pawn.tick"};
        CHECK(EventsAre(frame));
        auto camera =
            arena.With<PlayerController>(controller, [](PlayerController& value) noexcept {
                return value.CameraDirectorHandle();
            });
        auto view = arena.With<CameraDirector>(
            camera.Value(), [](CameraDirector& value) noexcept { return value.View().location.x; });
        CHECK(view.Value() == 100.0);

        // 12단계: 게임 중 Pawn 파괴 → Controller Inactive(PlayerState 유지) → RestartPlayer.
        REQUIRE(arena.DestroyActor(pawn.Value()));
        ResetEvents();
        REQUIRE(engine.RunFrame());
        auto inactive = arena.With<Controller>(controller, [](Controller& value) noexcept {
            return value.State() == ControllerState::Inactive &&
                   !value.PlayerStateHandle().IsNull();
        });
        CHECK(inactive.Value());
        auto restarted = arena.With<GameDirector>(
            gameplay->Director(),
            [controller](GameDirector& director) noexcept { director.RestartPlayer(controller); });
        REQUIRE(restarted);
        const char* const respawned[] = {"pc.playertick", "pawn.destroy", "pawn.awake",
                                         "pawn.possessed", "pawn.restart"};
        CHECK(EventsAre(respawned));

        // 13단계: World 전환 — Pawn → PlayerController(Logout) → EndPlay(LevelTransition) → World
        // 즉시 파괴 → 새 World 5–10단계. LocalPlayer·GameInstance Subsystem은 유지.
        ResetEvents();
        auto opened = client.WithInstance([](GameInstance& instance) noexcept {
            MapDesc next;
            next.name = "Next";
            return instance.OpenWorld(next);
        });
        REQUIRE(opened);
        const char* const transition[] = {"pawn.destroy",
                                          "director.logout",
                                          "pc.destroy",
                                          "actor.destroy:LevelTransition",
                                          "ws.endplay",
                                          "ws.deinit",
                                          "ws.init",
                                          "ws.postinit",
                                          "director.initgame",
                                          "gamestate.awake",
                                          "director.awake",
                                          "pc.awake",
                                          "ps.awake",
                                          "director.postlogin",
                                          "pawn.possessed",
                                          "ws.beginplay",
                                          "director.startplay",
                                          "pawn.start"};
        CHECK(EventsInOrder(transition));
        CHECK(IndexOfFirst("lps.deinit") == MaxEvents && IndexOfFirst("gis.deinit") == MaxEvents);
        CHECK(engine.WorldCount() == 1);

        // 14단계: 종료 — 정리(Logout) → EndPlay(Quit) → OnShutdown → LocalPlayer → GameInstance
        // Subsystem → World 파괴 → EngineSubsystem.
        ResetEvents();
        REQUIRE(engine.Shutdown());
        const char* const stopped[] = {"pawn.destroy", "director.logout", "pc.destroy",
                                       "ws.endplay",   "gi.shutdown",     "lps.deinit",
                                       "gis.deinit",   "ws.deinit",       "es.deinit"};
        CHECK(EventsInOrder(stopped));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 21.4절: GameInstance 없이 만든 World(도구·시험)는 GameplayWorldSubsystem·Director가 없고,
// BeginPlay가 바로 Actor를 시작한다. Editor World는 BeginPlay를 거부한다.
TEST(GameplayFlow_WorldWithoutDirector) {
    SystemAllocator allocator;
    {
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.fixedFrameDeltaSeconds = 1.0 / 60.0;
        config.registerTypes = &RegisterFlowTypes;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        WorldCreateDesc desc;
        desc.name = "Tool";
        desc.subsystems = worldSubsystems;
        desc.beginPlay = false;
        ResetEvents();
        auto created = engine.CreateWorld(desc);
        REQUIRE(created);
        World& world = *created.Value();
        CHECK(world.GetSubsystem<GameplayWorldSubsystem>() == nullptr);
        REQUIRE(world.SpawnActor<FlowActor>());
        REQUIRE(world.BeginPlay());
        const char* const started[] = {"ws.init", "ws.postinit", "actor.awake", "ws.beginplay",
                                       "actor.start"};
        CHECK(EventsAre(started));
        ResetEvents();
        REQUIRE(engine.DestroyWorld(&world));
        const char* const stopped[] = {"actor.destroy:Quit", "ws.endplay", "ws.deinit"};
        CHECK(EventsAre(stopped));

        desc.name = "Editing";
        desc.kind = WorldKind::Editor;
        auto editing = engine.CreateWorld(desc);
        REQUIRE(editing);
        CHECK(editing.Value()->BeginPlay().Error() == ErrorCode::InvalidArgument);
        REQUIRE(engine.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 17.6·18.5절: Engine::DestroyWorld의 사유가 World EndPlay를 거쳐 Actor 정리까지 간다(미룬 파괴의
// 사유 유지는 Engine 시험 Engine_DestroyWorldDefersOnlyDuringWorldTickAndWorldTeardown 경로).
TEST(GameplayFlow_EngineDestroyWorldPassesTheReason) {
    SystemAllocator allocator;
    {
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.fixedFrameDeltaSeconds = 1.0 / 60.0;
        config.registerTypes = &RegisterFlowTypes;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        for (EndPlayReason reason :
             {EndPlayReason::LevelTransition, EndPlayReason::TryoutEnded, EndPlayReason::Quit}) {
            auto created = engine.CreateWorld({});
            REQUIRE(created);
            REQUIRE(created.Value()->SpawnActor<FlowActor>());
            ResetEvents();
            REQUIRE(engine.DestroyWorld(created.Value(), reason));
            CHECK(eventCount >= 1 && strcmp(events[0], ReasonEvent(reason)) == 0);
        }

        REQUIRE(engine.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace gameplay_flow_tests_file
