#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <Gameplay/GameInstance.generated.h>
#include <Gameplay/GameInstanceClient.h>
#include <Gameplay/LocalPlayer.generated.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// 시험 실행기(Programs/FoundationTests/Private/Main.cpp)가 정의한다.
namespace rocks::test {
ModuleLoader& ProcessModules() noexcept;
} // namespace rocks::test

// GameInstance·LocalPlayer·GameInstanceClient·OpenWorld 시험이다(Docs/Design/EngineRuntime 18절,
// S10). 사건 순서를 정적 문자열 목록으로 남겨 단언한다.
using namespace rocks;
namespace gameplay_game_instance_tests_file {

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

const char* PlayerEvent(bool added, int32 controllerId) noexcept {
    static const char* const addedNames[] = {"lps.init:0", "lps.init:1", "lps.init:2"};
    static const char* const removedNames[] = {"lps.deinit:0", "lps.deinit:1", "lps.deinit:2"};
    const bool known = controllerId >= 0 && controllerId < 3;
    return !known ? "lps.?" : added ? addedNames[controllerId] : removedNames[controllerId];
}

// 시작 실패 지점이다.
enum class FailAt : uint8 {
    None,
    Init,
    Subsystem,
    PostInit,
    LocalPlayer
};
FailAt failAt = FailAt::None;

class RecordingEngineSubsystem final : public EngineSubsystem {
public:
    void Deinitialize(Engine&) noexcept override {
        Record("es.deinit");
    }
};

class RecordingGameSubsystem final : public GameInstanceSubsystem {
public:
    Result<void> Initialize(GameInstance&) noexcept override {
        Record("gis.init");
        return failAt == FailAt::Subsystem ? Result<void>::Failure(ErrorCode::DependencyFailed)
                                           : Result<void>::Success();
    }

    void Deinitialize(GameInstance&) noexcept override {
        Record("gis.deinit");
    }
};

class RecordingPlayerSubsystem final : public LocalPlayerSubsystem {
public:
    Result<void> Initialize(LocalPlayer& player) noexcept override {
        controllerId = player.ControllerId();
        Record(PlayerEvent(true, controllerId));
        return failAt == FailAt::LocalPlayer ? Result<void>::Failure(ErrorCode::DependencyFailed)
                                             : Result<void>::Success();
    }

    void Deinitialize(LocalPlayer& player) noexcept override {
        Record(PlayerEvent(false, player.ControllerId()));
    }

    int32 controllerId = -1;
};

class TestGameInstance final : public GameInstance {
    ROCKS_NATIVE_TYPE(TestGameInstance, GameInstance, 0x5a7e19c3d40b8f26ull, 0xb18f6e2d09c4a753ull)

public:
    explicit TestGameInstance(Allocator& allocator) noexcept : GameInstance(allocator) {
    }

protected:
    Result<void> OnInit() noexcept override {
        Record("gi.init");
        // Subsystem은 OnInit 뒤에 생긴다.
        CHECK(GetSubsystem<RecordingGameSubsystem>() == nullptr);
        return failAt == FailAt::Init ? Result<void>::Failure(ErrorCode::DependencyFailed)
                                      : Result<void>::Success();
    }

    Result<void> OnPostInit() noexcept override {
        Record("gi.postinit");
        CHECK(GetSubsystem<RecordingGameSubsystem>() != nullptr);
        CHECK(LocalPlayerCount() == 0);
        return failAt == FailAt::PostInit ? Result<void>::Failure(ErrorCode::DependencyFailed)
                                          : Result<void>::Success();
    }

    void OnStart() noexcept override {
        Record("gi.start");
        CHECK(CurrentWorld() != nullptr && CurrentWorld()->State() == WorldState::Playing);
    }

    void OnShutdown() noexcept override {
        Record("gi.shutdown");
        // 현재 World는 이미 끝났고 Subsystem·LocalPlayer는 아직 있다.
        CHECK(CurrentWorld() == nullptr);
        CHECK(GetSubsystem<RecordingGameSubsystem>() != nullptr);
    }
};

GameInstanceClient* activeClient = nullptr;
MapDesc reenterMap;

// 바깥 조회 창(GameInstance → Engine)을 확인하고 World 수명 hook을 남기는 WorldSubsystem이다.
class LookupWorldSubsystem final : public WorldSubsystem {
public:
    Result<void> Initialize(World& world) noexcept override {
        const bool found = GetGameInstanceSubsystem<RecordingGameSubsystem>(world) != nullptr &&
                           GetEngineSubsystem<RecordingEngineSubsystem>(world) != nullptr;
        Record(found ? "ws.init:lookup" : "ws.init:none");
        return Result<void>::Success();
    }

    void OnWorldBeginPlay(World&) noexcept override {
        Record("ws.beginplay");
    }

    void OnWorldEndPlay(World& world) noexcept override {
        Record(world.Name() == StringView("Arena") ? "ws.endplay:Arena" : "ws.endplay");
        // OpenWorld 실행 중(이전 World 정리)의 다시 부르기는 Busy다.
        if (reenter && activeClient != nullptr) {
            reenter = false;
            auto again = activeClient->WithInstance(
                [](GameInstance& game) noexcept { return game.OpenWorld(reenterMap); });
            CHECK(again.Error() == ErrorCode::Busy);
        }
    }

    void Deinitialize(World& world) noexcept override {
        // GameInstance Subsystem 조회 창은 EndPlay 끝에 닫혔다(16.3절).
        Record(GetGameInstanceSubsystem<RecordingGameSubsystem>(world) == nullptr
                   ? "ws.deinit"
                   : "ws.deinit:open");
    }

    static inline bool reenter = false;
};

Result<void> PopulateRecord(World& world, void* context) noexcept {
    Record(world.State() == WorldState::Initialized ? "populate" : "populate:late");
    const bool fail = context != nullptr && *static_cast<bool*>(context);
    return fail ? Result<void>::Failure(ErrorCode::DependencyFailed) : Result<void>::Success();
}

Result<void> RegisterTestTypes(TypeRegistry& types) noexcept {
    auto registered = RegisterNativeClass<TestGameInstance>(types);
    return registered ? Result<void>::Success() : Result<void>::Failure(registered.Error());
}

const EngineSubsystemType engineSubsystems[] = {
    MakeSubsystemType<EngineSubsystem, RecordingEngineSubsystem>("RecordingEngine")};
const GameInstanceSubsystemType gameSubsystems[] = {
    MakeSubsystemType<GameInstanceSubsystem, RecordingGameSubsystem>("RecordingGame")};
const LocalPlayerSubsystemType playerSubsystems[] = {
    MakeSubsystemType<LocalPlayerSubsystem, RecordingPlayerSubsystem>("RecordingPlayer")};
const WorldSubsystemType worldSubsystems[] = {
    MakeSubsystemType<WorldSubsystem, LookupWorldSubsystem>("LookupWorld")};

// BeginFrame(프레임 안, OnEngineTick 전)에서 OpenWorld를 요청하는 loop hook이다.
class OpenRequestHooks final : public EngineLoopHooks {
public:
    void BeginFrame(Engine&) noexcept override {
        for (uint32 index = 0; index < requests; ++index) {
            auto requested = client->WithInstance(
                [this](GameInstance& game) noexcept { return game.OpenWorld(maps[0]); });
            CHECK(requested.IsOk());
            auto pending = client->WithInstance(
                [](GameInstance& game) noexcept { return game.HasPendingOpen(); });
            CHECK(pending.Value());
        }

        requests = 0;
    }

    GameInstanceClient* client = nullptr;
    MapDesc maps[1];
    uint32 requests = 0;
};

struct Fixture {
    Fixture() noexcept {
        failAt = FailAt::None;
        game.gameInstanceType = ObjectTypeIdOf<TestGameInstance>();
        game.subsystems = gameSubsystems;
        game.localPlayerSubsystems = playerSubsystems;
        game.worldSubsystems = worldSubsystems;
        game.timeStep = {1.0 / 60.0, 0.25, 8};
        game.defaultMap.name = "Arena";
        game.defaultMap.populate = &PopulateRecord;
        config.modules = &test::ProcessModules();
        config.subsystems = engineSubsystems;
        config.fixedFrameDeltaSeconds = 1.0 / 60.0;
        config.registerTypes = &RegisterTestTypes;
        config.hooks = &hooks;
    }

    // 엔진을 시작한다(client는 이 때 만든다 — game 설정을 고친 뒤에 부른다).
    Result<void> Start() noexcept {
        auto madeClient = UniquePtr<GameInstanceClient>::TryMake(allocator, game);
        if (!madeClient) {
            return Result<void>::Failure(madeClient.Error());
        }

        client = Move(madeClient.Value());
        activeClient = client.Get();
        hooks.client = client.Get();
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
        activeClient = nullptr;
        failAt = FailAt::None;
    }

    template <class Function>
    auto With(Function&& function) noexcept {
        return client->WithInstance<TestGameInstance>(static_cast<Function&&>(function));
    }

    SystemAllocator allocator;
    GameInstanceConfig game;
    EngineConfig config;
    OpenRequestHooks hooks;
    UniquePtr<GameInstanceClient> client;
    UniquePtr<Engine> engine;
};

} // namespace

// 18.2절 표: OnEngineStart(생성 → OnInit → Subsystem → OnPostInit → 첫 LocalPlayer) → OnEngineReady
// (기본 맵 OpenWorld: World 생성 → populate → BeginPlay → OnStart). 종료는 World EndPlay(Quit) →
// OnShutdown → LocalPlayer 제거 → Subsystem 정리 → (Engine) World 파괴 → EngineSubsystem 정리.
TEST(GameInstance_StartupAndShutdownFollowTheDocumentedOrder) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    const char* const started[] = {"gi.init",        "gis.init", "gi.postinit",  "lps.init:0",
                                   "ws.init:lookup", "populate", "ws.beginplay", "gi.start"};
    CHECK(EventsAre(started));
    CHECK(fixture.engine->WorldCount() == 1);
    auto state = fixture.With([](TestGameInstance& game) noexcept {
        return game.LocalPlayerCount() == 1 && game.CurrentWorld() != nullptr &&
               game.GetNetMode() == NetMode::Standalone && !game.IsDedicatedServer();
    });
    CHECK(state.Value());
    auto player =
        fixture.With([](TestGameInstance& game) noexcept { return game.LocalPlayerAt(0); });
    auto controller = fixture.With([&](TestGameInstance& game) noexcept {
        return game.WithLocalPlayer(player.Value(), [&](LocalPlayer& local) noexcept {
            return local.ControllerId() * 100 + static_cast<int32>(local.Index()) +
                   (local.OwnerHandle() == fixture.client->Instance() ? 10 : 0) +
                   (local.GetSubsystem<RecordingPlayerSubsystem>() != nullptr ? 1000 : 0);
        });
    });
    CHECK(controller.Value() == 1010);

    ResetEvents();
    REQUIRE(fixture.engine->Shutdown());
    const char* const stopped[] = {"ws.endplay:Arena", "gi.shutdown", "lps.deinit:0",
                                   "gis.deinit",       "ws.deinit",   "es.deinit"};
    CHECK(EventsAre(stopped));
    CHECK(fixture.client->Instance().IsNull());
    CHECK(fixture.client->WithInstance([](GameInstance&) noexcept {}).Error() ==
          ErrorCode::StaleHandle);
}

// 18.2절: OnEngineStart의 한 단계가 실패하면 앞 단계를 역순 정리하고 엔진 초기화가 실패한다
// (OnEngineShutdown은 없다). OnEngineReady 실패(기본 맵 열기)는 정상 종료 순서로 정리한다.
TEST(GameInstance_StartFailuresRollBackInReverse) {
    struct Case {
        FailAt at;
        const char* expected[6];
        uint32 count;
    };
    // EngineSubsystem은 엔진 초기화 실패 정리가 마지막에 푼다.
    const Case cases[] = {
        {FailAt::Init, {"gi.init", "es.deinit"}, 2},
        {FailAt::Subsystem, {"gi.init", "gis.init", "es.deinit"}, 3},
        {FailAt::PostInit, {"gi.init", "gis.init", "gi.postinit", "gis.deinit", "es.deinit"}, 5},
        {FailAt::LocalPlayer,
         {"gi.init", "gis.init", "gi.postinit", "lps.init:0", "gis.deinit", "es.deinit"},
         6},
    };
    for (const Case& item : cases) {
        Fixture fixture;
        failAt = item.at;
        CHECK(fixture.Start().Error() == ErrorCode::DependencyFailed);
        CHECK(fixture.engine->State() == EngineState::Failed);
        bool same = eventCount == item.count;
        for (uint32 index = 0; same && index < item.count; ++index) {
            same = strcmp(events[index], item.expected[index]) == 0;
        }

        CHECK(same);
        CHECK(fixture.client->Instance().IsNull());
    }

    // 기본 맵이 없으면 OnEngineReady가 실패하고 OnEngineShutdown부터 정상 종료한다.
    {
        Fixture fixture;
        fixture.game.defaultMap.name = StringView();
        CHECK(fixture.Start().Error() == ErrorCode::InvalidArgument);
        const char* const failed[] = {"gi.init",     "gis.init",     "gi.postinit", "lps.init:0",
                                      "gi.shutdown", "lps.deinit:0", "gis.deinit",  "es.deinit"};
        CHECK(EventsAre(failed));
        CHECK(fixture.engine->State() == EngineState::Failed);
    }

    // 기본 맵 populate가 실패하면 새 World를 정리한 뒤 같은 정상 종료다.
    {
        Fixture fixture;
        bool failPopulate = true;
        fixture.game.defaultMap.populateContext = &failPopulate;
        CHECK(fixture.Start().Error() == ErrorCode::DependencyFailed);
        const char* const failed[] = {"gi.init",        "gis.init",   "gi.postinit", "lps.init:0",
                                      "ws.init:lookup", "populate",   "ws.deinit",   "gi.shutdown",
                                      "lps.deinit:0",   "gis.deinit", "es.deinit"};
        CHECK(EventsAre(failed));
        CHECK(fixture.engine->WorldCount() == 0);
    }
}

// 18.3절: 전용 서버에는 LocalPlayer가 없고 World는 BeginPlay한다.
TEST(GameInstance_DedicatedServerHasNoLocalPlayers) {
    Fixture fixture;
    fixture.game.netMode = NetMode::DedicatedServer;
    REQUIRE(fixture.Start());
    const char* const started[] = {"gi.init",  "gis.init",     "gi.postinit", "ws.init:lookup",
                                   "populate", "ws.beginplay", "gi.start"};
    CHECK(EventsAre(started));
    auto checked = fixture.With([](TestGameInstance& game) noexcept {
        CHECK(game.IsDedicatedServer());
        CHECK(game.LocalPlayerCount() == 0);
        CHECK(game.CreateLocalPlayer(0).Error() == ErrorCode::InvalidArgument);
        return game.CurrentWorld()->State() == WorldState::Playing;
    });
    CHECK(checked.Value());
    CHECK(strcmp(NetModeName(NetMode::DedicatedServer), "DedicatedServer") == 0);
}

// 18.4절: 추가는 Subsystem 초기화, 제거는 정리 뒤 목록에서 빼고 뒤 번호를 앞당긴다. 같은
// controllerId는 AlreadyExists, Subsystem 초기화 실패는 만들지 않은 것으로 되돌린다.
TEST(GameInstance_LocalPlayersAddRemoveAndRenumber) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    ResetEvents();
    auto checked = fixture.With([](TestGameInstance& game) noexcept {
        auto second = game.CreateLocalPlayer(1);
        auto third = game.CreateLocalPlayer(2);
        CHECK(second && third);
        CHECK(game.CreateLocalPlayer(1).Error() == ErrorCode::AlreadyExists);
        const ObjectHandle first = game.LocalPlayerAt(0);
        CHECK(game.RemoveLocalPlayer(first));
        CHECK(game.RemoveLocalPlayer(first).Error() == ErrorCode::NotFound);
        CHECK(game.LocalPlayerCount() == 2);
        auto index = [&](ObjectHandle player) noexcept {
            return game
                .WithLocalPlayer(player, [](LocalPlayer& local) noexcept { return local.Index(); })
                .Value();
        };
        CHECK(index(second.Value()) == 0 && index(third.Value()) == 1);
        CHECK(game.LocalPlayerAt(0) == second.Value() && game.LocalPlayerAt(2).IsNull());

        // LocalPlayer Subsystem 초기화 실패: 만들지 않는다.
        failAt = FailAt::LocalPlayer;
        CHECK(game.CreateLocalPlayer(0).Error() == ErrorCode::DependencyFailed);
        failAt = FailAt::None;
        CHECK(game.LocalPlayerCount() == 2);
        return true;
    });
    CHECK(checked.Value());
    const char* const expected[] = {"lps.init:1", "lps.init:2", "lps.deinit:0", "lps.init:0"};
    CHECK(EventsAre(expected));
    ResetEvents();
    REQUIRE(fixture.engine->Shutdown());
    const char* const stopped[] = {"ws.endplay:Arena", "gi.shutdown", "lps.deinit:2",
                                   "lps.deinit:1",     "gis.deinit",  "ws.deinit",
                                   "es.deinit"};
    CHECK(EventsAre(stopped));
}

// 18.5절: 프레임 안 요청은 다음 OnEngineTick에서 실행하고(마지막 요청만), 이전 World는 EndPlay 뒤
// 새 World 생성 전에 파괴된다. LocalPlayer·GameInstance Subsystem은 유지된다.
TEST(GameInstance_OpenWorldDefersInsideFrameAndKeepsSessionState) {
    Fixture fixture;
    fixture.hooks.maps[0].name = "Second";
    fixture.hooks.maps[0].populate = &PopulateRecord;
    REQUIRE(fixture.Start());
    auto subsystem = fixture.With([](TestGameInstance& game) noexcept {
        return reinterpret_cast<uptr>(game.GetSubsystem<RecordingGameSubsystem>());
    });
    // 같은 프레임에 두 번 요청: 마지막 것만 남는다(Warning).
    fixture.hooks.requests = 2;
    ResetEvents();
    REQUIRE(fixture.engine->RunFrame());
    const char* const reopened[] = {"ws.endplay:Arena", "ws.deinit", "ws.init:lookup", "populate",
                                    "ws.beginplay"};
    CHECK(EventsAre(reopened));
    // 새 World는 파괴된 Arena 자리에 할당될 수 있어 이름으로 확인한다.
    CHECK(fixture.engine->WorldCount() == 1 &&
          fixture.engine->WorldAt(0)->Name() == StringView("Second"));
    auto kept = fixture.With([&](TestGameInstance& game) noexcept {
        return !game.HasPendingOpen() && game.LocalPlayerCount() == 1 &&
               game.CurrentWorld() == fixture.engine->WorldAt(0) &&
               game.CurrentWorld()->Name() == StringView("Second") &&
               reinterpret_cast<uptr>(game.GetSubsystem<RecordingGameSubsystem>()) ==
                   subsystem.Value();
    });
    CHECK(kept.Value());

    // 프레임 밖은 바로 연다. 이름이 비면 InvalidArgument, Editor 종류는 BeginPlay하지 않는다.
    ResetEvents();
    auto direct = fixture.With([](TestGameInstance& game) noexcept {
        MapDesc empty;
        CHECK(game.OpenWorld(empty).Error() == ErrorCode::InvalidArgument);
        MapDesc editor;
        editor.name = "Editing";
        editor.kind = WorldKind::Editor;
        CHECK(game.OpenWorld(editor));
        return game.CurrentWorld()->State() == WorldState::Initialized;
    });
    CHECK(direct.Value());
    const char* const edited[] = {"ws.endplay", "ws.deinit", "ws.init:lookup"};
    CHECK(EventsAre(edited));
}

// 18.5절: populate 실패는 새 World를 정리하고 현재 World는 없음이 된다. 정리 hook 안의 OpenWorld는
// Busy다.
TEST(GameInstance_OpenWorldFailureAndReentry) {
    Fixture fixture;
    REQUIRE(fixture.Start());
    ResetEvents();
    LookupWorldSubsystem::reenter = true;
    reenterMap.name = "Nested";
    bool failPopulate = true;
    auto failed = fixture.With([&](TestGameInstance& game) noexcept {
        MapDesc broken;
        broken.name = "Broken";
        broken.populate = &PopulateRecord;
        broken.populateContext = &failPopulate;
        return game.OpenWorld(broken);
    });
    CHECK(failed.Error() == ErrorCode::DependencyFailed);
    CHECK(!LookupWorldSubsystem::reenter);
    const char* const expected[] = {"ws.endplay:Arena", "ws.deinit", "ws.init:lookup", "populate",
                                    "ws.deinit"};
    CHECK(EventsAre(expected));
    CHECK(fixture.engine->WorldCount() == 0);
    auto current = fixture.With(
        [](TestGameInstance& game) noexcept { return game.CurrentWorld() == nullptr; });
    CHECK(current.Value());
    // 현재 World가 없어도 다시 열 수 있고 종료는 정상이다.
    ResetEvents();
    auto opened = fixture.With([](TestGameInstance& game) noexcept {
        MapDesc map;
        map.name = "Recovered";
        return game.OpenWorld(map);
    });
    CHECK(opened.IsOk());
    const char* const recovered[] = {"ws.init:lookup", "ws.beginplay"};
    CHECK(EventsAre(recovered));
}

} // namespace gameplay_game_instance_tests_file
