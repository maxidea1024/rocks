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
#include <Input/InputEvent.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Camera/CameraComponent.generated.h>
#include <World/Component.generated.h>
#include <World/Camera/CameraComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/SceneComponent.h>
#include <stdio.h>
#include <string.h>

// 시험 실행기(Programs/FoundationTests/Private/Main.cpp)가 정의한다.
namespace rocks::test {
ModuleLoader& ProcessModules() noexcept;
} // namespace rocks::test

// Controller·Pawn·PlayerController·CameraDirector·AiController 시험이다(Docs/Design/EngineRuntime
// 20절, S12).
using namespace rocks;
namespace gameplay_controller_pawn_tests_file {

namespace {

constexpr uint32 MaxEvents = 48;
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

template <uint32 Count>
bool EventsAre(const char* const (&expected)[Count]) noexcept {
    bool same = eventCount == Count;
    for (uint32 index = 0; same && index < Count; ++index) {
        same = strcmp(events[index], expected[index]) == 0;
    }

    if (!same) {
        printf("  events:");
        for (uint32 index = 0; index < eventCount; ++index) {
            printf(" %s", events[index]);
        }

        printf("\n");
    }

    return same;
}

// 사건의 첫 위치다(없으면 MaxEvents).
uint32 IndexOf(const char* text) noexcept {
    for (uint32 index = 0; index < eventCount; ++index) {
        if (strcmp(events[index], text) == 0) {
            return index;
        }
    }

    return MaxEvents;
}

// 사건을 남기는 Pawn이다. 생성자가 nextMode를 읽어 autoPossessAi를 정한다(기본 Disabled).
// OnAwake에서 root를 만든다.
class RecordingPawn final : public Pawn {
    ROCKS_NATIVE_TYPE(RecordingPawn, Pawn, 0x5e2b8d0c4a19f736ull, 0x91c4e7a03b6d2f58ull)

public:
    explicit RecordingPawn(Allocator& allocator) noexcept : Pawn(allocator) {
        autoPossessAi = nextMode;
        PrimaryTick().tick = true;
    }

    Result<void> OnAwake() noexcept override {
        auto root = GetWorld().AddComponent<SceneComponent>(Handle());
        if (!root) {
            return Result<void>::Failure(root.Error());
        }

        return SetRootComponent(root.Value());
    }

    void OnTick(const TickContext&) noexcept override {
        Record("pawn.tick");
    }

    void OnDestroy(EndPlayReason) noexcept override {
        Record("pawn.destroy");
    }

    static inline AutoPossessAi nextMode = AutoPossessAi::Disabled;

protected:
    void OnPossessed(ObjectHandle) noexcept override {
        Record("pawn.possessed");
    }

    void OnUnpossessed() noexcept override {
        Record("pawn.unpossessed");
    }

    void OnControllerChanged(ObjectHandle, ObjectHandle) noexcept override {
        Record("pawn.changed");
    }

    void OnRestart() noexcept override {
        Record("pawn.restart");
    }
};

class RecordingAi final : public AiController {
    ROCKS_NATIVE_TYPE(RecordingAi, AiController, 0x2f6a9c1e7d04b583ull, 0xc8e31b5a6f907d24ull)

public:
    explicit RecordingAi(Allocator& allocator) noexcept : AiController(allocator) {
    }

    void OnDestroy(EndPlayReason) noexcept override {
        Record("ai.destroy");
    }

protected:
    void OnPossessedPawnChanged(ObjectHandle, ObjectHandle) noexcept override {
        Record("ctrl.changed");
    }
};

class RecordingPlayerController final : public PlayerController {
    ROCKS_NATIVE_TYPE(RecordingPlayerController, PlayerController, 0x7b0d3e9f5c2a4618ull,
                      0x4e9f1a6c8d3b0725ull)

public:
    explicit RecordingPlayerController(Allocator& allocator) noexcept
        : PlayerController(allocator) {
    }

    void OnDestroy(EndPlayReason) noexcept override {
        Record("pc.destroy");
    }

protected:
    void OnPlayerTick(const TickContext&) noexcept override {
        Record(PlayerInput().IsDown(input::Key::Space) ? "pc.player:space" : "pc.player");
    }

    void OnControllerTick(const TickContext& context) noexcept override {
        Record(context.paused ? "pc.controller:paused" : "pc.controller");
    }
};

class RecordingDirector final : public GameDirector {
    ROCKS_NATIVE_TYPE(RecordingDirector, GameDirector, 0xd16e4b8a2c7f3059ull, 0x08a5c3e9f71b6d42ull)

public:
    explicit RecordingDirector(Allocator& allocator) noexcept : GameDirector(allocator) {
        playerControllerType = ObjectTypeIdOf<RecordingPlayerController>();
        defaultPawnType = ObjectTypeIdOf<RecordingPawn>();
        aiControllerType = ObjectTypeIdOf<RecordingAi>();
    }

    void Logout(ObjectHandle controller) noexcept override {
        Record("director.logout");
        GameDirector::Logout(controller);
    }

    Result<void> InitNewPlayer(ObjectHandle controller) noexcept override {
        if (failInitNewPlayer) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        return GameDirector::InitNewPlayer(controller);
    }

    static inline bool failInitNewPlayer = false;
};

ObjectHandle placedPawn;

// 배치 Pawn 하나(AI 없음)를 둔다.
Result<void> PopulatePlacedPawn(World& world, void*) noexcept {
    auto pawn = world.SpawnActor<RecordingPawn>();
    placedPawn = pawn ? pawn.Value() : ObjectHandle{};
    return pawn ? Result<void>::Success() : Result<void>::Failure(pawn.Error());
}

template <class T>
Result<void> RegisterOne(TypeRegistry& types) noexcept {
    auto registered = RegisterNativeClass<T>(types);
    return registered ? Result<void>::Success() : Result<void>::Failure(registered.Error());
}

Result<void> RegisterTestTypes(TypeRegistry& types) noexcept {
    Result<void> registered = RegisterOne<RecordingPawn>(types);
    if (registered) {
        registered = RegisterOne<RecordingAi>(types);
    }

    if (registered) {
        registered = RegisterOne<RecordingPlayerController>(types);
    }

    if (registered) {
        registered = RegisterOne<RecordingDirector>(types);
    }

    return registered;
}

struct Fixture {
    Fixture() noexcept {
        RecordingPawn::nextMode = AutoPossessAi::Disabled;
        game.defaultDirectorType = ObjectTypeIdOf<RecordingDirector>();
        game.timeStep = {1.0 / 60.0, 0.25, 8};
        game.defaultMap.name = "Arena";
        config.modules = &test::ProcessModules();
        config.fixedFrameDeltaSeconds = 1.0 / 60.0;
        config.registerTypes = &RegisterTestTypes;
    }

    Result<void> Start() noexcept {
        auto madeClient = UniquePtr<GameInstanceClient>::TryMake(allocator, allocator, game);
        if (!madeClient) {
            return Result<void>::Failure(madeClient.Error());
        }

        client = Move(madeClient.Value());
        config.client = client.Get();
        auto madeEngine = UniquePtr<Engine>::TryMake(allocator, allocator);
        if (!madeEngine) {
            return Result<void>::Failure(madeEngine.Error());
        }

        engine = Move(madeEngine.Value());
        ResetEvents();
        return engine->Initialize(config);
    }

    ~Fixture() {
        if (engine) {
            CHECK(engine->Shutdown().IsOk());
        }

        engine.Reset();
        client.Reset();
        RecordingPawn::nextMode = AutoPossessAi::Disabled;
        RecordingDirector::failInitNewPlayer = false;
    }

    World& Current() noexcept {
        auto world = client->WithInstance([](GameInstance& instance) noexcept {
            return reinterpret_cast<uptr>(instance.CurrentWorld());
        });
        return *reinterpret_cast<World*>(world.Value());
    }

    GameplayWorldSubsystem& Gameplay() noexcept {
        return *Current().GetSubsystem<GameplayWorldSubsystem>();
    }

    ObjectHandle Player(uint32 index) noexcept {
        auto player = client->WithInstance(
            [index](GameInstance& instance) noexcept { return instance.LocalPlayerAt(index); });
        return player.Value();
    }

    // index번째 LocalPlayer의 PlayerController다.
    ObjectHandle PlayerControllerOf(uint32 index) noexcept {
        const ObjectHandle player = Player(index);
        auto controller = client->WithInstance([player](GameInstance& instance) noexcept {
            return instance.WithLocalPlayer(
                player, [](LocalPlayer& local) noexcept { return local.PlayerControllerHandle(); });
        });
        return controller ? controller.Value() : ObjectHandle{};
    }

    ObjectHandle PawnOf(ObjectHandle controller) noexcept {
        auto pawn = Current().With<Controller>(
            controller, [](Controller& value) noexcept { return value.PawnHandle(); });
        return pawn ? pawn.Value() : ObjectHandle{};
    }

    SystemAllocator allocator;
    GameInstanceConfig game;
    EngineConfig config;
    UniquePtr<GameInstanceClient> client;
    UniquePtr<Engine> engine;
};

ObjectHandle SpawnPawn(World& world, const Transform& at = Transform::Identity()) noexcept {
    auto pawn = world.SpawnActor<RecordingPawn>(SpawnParams::At(at));
    return pawn ? pawn.Value() : ObjectHandle{};
}

ObjectHandle SpawnAi(World& world) noexcept {
    auto ai = world.SpawnActor<RecordingAi>();
    return ai ? ai.Value() : ObjectHandle{};
}

Result<void> Possess(World& world, ObjectHandle controller, ObjectHandle pawn) noexcept {
    return world.With<Controller>(
        controller, [pawn](Controller& value) noexcept { return value.Possess(pawn); });
}

ObjectHandle ControllerOfPawn(World& world, ObjectHandle pawn) noexcept {
    auto controller =
        world.With<Pawn>(pawn, [](Pawn& value) noexcept { return value.ControllerHandle(); });
    return controller ? controller.Value() : ObjectHandle{};
}

} // namespace

// 20.3·20.4절: Possess는 기존 연결을 풀고 PossessedBy → SetPawn → controlRotation(Pawn 회전) →
// Restart 순이다. 다른 Controller의 Pawn을 빼앗으면 그쪽이 먼저 UnPossess된다.
TEST(ControllerPawn_PossessOrderAndStealing) {
    Fixture fixture;
    fixture.game.netMode = NetMode::DedicatedServer;
    REQUIRE(fixture.Start());
    World& world = fixture.Current();
    const Transform facing(Rotator<double>(0.0, 90.0, 0.0).ToQuaternion(), Vec3d(0.0, 0.0, 0.0),
                           Vec3d::One());
    const ObjectHandle first = SpawnPawn(world, facing);
    const ObjectHandle second = SpawnPawn(world);
    const ObjectHandle alpha = SpawnAi(world);
    const ObjectHandle beta = SpawnAi(world);
    REQUIRE(!first.IsNull() && !second.IsNull() && !alpha.IsNull() && !beta.IsNull());

    ResetEvents();
    REQUIRE(Possess(world, alpha, first));
    const char* const possessed[] = {"pawn.possessed", "pawn.changed", "pawn.restart",
                                     "ctrl.changed"};
    CHECK(EventsAre(possessed));
    CHECK(ControllerOfPawn(world, first) == alpha && fixture.PawnOf(alpha) == first);
    auto linked = world.With<Pawn>(first, [alpha](Pawn& pawn) noexcept {
        const double yaw = pawn.GetControlRotation().yaw;
        return pawn.Owner() == alpha && yaw > 89.999 && yaw < 90.001;
    });
    CHECK(linked.Value());
    // Actor view 회전(SpringArm usePawnControlRotation — EngineRuntime 26.2절)은 control
    // rotation이다.
    CHECK(world.With<Controller>(alpha, [](Controller& value) noexcept {
        value.SetControlRotation(Rotator<double>(-20.0, 30.0, 0.0));
    }));
    auto viewed = world.With<Actor>(first, [](Actor& actor) noexcept {
        Rotator<double> view;
        const bool provided = actor.TryGetViewRotation(view);
        return provided && view.pitch > -20.001 && view.pitch < -19.999 && view.yaw > 29.999 &&
               view.yaw < 30.001;
    });
    CHECK(viewed && viewed.Value());

    // beta가 first를 빼앗는다: alpha UnPossess(→ 이벤트) → beta PossessedBy.
    ResetEvents();
    REQUIRE(Possess(world, beta, first));
    const char* const stolen[] = {"pawn.unpossessed", "pawn.changed", "ctrl.changed",
                                  "pawn.possessed",   "pawn.changed", "pawn.restart",
                                  "ctrl.changed"};
    CHECK(EventsAre(stolen));
    CHECK(fixture.PawnOf(alpha).IsNull() && ControllerOfPawn(world, first) == beta);

    // beta가 second로 옮긴다: first를 먼저 놓는다.
    REQUIRE(Possess(world, beta, second));
    CHECK(ControllerOfPawn(world, first).IsNull() && ControllerOfPawn(world, second) == beta);
    // Pawn이 아니면 InvalidArgument, 없는 handle은 StaleHandle.
    CHECK(Possess(world, beta, alpha).Error() == ErrorCode::InvalidArgument);
    CHECK(Possess(world, beta, ObjectHandle{}).Error() == ErrorCode::StaleHandle);
}

// 20.3·20.4절: Pawn 파괴 → Controller Inactive(PlayerState 유지). PlayerState가 없는 AI는 자신도
// 파괴된다. Controller 파괴 → Pawn은 남고 Logout이 PlayerController EndPlay보다 먼저다.
TEST(ControllerPawn_DestructionKeepsPlayerStateAndLogsOutFirst) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    World& world = fixture.Current();
    const ObjectHandle controller = fixture.PlayerControllerOf(0);
    REQUIRE(!controller.IsNull());
    const ObjectHandle pawn = fixture.PawnOf(controller);
    REQUIRE(!pawn.IsNull());
    auto playerState = world.With<Controller>(
        controller, [](Controller& value) noexcept { return value.PlayerStateHandle(); });
    REQUIRE(playerState && !playerState.Value().IsNull());
    auto statePawn = world.With<PlayerState>(
        playerState.Value(), [](PlayerState& value) noexcept { return value.PawnHandle(); });
    CHECK(statePawn.Value() == pawn);

    ResetEvents();
    REQUIRE(world.DestroyActor(pawn));
    REQUIRE(fixture.engine->RunFrame());
    auto state = world.With<Controller>(controller, [](Controller& value) noexcept {
        return value.State() == ControllerState::Inactive && value.PawnHandle().IsNull();
    });
    CHECK(state.Value());
    CHECK(world.HasActor(playerState.Value()));

    // AI는 Pawn을 잃으면 자신도 파괴된다.
    const ObjectHandle aiPawn = SpawnPawn(world);
    const ObjectHandle ai = SpawnAi(world);
    REQUIRE(Possess(world, ai, aiPawn));
    REQUIRE(world.DestroyActor(aiPawn));
    REQUIRE(fixture.engine->RunFrame());
    REQUIRE(fixture.engine->RunFrame());
    CHECK(!world.HasActor(ai));

    // PlayerController 파괴: Logout → PlayerController 정리, PlayerState 제거, 새 Pawn은 남는다.
    const ObjectHandle kept = SpawnPawn(world);
    REQUIRE(Possess(world, controller, kept));
    ResetEvents();
    REQUIRE(world.DestroyActor(controller));
    REQUIRE(fixture.engine->RunFrame());
    CHECK(IndexOf("pc.destroy") < MaxEvents && IndexOf("director.logout") < IndexOf("pc.destroy"));
    CHECK(world.HasActor(kept) && ControllerOfPawn(world, kept).IsNull());
    CHECK(!world.HasActor(playerState.Value()));
    auto players =
        world.With<GameState>(fixture.Gameplay().GameStateHandle(), [](GameState& value) noexcept {
            return value.PlayerStates().Size();
        });
    CHECK(players.Value() == 0);
}

// 20.3절: SetPawn이 Pawn OnTick → Controller OnTick 선행을 건다(Pawn 우선순위가 더 앞이어도).
// Pawn을 바꾸면 간선이 옮겨 간다.
TEST(ControllerPawn_SetPawnOrdersPawnTickAfterController) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    World& world = fixture.Current();
    const ObjectHandle controller = fixture.PlayerControllerOf(0);
    const ObjectHandle pawn = fixture.PawnOf(controller);
    REQUIRE(!pawn.IsNull());
    REQUIRE(world.SetTickPriority(pawn, -100));
    REQUIRE(fixture.engine->RunFrame());
    ResetEvents();
    REQUIRE(fixture.engine->RunFrame());
    const char* const ordered[] = {"pc.player", "pc.controller", "pawn.tick"};
    CHECK(EventsAre(ordered));

    // 다른 Pawn으로 옮기면 앞 Pawn은 우선순위대로 먼저 돈다.
    const ObjectHandle other = SpawnPawn(world);
    REQUIRE(world.SetTickPriority(other, 100));
    REQUIRE(Possess(world, controller, other));
    REQUIRE(fixture.engine->RunFrame());
    ResetEvents();
    REQUIRE(fixture.engine->RunFrame());
    const char* const moved[] = {"pawn.tick", "pc.player", "pc.controller", "pawn.tick"};
    CHECK(EventsAre(moved));
}

// 20.5절: PlayerTick은 입력 스냅숏 → OnPlayerTick → OnControllerTick이고, pause 중에는
// OnPlayerTick을 건너뛴다. 입력은 SetLocalPlayerInput으로 주입한다(헤드리스).
TEST(ControllerPawn_PlayerTickReadsInjectedInputAndSkipsPlayerTickWhilePaused) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    World& world = fixture.Current();
    input::InputState keyboard;
    const input::InputEvent pressed[] = {input::InputEvent::KeyDown(input::Key::Space)};
    keyboard.BeginFrame(Span<const input::InputEvent>(pressed, 1));
    const ObjectHandle player = fixture.Player(0);
    auto injected = fixture.client->WithInstance([&](GameInstance& instance) noexcept {
        return instance.SetLocalPlayerInput(player, &keyboard);
    });
    REQUIRE(injected);
    REQUIRE(fixture.engine->RunFrame());
    ResetEvents();
    REQUIRE(fixture.engine->RunFrame());
    const char* const ticked[] = {"pc.player:space", "pc.controller", "pawn.tick"};
    CHECK(EventsAre(ticked));

    world.SetPaused(true);
    ResetEvents();
    REQUIRE(fixture.engine->RunFrame());
    const char* const paused[] = {"pc.controller:paused"};
    CHECK(EventsAre(paused));
    world.SetPaused(false);
    auto removed = fixture.client->WithInstance([&](GameInstance& instance) noexcept {
        return instance.SetLocalPlayerInput(player, nullptr);
    });
    CHECK(removed.IsOk());
}

// 입력 액션(Docs/Design/InputActions 5.1절 — IA3): PlayerController가 OnTick에서
// 평가·전달·정리하고, pause 중에도 전달한다(이벤트는 triggerWhenPaused 액션만). 패드 번호는
// LocalPlayer ControllerId다.
TEST(ControllerPawn_InputActionsDispatchFromPlayerTick) {
    // 콜백·bundle의 allocator는 PlayerController보다 오래 살아야 한다(Fixture보다 먼저 만든다).
    SystemAllocator allocator;
    Fixture fixture;
    REQUIRE(fixture.Start());
    World& world = fixture.Current();
    input::InputState keyboard;
    const ObjectHandle player = fixture.Player(0);
    REQUIRE(fixture.client->WithInstance([&](GameInstance& instance) noexcept {
        return instance.SetLocalPlayerInput(player, &keyboard);
    }));
    const ObjectHandle controller = fixture.PlayerControllerOf(0);
    REQUIRE(!controller.IsNull());
    constexpr Guid JumpId{0x4941, 1};
    constexpr Guid MenuId{0x4941, 2};
    input::InputContextBundle bundle(allocator);
    bundle.id = Guid{0x4941, 100};
    for (const Guid id : {JumpId, MenuId}) {
        input::InputActionData action(allocator);
        action.id = id;
        action.settings.triggerWhenPaused = id == MenuId;
        REQUIRE(bundle.actions.TryPushBack(Move(action)));
    }

    for (const char* key : {"SpaceBar", "Escape"}) {
        input::InputBinding binding(allocator);
        binding.action = key[0] == 'S' ? JumpId : MenuId;
        REQUIRE(binding.key.TryAssign(key));
        REQUIRE(bundle.data.bindings.TryPushBack(Move(binding)));
    }

    uint32 jumps = 0;
    uint32 menus = 0;
    auto jump = input::InputActionBindings::Callback::TryCreate(
        allocator, [&jumps](const input::ActionEventData&) noexcept {
            ++jumps;
            Record("action.jump");
        });
    auto menu = input::InputActionBindings::Callback::TryCreate(
        allocator, [&menus](const input::ActionEventData&) noexcept { ++menus; });
    REQUIRE(jump && menu);
    REQUIRE(world.With<PlayerController>(controller, [&](PlayerController& value) noexcept {
        CHECK(value.InputGamepad() == 0);
        CHECK(value.ActionInput().AddContext(Move(bundle), 0).IsOk());
        CHECK(value.ActionBindings()
                  .BindAction(JumpId, input::ActionEvent::Started, Move(jump.Value()))
                  .IsOk());
        CHECK(value.ActionBindings()
                  .BindAction(MenuId, input::ActionEvent::Started, Move(menu.Value()))
                  .IsOk());
    }));
    REQUIRE(fixture.engine->RunFrame());
    const input::InputEvent space[] = {input::InputEvent::KeyDown(input::Key::Space)};
    keyboard.BeginFrame(Span<const input::InputEvent>(space, 1));
    ResetEvents();
    REQUIRE(fixture.engine->RunFrame());
    CHECK(jumps == 1 && menus == 0);
    // 전달이 OnPlayerTick보다 먼저다.
    const char* const order[] = {"action.jump", "pc.player:space", "pc.controller", "pawn.tick"};
    CHECK(EventsAre(order));

    // pause: 전달은 하고 triggerWhenPaused인 Menu만 이벤트가 난다.
    world.SetPaused(true);
    const input::InputEvent escape[] = {input::InputEvent::KeyUp(input::Key::Space),
                                        input::InputEvent::KeyDown(input::Key::Escape)};
    keyboard.BeginFrame(Span<const input::InputEvent>(escape, 2));
    REQUIRE(fixture.engine->RunFrame());
    CHECK(jumps == 1 && menus == 1);
    // pause 중 Space를 다시 눌러도 Jump(triggerWhenPaused 아님)는 이벤트가 없다.
    keyboard.BeginFrame(Span<const input::InputEvent>(space, 1));
    REQUIRE(fixture.engine->RunFrame());
    CHECK(jumps == 1);
    world.SetPaused(false);
    REQUIRE(world.With<PlayerController>(controller, [](PlayerController& value) noexcept {
        value.ActionBindings().ClearBindings();
    }));
    CHECK(fixture.client
              ->WithInstance([&](GameInstance& instance) noexcept {
                  return instance.SetLocalPlayerInput(player, nullptr);
              })
              .IsOk());

    // 패드 번호: ControllerId 2 → 2, 음수 → 0.
    uint32 index = 1;
    for (const int32 id : {2, -3}) {
        auto created = fixture.client->WithInstance(
            [id](GameInstance& instance) noexcept { return instance.CreateLocalPlayer(id); });
        REQUIRE(created);
        const ObjectHandle pc = fixture.PlayerControllerOf(index++);
        REQUIRE(!pc.IsNull());
        uint32 gamepad = 99;
        REQUIRE(world.With<PlayerController>(
            pc, [&gamepad](PlayerController& value) noexcept { gamepad = value.InputGamepad(); }));
        CHECK(gamepad == (id > 0 ? static_cast<uint32>(id) : 0));
    }
}

// 20.6절: CameraDirector는 LateUpdate 시작(CameraUpdate)에 view target(Possess 뒤 Pawn — 위치는
// root, 회전은 controlRotation)으로 갱신한다. UnPossess 뒤에는 PlayerController로 돌아간다. pause
// 중에는 갱신하지 않고 단일 스텝 프레임은 갱신한다.
TEST(ControllerPawn_CameraFollowsViewTargetAndPauseRules) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    World& world = fixture.Current();
    const ObjectHandle controller = fixture.PlayerControllerOf(0);
    const ObjectHandle pawn = fixture.PawnOf(controller);
    auto camera = world.With<PlayerController>(
        controller, [](PlayerController& value) noexcept { return value.CameraDirectorHandle(); });
    REQUIRE(camera && !camera.Value().IsNull());
    auto moveTo = [&](double x) noexcept {
        auto root =
            world.With<Actor>(pawn, [](Actor& actor) noexcept { return actor.RootComponent(); });
        return world.With<SceneComponent>(root.Value(), [x](SceneComponent& scene) noexcept {
            return scene.SetRelativeLocation(Vec3d(x, 0.0, 0.0));
        });
    };
    auto view = [&]() noexcept {
        return world
            .With<CameraDirector>(camera.Value(),
                                  [](CameraDirector& value) noexcept { return value.View(); })
            .Value();
    };
    auto target = [&]() noexcept {
        return world
            .With<CameraDirector>(camera.Value(),
                                  [](CameraDirector& value) noexcept { return value.ViewTarget(); })
            .Value();
    };
    CHECK(target() == pawn);
    REQUIRE(moveTo(250.0));
    REQUIRE(world.With<Controller>(controller, [](Controller& value) noexcept {
        value.SetControlRotation(Rotator<double>(10.0, 45.0, 0.0));
    }));
    REQUIRE(fixture.engine->RunFrame());
    CHECK(view().location.x == 250.0 && view().rotation.yaw == 45.0 &&
          view().rotation.pitch == 10.0);
    CHECK(view().fov == CameraView::DefaultFov);

    world.SetPaused(true);
    REQUIRE(moveTo(500.0));
    REQUIRE(fixture.engine->RunFrame());
    CHECK(view().location.x == 250.0);
    REQUIRE(world.Clock().RequestSingleStep());
    REQUIRE(fixture.engine->RunFrame());
    CHECK(view().location.x == 500.0);
    world.SetPaused(false);

    REQUIRE(
        world.With<Controller>(controller, [](Controller& value) noexcept { value.UnPossess(); }));
    CHECK(target() == controller);
}

// 20.4절: AutoPossessAi 네 값 × 배치(populate 중)·게임 중 Spawn.
TEST(ControllerPawn_AutoPossessAiMatrix) {
    struct Row {
        AutoPossessAi mode;
        bool placed;
        bool spawned;
    };
    static const Row rows[] = {{AutoPossessAi::Disabled, false, false},
                               {AutoPossessAi::PlacedInWorld, true, false},
                               {AutoPossessAi::Spawned, false, true},
                               {AutoPossessAi::PlacedInWorldOrSpawned, true, true}};
    static ObjectHandle placedPawns[4];
    Fixture fixture;
    fixture.game.netMode = NetMode::DedicatedServer;
    fixture.game.defaultMap.populate = [](World& world, void*) noexcept -> Result<void> {
        for (uint32 index = 0; index < 4; ++index) {
            RecordingPawn::nextMode = rows[index].mode;
            placedPawns[index] = SpawnPawn(world);
        }

        RecordingPawn::nextMode = AutoPossessAi::Disabled;
        return Result<void>::Success();
    };
    REQUIRE(fixture.Start());
    World& world = fixture.Current();
    for (uint32 index = 0; index < 4; ++index) {
        const bool placedPossessed = !ControllerOfPawn(world, placedPawns[index]).IsNull();
        CHECK(placedPossessed == rows[index].placed);
        auto placed = world.With<Pawn>(placedPawns[index],
                                       [](Pawn& pawn) noexcept { return pawn.IsPlacedInWorld(); });
        CHECK(placed.Value());
        RecordingPawn::nextMode = rows[index].mode;
        const ObjectHandle spawned = SpawnPawn(world);
        RecordingPawn::nextMode = AutoPossessAi::Disabled;
        const ObjectHandle ai = ControllerOfPawn(world, spawned);
        CHECK(!ai.IsNull() == rows[index].spawned);
        if (!ai.IsNull()) {
            // Director의 aiControllerType(RecordingAi)이고 PlayerState는 없다.
            auto kind = world.With<RecordingAi>(
                ai, [](RecordingAi& value) noexcept { return value.PlayerStateHandle().IsNull(); });
            CHECK(kind.Value());
        }
    }

    // 회전한 Transform으로 Spawn해도 AI의 controlRotation이 그 회전이다(Transform이 PostAwake 전 —
    // S12 리뷰).
    RecordingPawn::nextMode = AutoPossessAi::PlacedInWorldOrSpawned;
    const ObjectHandle turned =
        SpawnPawn(world, Transform(Rotator<double>(0.0, 90.0, 0.0).ToQuaternion(),
                                   Vec3d(0.0, 0.0, 0.0), Vec3d::One()));
    RecordingPawn::nextMode = AutoPossessAi::Disabled;
    const ObjectHandle turnedAi = ControllerOfPawn(world, turned);
    REQUIRE(!turnedAi.IsNull());
    auto yaw = world.With<Controller>(
        turnedAi, [](Controller& value) noexcept { return value.ControlRotation().yaw; });
    CHECK(yaw.Value() > 89.999 && yaw.Value() < 90.001);
    // PlayerState 없는 AI의 파괴는 Logout을 부르지 않는다(UE Controller.cpp:597).
    ResetEvents();
    REQUIRE(world.DestroyActor(turnedAi));
    REQUIRE(fixture.engine->RunFrame());
    CHECK(IndexOf("ai.destroy") < MaxEvents && IndexOf("director.logout") == MaxEvents);

    // Editor World·Client에서는 자동 Possess하지 않는다.
    MapDesc editor;
    editor.name = "Editing";
    editor.kind = WorldKind::Editor;
    auto opened = fixture.client->WithInstance(
        [&editor](GameInstance& instance) noexcept { return instance.OpenWorld(editor); });
    REQUIRE(opened);
    RecordingPawn::nextMode = AutoPossessAi::PlacedInWorldOrSpawned;
    const ObjectHandle idle = SpawnPawn(fixture.Current());
    RecordingPawn::nextMode = AutoPossessAi::Disabled;
    CHECK(ControllerOfPawn(fixture.Current(), idle).IsNull());
}

// 21.3절: World 전환은 Spawn된 Pawn → PlayerController(Logout 먼저) 순으로 정리한다. 배치 Pawn은
// Pawn 정리에 넣지 않는다. 게임 중 추가한 LocalPlayer는 바로 Login하고, 제거하면 PlayerController와
// 그 Pawn이 파괴된다.
TEST(ControllerPawn_TransitionCleanupAndLocalPlayerLogin) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    World& world = fixture.Current();
    const ObjectHandle controller = fixture.PlayerControllerOf(0);
    REQUIRE(!controller.IsNull() && !fixture.PawnOf(controller).IsNull());

    // 게임 중 LocalPlayer 추가 → 바로 Login(PlayerController·Pawn).
    auto second = fixture.client->WithInstance(
        [](GameInstance& instance) noexcept { return instance.CreateLocalPlayer(1); });
    REQUIRE(second);
    const ObjectHandle secondController = fixture.PlayerControllerOf(1);
    REQUIRE(!secondController.IsNull());
    const ObjectHandle secondPawn = fixture.PawnOf(secondController);
    CHECK(!secondPawn.IsNull());
    CHECK(fixture.Gameplay().Controllers().Size() == 2);

    // 제거 → 끊긴 PlayerController가 Pawn도 지운다(UE PawnLeavingGame).
    ResetEvents();
    auto removed = fixture.client->WithInstance([&second](GameInstance& instance) noexcept {
        return instance.RemoveLocalPlayer(second.Value());
    });
    REQUIRE(removed);
    REQUIRE(fixture.engine->RunFrame());
    CHECK(!world.HasActor(secondController) && !world.HasActor(secondPawn));
    CHECK(fixture.Gameplay().Controllers().Size() == 1);

    // 전환: Spawn된 Pawn 정리 → Logout → PlayerController 정리 순서.
    ResetEvents();
    MapDesc next;
    next.name = "Next";
    auto opened = fixture.client->WithInstance(
        [&next](GameInstance& instance) noexcept { return instance.OpenWorld(next); });
    REQUIRE(opened);
    CHECK(IndexOf("pawn.destroy") < IndexOf("director.logout") &&
          IndexOf("director.logout") < IndexOf("pc.destroy") && IndexOf("pc.destroy") < MaxEvents);
    // 새 World에서 다시 Login했다.
    CHECK(!fixture.PlayerControllerOf(0).IsNull());
    CHECK(!fixture.PawnOf(fixture.PlayerControllerOf(0)).IsNull());
}

// 21.3절(S12 리뷰 보강): PlayerController가 배치 Pawn을 조종 중이면 전환 정리는 그 Pawn을 지우지
// 않는다 — Logout(PlayerController 정리)이 먼저이고 배치 Pawn은 EndPlay의 일반 정리에서 지워진다.
// RestartPlayer는 Pawn이 있으면 다시 쓴다.
TEST(ControllerPawn_TransitionKeepsPlacedPawnAndRestartReusesPawn) {
    Fixture fixture;
    fixture.game.defaultMap.populate = &PopulatePlacedPawn;
    REQUIRE(fixture.Start());
    World& world = fixture.Current();
    const ObjectHandle controller = fixture.PlayerControllerOf(0);
    const ObjectHandle spawned = fixture.PawnOf(controller);
    REQUIRE(!placedPawn.IsNull() && !spawned.IsNull() && placedPawn != spawned);

    // Pawn이 있으면 RestartPlayer가 새로 만들지 않는다.
    const ObjectHandle director = fixture.Gameplay().Director();
    REQUIRE(world.With<GameDirector>(
        director, [controller](GameDirector& value) noexcept { value.RestartPlayer(controller); }));
    CHECK(fixture.PawnOf(controller) == spawned);

    REQUIRE(Possess(world, controller, placedPawn));
    ResetEvents();
    auto opened = fixture.client->WithInstance([](GameInstance& instance) noexcept {
        MapDesc next;
        next.name = "Next";
        return instance.OpenWorld(next);
    });
    REQUIRE(opened);
    CHECK(IndexOf("director.logout") < IndexOf("pawn.destroy") &&
          IndexOf("pawn.destroy") < MaxEvents);
}

// 18.4절(S12 리뷰 보강): 게임 중 CreateLocalPlayer의 Login이 실패하면 LocalPlayer도 되돌린다.
TEST(ControllerPawn_CreateLocalPlayerRollsBackWhenLoginFails) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    RecordingDirector::failInitNewPlayer = true;
    auto created = fixture.client->WithInstance(
        [](GameInstance& instance) noexcept { return instance.CreateLocalPlayer(1); });
    CHECK(created.Error() == ErrorCode::DependencyFailed);
    RecordingDirector::failInitNewPlayer = false;
    auto count = fixture.client->WithInstance(
        [](GameInstance& instance) noexcept { return instance.LocalPlayerCount(); });
    CHECK(count.Value() == 1);
    REQUIRE(fixture.engine->RunFrame());
    CHECK(fixture.Gameplay().Controllers().Size() == 1);
}

// 23.4절 CM0c: view target Pawn에 켜진 CameraComponent가 있으면 CameraDirector가 그 view를 쓴다(UE
// CalcCamera) — usePawnControlRotation이면 회전은 controlRotation이다. 꺼지면 기본 규칙(root 위치·
// 기본 시야)으로 돌아간다.
TEST(ControllerPawn_CameraDirectorPrefersCameraComponent) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    World& world = fixture.Current();
    const ObjectHandle controller = fixture.PlayerControllerOf(0);
    const ObjectHandle pawn = fixture.PawnOf(controller);
    REQUIRE(!pawn.IsNull());
    auto root =
        world.With<Actor>(pawn, [](Actor& actor) noexcept { return actor.RootComponent(); });
    REQUIRE(root && !root.Value().IsNull());
    auto camera = world.AddComponent<CameraComponent>(pawn);
    REQUIRE(camera);
    REQUIRE(world.With<CameraComponent>(
        camera.Value(), [&](CameraComponent& value) noexcept -> Result<void> {
            value.SetUsePawnControlRotation(true);
            if (auto fov = value.SetFieldOfView(100.0); !fov) {
                return fov;
            }

            if (auto attached = value.AttachTo(root.Value()); !attached) {
                return attached;
            }

            return value.SetRelativeLocation(Vec3d(0.0, 0.0, 60.0));
        }));
    REQUIRE(world.With<Controller>(controller, [](Controller& value) noexcept {
        value.SetControlRotation(Rotator<double>(10.0, 45.0, 0.0));
    }));
    auto director = world.With<PlayerController>(
        controller, [](PlayerController& value) noexcept { return value.CameraDirectorHandle(); });
    REQUIRE(director);
    auto view = [&]() noexcept {
        return world
            .With<CameraDirector>(director.Value(),
                                  [](CameraDirector& value) noexcept { return value.View(); })
            .Value();
    };
    REQUIRE(fixture.engine->RunFrame());
    CameraView seen = view();
    CHECK(seen.location.z == 60.0 && seen.fov == 100.0);
    CHECK(seen.rotation.pitch > 9.999 && seen.rotation.pitch < 10.001);
    CHECK(seen.rotation.yaw > 44.999 && seen.rotation.yaw < 45.001);

    REQUIRE(world.With<Component>(
        camera.Value(), [](Component& value) noexcept { return value.SetEnabled(false); }));
    REQUIRE(fixture.engine->RunFrame());
    seen = view();
    CHECK(seen.location.z == 0.0 && seen.fov == CameraView::DefaultFov);
}
} // namespace gameplay_controller_pawn_tests_file
