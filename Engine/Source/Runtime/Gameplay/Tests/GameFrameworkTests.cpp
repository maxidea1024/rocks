#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <Gameplay/GameDirector.generated.h>
#include <Gameplay/GameDirector.h>
#include <Gameplay/GameInstance.generated.h>
#include <Gameplay/GameInstanceClient.h>
#include <Gameplay/GameState.generated.h>
#include <Gameplay/GameState.h>
#include <Gameplay/GameplayWorldSubsystem.h>
#include <Gameplay/LocalPlayer.generated.h>
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

// GameDirector·GameState·PlayerState·PlayerStart·GameplayWorldSubsystem 시험이다(Docs/Design/
// EngineRuntime 19절, S11).
using namespace rocks;
namespace gameplay_game_framework_tests_file {

namespace {

constexpr uint32 MaxEvents = 32;
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

class RecordingGameState final : public GameState {
    ROCKS_NATIVE_TYPE(RecordingGameState, GameState, 0x3b9d2e7f1a4c6058ull, 0xd4a17c0e5b8f2936ull)

public:
    explicit RecordingGameState(Allocator& allocator) noexcept : GameState(allocator) {
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

class RecordingDirector final : public GameDirector {
    ROCKS_NATIVE_TYPE(RecordingDirector, GameDirector, 0x81f4c6a2e09d3b57ull, 0x5c2e8b0d7f1a4963ull)

public:
    explicit RecordingDirector(Allocator& allocator) noexcept : GameDirector(allocator) {
        gameStateType = ObjectTypeIdOf<RecordingGameState>();
    }

    Result<void> InitGame(StringView mapName, StringView options) noexcept override {
        Record("director.initgame");
        if (failInit) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        // GameState는 아직 없고 options는 이미 보관됐다.
        CHECK(GameStateHandle().IsNull());
        CHECK(Options() == options);
        StringView rounds;
        CHECK(FindMapOption(options, "Rounds", rounds) && rounds == StringView("3"));
        // 미룬 요청(Deferred)도 복사한 이름이 그대로 온다.
        CHECK(mapName == StringView("Arena") || mapName == StringView("Deferred"));
        return Result<void>::Success();
    }

    Result<void> OnAwake() noexcept override {
        Record("director.awake");
        CHECK(!GameStateHandle().IsNull());
        return Result<void>::Success();
    }

    Result<void> StartPlay() noexcept override {
        Record("director.startplay");
        // 기본 흐름을 부르지 않으면 World BeginPlay가 NotReady로 실패한다.
        return skipBase ? Result<void>::Success() : GameDirector::StartPlay();
    }

    static inline bool skipBase = false;
    static inline bool failInit = false;
};

// OnAwake가 실패하는 Director다(PreAwake가 만든 GameState를 EndPlay가 되돌린다).
class FailingAwakeDirector final : public GameDirector {
    ROCKS_NATIVE_TYPE(FailingAwakeDirector, GameDirector, 0x7a3e0c5d9b1f4268ull,
                      0xe6b2d8104fa9c35bull)

public:
    explicit FailingAwakeDirector(Allocator& allocator) noexcept : GameDirector(allocator) {
    }

    Result<void> OnAwake() noexcept override {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }
};

// BeginFrame(프레임 안)에서 OpenWorld를 요청하는 loop hook이다(미룬 요청의 복사 확인).
class RequestHooks final : public EngineLoopHooks {
public:
    void BeginFrame(Engine&) noexcept override {
        if (!pending || client == nullptr) {
            return;
        }

        pending = false;
        // 요청 뒤 원본 문자열을 지워도 미룬 요청은 자기 사본을 쓴다.
        char name[16] = "Deferred";
        char options[16] = "?Rounds=3";
        MapDesc map = request;
        map.name = StringView(name);
        map.options = StringView(options);
        auto requested = client->WithInstance(
            [&map](GameInstance& instance) noexcept { return instance.OpenWorld(map); });
        CHECK(requested.IsOk());
        memset(name, 'x', sizeof(name) - 1);
        memset(options, 'x', sizeof(options) - 1);
    }

    GameInstanceClient* client = nullptr;
    MapDesc request;
    bool pending = false;
};

class DirectorA final : public GameDirector {
    ROCKS_NATIVE_TYPE(DirectorA, GameDirector, 0x6e0a3d9c4b27f185ull, 0x29c7f15e8d03ab46ull)

public:
    explicit DirectorA(Allocator& allocator) noexcept : GameDirector(allocator) {
    }
};

class DirectorB final : public GameDirector {
    ROCKS_NATIVE_TYPE(DirectorB, GameDirector, 0xa25c7e14f9d036b8ull, 0x73e0b9a4c51d2f86ull)

public:
    explicit DirectorB(Allocator& allocator) noexcept : GameDirector(allocator) {
    }
};

// gameStateType이 GameState가 아닌 Director다(PreAwake가 InvalidArgument로 실패한다).
class BadStateDirector final : public GameDirector {
    ROCKS_NATIVE_TYPE(BadStateDirector, GameDirector, 0x0f7b3c95e2a64d18ull, 0xb8d14a6f30c7e925ull)

public:
    explicit BadStateDirector(Allocator& allocator) noexcept : GameDirector(allocator) {
        gameStateType = ObjectTypeIdOf<PlayerStart>();
    }
};

class RecordingActor final : public Actor {
    ROCKS_NATIVE_TYPE(RecordingActor, Actor, 0x4d81e6b0a93f2c57ull, 0x1a6c9e3f7b05d284ull)

public:
    explicit RecordingActor(Allocator& allocator) noexcept : Actor(allocator) {
    }

    Result<void> OnAwake() noexcept override {
        Record("actor.awake");
        return Result<void>::Success();
    }

    void OnStart() noexcept override {
        Record("actor.start");
    }
};

// B를 받으면 A로 바꾸는 GameInstance다(override hook).
class OverridingGameInstance final : public GameInstance {
    ROCKS_NATIVE_TYPE(OverridingGameInstance, GameInstance, 0xc3e85a1f6d2b4079ull,
                      0x8f2d6b4e0a9c3175ull)

public:
    explicit OverridingGameInstance(Allocator& allocator) noexcept : GameInstance(allocator) {
    }

protected:
    TypeId OverrideDirectorType(TypeId type) noexcept override {
        return overrideBToA && type == ObjectTypeIdOf<DirectorB>() ? ObjectTypeIdOf<DirectorA>()
                                                                   : type;
    }

public:
    static inline bool overrideBToA = false;
};

Result<void> PopulateRecording(World& world, void*) noexcept {
    auto spawned = world.SpawnActor<RecordingActor>();
    return spawned ? Result<void>::Success() : Result<void>::Failure(spawned.Error());
}

template <class T>
Result<void> RegisterOne(TypeRegistry& types) noexcept {
    auto registered = RegisterNativeClass<T>(types);
    return registered ? Result<void>::Success() : Result<void>::Failure(registered.Error());
}

Result<void> RegisterTestTypes(TypeRegistry& types) noexcept {
    Result<void> registered = RegisterOne<RecordingGameState>(types);
    if (registered) {
        registered = RegisterOne<RecordingDirector>(types);
    }

    if (registered) {
        registered = RegisterOne<DirectorA>(types);
    }

    if (registered) {
        registered = RegisterOne<DirectorB>(types);
    }

    if (registered) {
        registered = RegisterOne<BadStateDirector>(types);
    }

    if (registered) {
        registered = RegisterOne<RecordingActor>(types);
    }

    if (registered) {
        registered = RegisterOne<OverridingGameInstance>(types);
    }

    if (registered) {
        registered = RegisterOne<FailingAwakeDirector>(types);
    }

    return registered;
}

struct Fixture {
    Fixture() noexcept {
        game.gameInstanceType = ObjectTypeIdOf<OverridingGameInstance>();
        game.timeStep = {1.0 / 60.0, 0.25, 8};
        game.defaultMap.name = "Arena";
        config.modules = &test::ProcessModules();
        config.fixedFrameDeltaSeconds = 1.0 / 60.0;
        config.registerTypes = &RegisterTestTypes;
        config.hooks = &hooks;
    }

    Result<void> Start() noexcept {
        auto madeClient = UniquePtr<GameInstanceClient>::TryMake(allocator, allocator, game);
        if (!madeClient) {
            return Result<void>::Failure(madeClient.Error());
        }

        client = Move(madeClient.Value());
        config.client = client.Get();
        hooks.client = client.Get();
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
        RecordingDirector::skipBase = false;
        RecordingDirector::failInit = false;
        OverridingGameInstance::overrideBToA = false;
    }

    // 현재 World다(없으면 null).
    World* Current() noexcept {
        auto world = client->WithInstance([](GameInstance& instance) noexcept {
            return reinterpret_cast<uptr>(instance.CurrentWorld());
        });
        return world ? reinterpret_cast<World*>(world.Value()) : nullptr;
    }

    Result<void> Open(const MapDesc& map) noexcept {
        return client->WithInstance(
            [&map](GameInstance& instance) noexcept { return instance.OpenWorld(map); });
    }

    GameplayWorldSubsystem* Gameplay() noexcept {
        World* world = Current();
        return world != nullptr ? world->GetSubsystem<GameplayWorldSubsystem>() : nullptr;
    }

    // 현재 World GameState의 Director 타입이다(없으면 빈 값).
    TypeId DirectorTypeOfCurrent() noexcept {
        GameplayWorldSubsystem* gameplay = Gameplay();
        if (gameplay == nullptr || gameplay->GameStateHandle().IsNull()) {
            return TypeId{};
        }

        auto type =
            Current()->With<GameState>(gameplay->GameStateHandle(), [](GameState& state) noexcept {
                return state.DirectorType();
            });
        return type ? type.Value() : TypeId{};
    }

    SystemAllocator allocator;
    GameInstanceConfig game;
    EngineConfig config;
    RequestHooks hooks;
    UniquePtr<GameInstanceClient> client;
    UniquePtr<Engine> engine;
};

ObjectHandle SpawnPlayerStart(World& world, const char* tag, const Transform& at) noexcept {
    auto start = world.SpawnActor<PlayerStart>(SpawnParams::At(at));
    if (!start) {
        return {};
    }

    auto tagged = world.With<PlayerStart>(
        start.Value(), [tag](PlayerStart& value) noexcept { return value.SetPlayerStartTag(tag); });
    return tagged ? start.Value() : ObjectHandle{};
}

} // namespace

// 19.3절: Director는 Game·Tryout World이고 Client가 아닐 때만 생긴다. GameplayWorldSubsystem은
// OpenWorld가 만드는 World에만, World 종류 기본 판정(Game·Editor·Tryout)으로 생긴다.
TEST(GameFramework_DirectorExistsOnlyForGameWorldsOutsideClients) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    GameplayWorldSubsystem* gameplay = fixture.Gameplay();
    REQUIRE(gameplay != nullptr);
    CHECK(!gameplay->Director().IsNull() && !gameplay->GameStateHandle().IsNull());
    CHECK(fixture.DirectorTypeOfCurrent() == ObjectTypeIdOf<GameDirector>());
    auto started = fixture.Current()->With<GameState>(
        gameplay->GameStateHandle(),
        [](GameState& state) noexcept { return state.HasBegunPlay() && state.HasMatchStarted(); });
    CHECK(started.Value());

    MapDesc editor;
    editor.name = "Editing";
    editor.kind = WorldKind::Editor;
    REQUIRE(fixture.Open(editor));
    REQUIRE(fixture.Gameplay() != nullptr);
    CHECK(fixture.Gameplay()->Director().IsNull());

    MapDesc preview;
    preview.name = "Preview";
    preview.kind = WorldKind::Preview;
    REQUIRE(fixture.Open(preview));
    CHECK(fixture.Gameplay() == nullptr);
    CHECK(fixture.Current()->State() == WorldState::Playing);

    MapDesc tryout;
    tryout.name = "Tryout";
    tryout.kind = WorldKind::Tryout;
    REQUIRE(fixture.Open(tryout));
    CHECK(!fixture.Gameplay()->Director().IsNull());

    // Client는 Director 없이 바로 시작한다(21.4절).
    Fixture client;
    client.game.netMode = NetMode::Client;
    REQUIRE(client.Start());
    REQUIRE(client.Gameplay() != nullptr);
    CHECK(client.Gameplay()->Director().IsNull() && client.Gameplay()->GameStateHandle().IsNull());
    CHECK(client.Current()->State() == WorldState::Playing);
}

// 19.3절: 맵 → 프로젝트 기본 → GameDirector, 마지막에 override hook. 파생이 아니거나 등록되지 않은
// 타입은 InvalidArgument로 OpenWorld가 실패한다.
TEST(GameFramework_DirectorTypePriorityOverrideAndRejection) {
    Fixture fixture;
    fixture.game.defaultDirectorType = ObjectTypeIdOf<DirectorA>();
    REQUIRE(fixture.Start());
    CHECK(fixture.DirectorTypeOfCurrent() == ObjectTypeIdOf<DirectorA>());

    MapDesc map;
    map.name = "Second";
    map.directorType = ObjectTypeIdOf<DirectorB>();
    REQUIRE(fixture.Open(map));
    CHECK(fixture.DirectorTypeOfCurrent() == ObjectTypeIdOf<DirectorB>());

    OverridingGameInstance::overrideBToA = true;
    REQUIRE(fixture.Open(map));
    CHECK(fixture.DirectorTypeOfCurrent() == ObjectTypeIdOf<DirectorA>());
    OverridingGameInstance::overrideBToA = false;

    MapDesc notDirector;
    notDirector.name = "Wrong";
    notDirector.directorType = ObjectTypeIdOf<RecordingActor>();
    CHECK(fixture.Open(notDirector).Error() == ErrorCode::InvalidArgument);
    CHECK(fixture.Current() == nullptr && fixture.engine->WorldCount() == 0);
    MapDesc unknown;
    unknown.name = "Unknown";
    unknown.directorType = TypeId{0x1234ull, 0x5678ull};
    CHECK(fixture.Open(unknown).Error() == ErrorCode::InvalidArgument);

    // Director의 gameStateType이 GameState가 아니면 Director Spawn이 실패한다.
    MapDesc badState;
    badState.name = "BadState";
    badState.directorType = ObjectTypeIdOf<BadStateDirector>();
    CHECK(fixture.Open(badState).Error() == ErrorCode::InvalidArgument);
    CHECK(fixture.engine->WorldCount() == 0);
}

// 19.2·21.2절: InitGame(FinishSpawning 전) → GameState Awake(Director PreAwake) → Director Awake →
// 배치 Actor → BeginPlay: StartPlay → HandleBeginPlay → Actor 시작. StartPlay가 기본 흐름을 부르지
// 않으면 BeginPlay가 NotReady로 실패하고 World를 정리한다.
TEST(GameFramework_InitGameStartPlayOrderAndDispatchContract) {
    Fixture fixture;
    fixture.game.defaultDirectorType = ObjectTypeIdOf<RecordingDirector>();
    fixture.game.defaultMap.options = "?Rounds=3?start=North";
    fixture.game.defaultMap.populate = &PopulateRecording;
    REQUIRE(fixture.Start());
    const char* const started[] = {
        "director.initgame",  "gamestate.awake",           "director.awake", "actor.awake",
        "director.startplay", "gamestate.handlebeginplay", "actor.start"};
    CHECK(EventsAre(started));
    CHECK(fixture.DirectorTypeOfCurrent() == ObjectTypeIdOf<RecordingDirector>());
    // 이미 시작한 GameState의 HandleBeginPlay는 WrongContext이고 상태는 그대로다.
    auto again = fixture.Current()->With<GameState>(
        fixture.Gameplay()->GameStateHandle(), [](GameState& state) noexcept {
            const bool wrong = state.HandleBeginPlay().Error() == ErrorCode::WrongContext;
            return wrong && state.HasBegunPlay() && state.HasMatchStarted();
        });
    CHECK(again.Value());

    // InitGame 실패는 OpenWorld 실패다(새 World 정리).
    RecordingDirector::failInit = true;
    ResetEvents();
    CHECK(fixture.Open(fixture.game.defaultMap).Error() == ErrorCode::DependencyFailed);
    CHECK(fixture.Current() == nullptr && fixture.engine->WorldCount() == 0);
    RecordingDirector::failInit = false;

    RecordingDirector::skipBase = true;
    ResetEvents();
    MapDesc map = fixture.game.defaultMap;
    CHECK(fixture.Open(map).Error() == ErrorCode::NotReady);
    CHECK(fixture.Current() == nullptr && fixture.engine->WorldCount() == 0);
}

// 19.6절(S11 — 직접 Spawn): PlayerState는 Director가 1부터 번호를 매기고 GameState 목록에 추가
// 순서로 들어가며, 파괴되면 빠진다. GameState가 늦게 생기면 이미 있는 PlayerState를 Spawn 순번으로
// 모은다(Level 배열 순서와 무관).
TEST(GameFramework_PlayerStatesNumberRegisterAndLeave) {
    Fixture fixture;
    // 직접 Spawn 경로만 본다(전용 서버 — LocalPlayer·Login이 없다).
    fixture.game.netMode = NetMode::DedicatedServer;
    REQUIRE(fixture.Start());
    World& world = *fixture.Current();
    const ObjectHandle gameState = fixture.Gameplay()->GameStateHandle();
    auto first = world.SpawnActor<PlayerState>();
    auto second = world.SpawnActor<PlayerState>();
    REQUIRE(first && second);
    auto id = [&](ObjectHandle player) noexcept {
        return world
            .With<PlayerState>(player, [](PlayerState& state) noexcept { return state.PlayerId(); })
            .Value();
    };
    CHECK(id(first.Value()) == 1 && id(second.Value()) == 2);
    auto list = [&]() noexcept {
        return world
            .With<GameState>(
                gameState,
                [&](GameState& state) noexcept {
                    const Span<const ObjectHandle> players = state.PlayerStates();
                    return players.Size() == 1 ? (players[0] == second.Value() ? 2 : -1)
                           : players.Size() == 2
                               ? (players[0] == first.Value() && players[1] == second.Value() ? 12
                                                                                              : -2)
                               : -3;
                })
            .Value();
    };
    CHECK(list() == 12);
    REQUIRE(world.DestroyActor(first.Value()));
    REQUIRE(fixture.engine->RunFrame());
    CHECK(list() == 2);

    // 늦게 생긴 GameState의 모으기: Editor World(Director 없음)에서 dummy → x → y Spawn 뒤 dummy를
    // 지워 Level 배열 순서를 바꾼다(끝 원소가 앞으로 온다).
    MapDesc editor;
    editor.name = "Editing";
    editor.kind = WorldKind::Editor;
    REQUIRE(fixture.Open(editor));
    World& editing = *fixture.Current();
    auto dummy = editing.SpawnActor<RecordingActor>();
    auto x = editing.SpawnActor<PlayerState>();
    auto y = editing.SpawnActor<PlayerState>();
    REQUIRE(dummy && x && y);
    REQUIRE(editing.DestroyActor(dummy.Value()));
    REQUIRE(fixture.engine->RunFrame());
    auto late = editing.SpawnActor<GameState>();
    REQUIRE(late);
    auto collected = editing.With<GameState>(late.Value(), [&](GameState& state) noexcept {
        const Span<const ObjectHandle> players = state.PlayerStates();
        return players.Size() == 2 && players[0] == x.Value() && players[1] == y.Value();
    });
    CHECK(collected.Value());
    auto unnumbered = editing.With<PlayerState>(
        x.Value(), [](PlayerState& state) noexcept { return state.PlayerId(); });
    CHECK(unnumbered.Value() == 0);
}

// 19.2절: 이름 일치 중 Spawn 순번 최소 → PlayerStart 중 Spawn 순번 최소 → null. 이름이 비면
// options의
// "?start=". Level 배열 순서와 무관하고 파괴 대기는 뺀다. PlayerStart root에 Spawn Transform이
// 붙는다.
TEST(GameFramework_FindPlayerStartRule) {
    Fixture fixture;
    fixture.game.defaultMap.options = "?start=b";
    REQUIRE(fixture.Start());
    World& world = *fixture.Current();
    const ObjectHandle director = fixture.Gameplay()->Director();
    auto find = [&](const char* name) noexcept {
        return world
            .With<GameDirector>(director,
                                [name](GameDirector& value) noexcept {
                                    return value.FindPlayerStart(ObjectHandle{}, name);
                                })
            .Value();
    };
    CHECK(find("").IsNull());
    auto dummy = world.SpawnActor<RecordingActor>();
    REQUIRE(dummy);
    const ObjectHandle s1 = SpawnPlayerStart(
        world, "a", Transform(Quatd::Identity(), Vec3d(10.0, 0.0, 0.0), Vec3d::One()));
    const ObjectHandle s2 = SpawnPlayerStart(world, "b", Transform::Identity());
    const ObjectHandle s3 = SpawnPlayerStart(world, "b", Transform::Identity());
    REQUIRE(!s1.IsNull() && !s2.IsNull() && !s3.IsNull());
    REQUIRE(world.DestroyActor(dummy.Value()));
    REQUIRE(fixture.engine->RunFrame());
    CHECK(find("a") == s1);
    CHECK(find("b") == s2);
    CHECK(find("zzz") == s1);
    // 이름이 비면 options "?start=b"다.
    CHECK(find("") == s2);
    auto root = world.With<Actor>(s1, [](Actor& actor) noexcept { return actor.RootComponent(); });
    REQUIRE(root && !root.Value().IsNull());
    auto location = world.With<SceneComponent>(root.Value(), [](SceneComponent& scene) noexcept {
        return scene.RelativeTransform().translation.x;
    });
    CHECK(location.Value() == 10.0);
    // 파괴 대기는 고르지 않는다.
    REQUIRE(world.DestroyActor(s2));
    CHECK(find("b") == s3);
    REQUIRE(world.DestroyActor(s1));
    REQUIRE(world.DestroyActor(s3));
    CHECK(find("").IsNull());
}

TEST(GameFramework_FindMapOptionParsesKeysCaseInsensitively) {
    StringView value;
    CHECK(FindMapOption("Arena?start=North?Rounds=3?listen", "rounds", value) &&
          value == StringView("3"));
    CHECK(FindMapOption("?start=North?listen", "LISTEN", value) && value.IsEmpty());
    CHECK(FindMapOption("?start=North", "start", value) && value == StringView("North"));
    value = "kept";
    CHECK(!FindMapOption("start=North", "start", value) && value == StringView("kept"));
    CHECK(!FindMapOption("", "start", value));
    CHECK(!FindMapOption("?starter=1", "start", value));
}

// 리뷰 보강(19.7절): 프레임 안에서 미룬 OpenWorld는 name·options·directorType을 복사해 둔다.
TEST(GameFramework_DeferredOpenKeepsOptionsAndDirectorType) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    fixture.hooks.request.directorType = ObjectTypeIdOf<RecordingDirector>();
    fixture.hooks.pending = true;
    // BeginFrame의 요청은 같은 프레임 OnEngineTick에서 실행된다(그 사이 원본 문자열은 지워졌다).
    // RecordingDirector InitGame이 복사한 맵 이름·options("?Rounds=3")를 확인한다.
    REQUIRE(fixture.engine->RunFrame());
    REQUIRE(fixture.Current() != nullptr);
    CHECK(fixture.Current()->Name() == StringView("Deferred"));
    CHECK(fixture.DirectorTypeOfCurrent() == ObjectTypeIdOf<RecordingDirector>());
    auto options = fixture.Current()->With<GameDirector>(
        fixture.Gameplay()->Director(), [](GameDirector& director) noexcept {
            return director.Options() == StringView("?Rounds=3");
        });
    CHECK(options.Value());
}

// 리뷰 보강(17.8·19.7절): Director OnAwake가 실패하면 PreAwake가 만든 GameState도 정리된다. 지연
// Spawn 중(Constructed)인 PlayerState는 GameState가 모으지 않고 자기 PostAwake에서 들어온다.
TEST(GameFramework_FailedDirectorRollsBackGameStateAndDeferredPlayerStateJoinsLater) {
    Fixture fixture;
    fixture.game.defaultMap.kind = WorldKind::Editor;
    REQUIRE(fixture.Start());
    World& world = *fixture.Current();
    CHECK(world.SpawnActor<FailingAwakeDirector>().Error() == ErrorCode::DependencyFailed);
    REQUIRE(fixture.engine->RunFrame());
    uint32 gameStates = 0;
    world.ForEachActor([&](ObjectHandle actor) noexcept {
        if (world.With<GameState>(actor, [](GameState&) noexcept {})) {
            ++gameStates;
        }
    });
    CHECK(gameStates == 0);
    CHECK(fixture.Gameplay()->GameStateHandle().IsNull() &&
          fixture.Gameplay()->Director().IsNull());

    auto deferred = world.SpawnActorDeferred<PlayerState>();
    REQUIRE(deferred);
    auto state = world.SpawnActor<GameState>();
    REQUIRE(state);
    auto count = [&]() noexcept {
        return world
            .With<GameState>(state.Value(),
                             [](GameState& value) noexcept { return value.PlayerStates().Size(); })
            .Value();
    };
    CHECK(count() == 0);
    REQUIRE(world.FinishSpawning(deferred.Value()));
    CHECK(count() == 1);
}
} // namespace gameplay_game_framework_tests_file
