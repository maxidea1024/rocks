#include <Core/Diagnostics/Log.h>
#include <Core/ScopeExit.h>
#include <Gameplay/Controller.generated.h>
#include <Gameplay/GameDirector.generated.h>
#include <Gameplay/GameDirector.h>
#include <Gameplay/GameInstance.generated.h>
#include <Gameplay/GameInstanceClient.h>
#include <Gameplay/GameplayWorldSubsystem.h>
#include <Gameplay/Pawn.generated.h>
#include <Gameplay/Pawn.h>
#include <Gameplay/PlayerController.generated.h>
#include <Gameplay/PlayerController.h>
#include <Gameplay/LocalPlayer.generated.h>
#include <Platform/Fatal.h>

// GameInstance·GameInstanceClient다(Docs/Design/EngineRuntime 18절).
namespace rocks {

namespace {

// OpenWorld 실패를 단계 이름과 함께 한 번 기록한다.
void LogOpenFailure(StringView map, const char* stage, ErrorCode error) noexcept {
    Log::Error(LogGameplay, "OpenWorld {Map} failed during {Stage}: {Error}", map, stage,
               ErrorCodeName(error));
}

} // namespace

GameInstance::GameInstance(Allocator& allocator) noexcept
    : localPlayers_(allocator), pendingName_(allocator), pendingOptions_(allocator) {
}

GameInstance::~GameInstance() noexcept = default;

Result<ObjectHandle> GameInstance::CreateLocalPlayer(int32 controllerId,
                                                     bool spawnPlayerController) noexcept {
    // 전용 서버에는 LocalPlayer가 없다(UE GameInstance.cpp:854–861).
    if (IsDedicatedServer()) {
        return Result<ObjectHandle>::Failure(ErrorCode::InvalidArgument);
    }

    ObjectRegistry& objects = engine_->Objects();
    ObjectAccessScope scope(objects.OwnerAccess());
    for (ObjectHandle existing : localPlayers_.View()) {
        auto same = WithLocalPlayer(existing, [controllerId](LocalPlayer& player) noexcept {
            return player.controllerId_ == controllerId;
        });
        if (same && same.Value()) {
            return Result<ObjectHandle>::Failure(ErrorCode::AlreadyExists);
        }
    }

    const TypeId type = config_->localPlayerType != TypeId{} ? config_->localPlayerType
                                                             : ObjectTypeIdOf<LocalPlayer>();
    auto created = objects.NewObject<LocalPlayer>(objects.OwnerAccess(), type);
    if (!created) {
        return Result<ObjectHandle>::Failure(created.Error());
    }

    // 생성 루트는 목록 간선이 생길 때까지만 쥔다(함수 끝에 놓는다 — 실패하면 엔진 Registry GC가
    // 회수한다).
    RootRef<LocalPlayer> root = Move(created.Value());
    const ObjectHandle handle = root.Handle();
    const ObjectHandle owner = Handle();
    const uint32 index = LocalPlayerCount();
    // UE AddLocalPlayer처럼 목록에 먼저 넣고 Subsystem을 초기화한다(UE PlayerAdded —
    // LocalPlayer.cpp:262).
    if (auto added = objects.AddReference(objects.OwnerAccess(), owner, localPlayers_, handle);
        !added) {
        return Result<ObjectHandle>::Failure(added.Error());
    }

    const Span<const LocalPlayerSubsystemType> types = config_->localPlayerSubsystems;
    auto initialized = WithLocalPlayer(handle, [&](LocalPlayer& player) noexcept {
        player.owner_ = owner;
        player.controllerId_ = controllerId;
        player.index_ = index;
        // 실패하면 묶음이 이미 초기화한 것을 되돌린다.
        return player.subsystems_.Initialize(types, player);
    });
    if (!initialized) {
        auto removed =
            objects.RemoveReferenceAt(objects.OwnerAccess(), owner, localPlayers_, index);
        (void)removed;
        return Result<ObjectHandle>::Failure(initialized.Error());
    }

    // 게임 중 추가면 바로 PlayerController를 만든다(UE SpawnPlayActor). 실패하면 LocalPlayer도
    // 되돌린다(UE RemoveLocalPlayer).
    if (spawnPlayerController && currentWorld_ != nullptr &&
        currentWorld_->State() == WorldState::Playing) {
        if (auto logged = LoginPlayer(*currentWorld_, handle); !logged) {
            auto removed = RemoveLocalPlayer(handle);
            (void)removed;
            return Result<ObjectHandle>::Failure(logged.Error());
        }
    }

    return Result<ObjectHandle>::Success(handle);
}

Result<void> GameInstance::SetLocalPlayerInput(ObjectHandle player,
                                               const input::InputState* input) noexcept {
    ObjectHandle controller;
    bool found = false;
    for (ObjectHandle existing : localPlayers_.View()) {
        found = found || existing == player;
    }

    if (!found) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    auto set = WithLocalPlayer(player, [&](LocalPlayer& local) noexcept {
        local.input_ = input;
        controller = local.playerController_;
    });
    if (!set) {
        return set;
    }

    if (currentWorld_ != nullptr && !controller.IsNull() && currentWorld_->HasActor(controller)) {
        auto changed = currentWorld_->With<PlayerController>(
            controller, [input](PlayerController& value) noexcept { value.SetInputSource(input); });
        (void)changed;
    }

    return Result<void>::Success();
}

Result<void> GameInstance::LoginPlayer(World& world, ObjectHandle player) noexcept {
    auto* gameplay = world.GetSubsystem<GameplayWorldSubsystem>();
    if (gameplay == nullptr || gameplay->Director().IsNull()) {
        return Result<void>::Success();
    }

    const ObjectHandle director = gameplay->Director();
    const input::InputState* input = nullptr;
    auto read =
        WithLocalPlayer(player, [&input](LocalPlayer& local) noexcept { input = local.input_; });
    if (!read) {
        return read;
    }

    // 21.2절 8–9단계: Login → SetPlayer(양방향) → PostLogin(→ RestartPlayer → Possess).
    auto controller = world.With<GameDirector>(
        director, [player](GameDirector& value) noexcept { return value.Login(player); });
    if (!controller) {
        return Result<void>::Failure(controller.Error());
    }

    const ObjectHandle created = controller.Value();
    auto linked =
        world.With<PlayerController>(created, [player, input](PlayerController& value) noexcept {
            value.SetPlayer(player, input);
        });
    if (!linked) {
        return linked;
    }

    auto noted = WithLocalPlayer(
        player, [created](LocalPlayer& local) noexcept { local.playerController_ = created; });
    (void)noted;
    return world.With<GameDirector>(
        director, [created](GameDirector& value) noexcept { value.PostLogin(created); });
}

void GameInstance::CleanupPlayers(World& world) noexcept {
    // 21.3절: Pawn을 먼저, 그다음 PlayerController를 대기에 넣는다(UE LoadMap — UnrealEngine.cpp
    // :16245–16250). EndPlay가 넣은 순서대로 정리하므로 Logout이 PlayerController EndPlay 전에
    // 돈다.
    for (ObjectHandle player : localPlayers_.View()) {
        ObjectHandle controller;
        auto read = WithLocalPlayer(player, [&controller](LocalPlayer& local) noexcept {
            controller = local.playerController_;
            local.playerController_ = {};
        });
        if (!read || controller.IsNull() || !world.HasActor(controller)) {
            continue;
        }

        auto pawn = world.With<Controller>(
            controller, [](Controller& value) noexcept { return value.PawnHandle(); });
        if (pawn && !pawn.Value().IsNull()) {
            auto placed = world.With<Pawn>(
                pawn.Value(), [](Pawn& value) noexcept { return value.IsPlacedInWorld(); });
            if (placed && !placed.Value()) {
                auto destroyed = world.DestroyActor(pawn.Value());
                (void)destroyed;
            }
        }

        auto destroyed = world.DestroyActor(controller);
        (void)destroyed;
    }
}

Result<void> GameInstance::RemoveLocalPlayer(ObjectHandle player) noexcept {
    const Span<const ObjectHandle> players = localPlayers_.View();
    for (usize index = 0; index < players.Size(); ++index) {
        if (players[index] == player) {
            return RemoveLocalPlayerAt(index);
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> GameInstance::RemoveLocalPlayerAt(usize index) noexcept {
    // UE PlayerRemoved(LocalPlayer.cpp:270)처럼 목록에서 빼기 전에 Subsystem을 정리한다. 정리
    // 중에도 목록·Index는 그대로다.
    ObjectRegistry& objects = engine_->Objects();
    ObjectAccessScope scope(objects.OwnerAccess());
    // 그 PlayerController를 끊고 파괴한다(UE RemoveLocalPlayer — 끊긴 PlayerController는 Pawn도
    // 지운다, 20.5절).
    ObjectHandle controller;
    auto unlinked =
        WithLocalPlayer(localPlayers_.View()[index], [&controller](LocalPlayer& player) noexcept {
            controller = player.playerController_;
            player.playerController_ = {};
        });
    (void)unlinked;
    if (currentWorld_ != nullptr && !controller.IsNull() && currentWorld_->HasActor(controller)) {
        auto cleared = currentWorld_->With<PlayerController>(
            controller, [](PlayerController& value) noexcept { value.ClearPlayer(); });
        (void)cleared;
        auto destroyed = currentWorld_->DestroyActor(controller);
        (void)destroyed;
    }

    auto removed = WithLocalPlayer(localPlayers_.View()[index], [](LocalPlayer& player) noexcept {
        player.subsystems_.Deinitialize(player);
    });
    // 간선을 끊으면 LocalPlayer는 엔진 Registry GC가 회수한다. 순서를 지킨다(Index 규칙).
    auto erased = objects.RemoveReferenceAt(objects.OwnerAccess(), Handle(), localPlayers_, index);
    if (!erased) {
        return erased;
    }

    // 뒤 LocalPlayer의 로컬 번호를 앞당긴다.
    const Span<const ObjectHandle> players = localPlayers_.View();
    for (usize later = index; later < players.Size(); ++later) {
        auto renumbered = WithLocalPlayer(players[later], [later](LocalPlayer& player) noexcept {
            player.index_ = static_cast<uint32>(later);
        });
        (void)renumbered;
    }

    return removed;
}

void GameInstance::RemoveAllLocalPlayers() noexcept {
    // 나중에 추가한 것부터 지운다(UE Shutdown — GameInstance.cpp:154–160).
    while (LocalPlayerCount() != 0) {
        const uint32 before = LocalPlayerCount();
        auto removed = RemoveLocalPlayerAt(before - 1);
        (void)removed;
        // 간선을 끊지 못하는 것은 Registry 불변식 위반이다(종료가 멈춘다).
        if (LocalPlayerCount() == before) {
            platform::Fatal("GameInstance could not remove a local player during shutdown");
        }
    }
}

OuterSubsystemLookup GameInstance::SubsystemLookup() const noexcept {
    OuterSubsystemLookup lookup;
    // context는 client가 소유한 묶음이다(GameInstance 객체를 빌리지 않는다 — client는 Engine보다
    // 오래 살므로 World Shutdown까지 유효하다).
    lookup.context = services_;
    lookup.find = [](const void* context, SubsystemKey key) noexcept -> void* {
        // 계층 순서(16.3절): GameInstance Subsystem → EngineSubsystem. 각 계층의 Base 포인터를
        // 돌려주고 타입 도우미(GetGameInstanceSubsystem·GetEngineSubsystem)가 되돌린다. 키는
        // 타입마다 하나라 다른 계층과 겹치지 않는다.
        const auto& services = *static_cast<const detail::GameInstanceServices*>(context);
        if (services.visible) {
            if (GameInstanceSubsystem* found = services.subsystems.Find(key)) {
                return found;
            }
        }

        return services.engine->FindSubsystem(key);
    };
    return lookup;
}

Result<void> GameInstance::OpenWorld(const MapDesc& map) noexcept {
    if (opening_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (map.name.IsEmpty() || engine_->State() != EngineState::Running) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 프레임 안(World Tick·loop hook)이면 다음 OnEngineTick으로 미룬다. OnEngineTick 안과 프레임
    // 밖에서는 바로 연다(World 전환은 프레임 사이에서만 — 18.5절).
    if (engine_->IsInFrame() && !inEngineTick_) {
        // 둘 다 복사한 뒤에만 바꾼다(앞 요청과 섞이지 않게).
        String name(pendingName_.GetAllocator());
        String options(pendingOptions_.GetAllocator());
        auto copied = name.TryAssign(map.name);
        if (copied) {
            copied = options.TryAssign(map.options);
        }

        if (!copied) {
            return copied;
        }

        pendingName_ = Move(name);
        pendingOptions_ = Move(options);

        if (hasPendingOpen_) {
            Log::Warning(LogGameplay, "OpenWorld {Map} replaces an earlier pending request",
                         map.name);
        }

        pendingMap_ = map;
        pendingMap_.name = pendingName_.View();
        pendingMap_.options = pendingOptions_.View();
        hasPendingOpen_ = true;
        return Result<void>::Success();
    }

    // 바로 여는 요청이 앞서 미룬 요청을 대신한다(마지막 요청만 남는다).
    hasPendingOpen_ = false;
    return OpenWorldNow(map);
}

Result<void> GameInstance::RunPendingOpen() noexcept {
    if (!hasPendingOpen_) {
        return Result<void>::Success();
    }

    hasPendingOpen_ = false;
    // OpenWorldNow 안의 정리 hook이 새 요청을 남길 수 있어 복사해서 쓴다.
    String name(pendingName_.GetAllocator());
    String options(pendingOptions_.GetAllocator());
    auto copied = name.TryAssign(pendingName_.View());
    if (copied) {
        copied = options.TryAssign(pendingOptions_.View());
    }

    if (!copied) {
        LogOpenFailure(pendingName_.View(), "Request", copied.Error());
        return copied;
    }

    MapDesc map = pendingMap_;
    map.name = name.View();
    map.options = options.View();
    return OpenWorldNow(map);
}

void GameInstance::CloseCurrentWorld(EndPlayReason reason) noexcept {
    if (currentWorld_ == nullptr) {
        return;
    }

    World* previous = currentWorld_;
    currentWorld_ = nullptr;
    // 21.3절 정리 → EndPlay(대기 순서대로 Pawn → PlayerController → 나머지 → OnWorldEndPlay).
    // 파괴는 World Tick 밖이므로 즉시다(Engine::DestroyWorld 규칙).
    CleanupPlayers(*previous);
    auto ended = previous->EndPlay(reason);
    (void)ended;
    auto destroyed = engine_->DestroyWorld(previous, reason);
    // Busy(외부 보유권)면 Engine이 다음 프레임부터 다시 시도하고 기록한다.
    (void)destroyed;
}

Result<void> GameInstance::OpenWorldNow(const MapDesc& map) noexcept {
    opening_ = true;
    auto finished = MakeScopeExit([this]() noexcept { opening_ = false; });
    Log::Info(LogGameplay, "Opening world {Map}", map.name);
    CloseCurrentWorld(EndPlayReason::LevelTransition);

    WorldCreateDesc desc;
    desc.name = map.name;
    desc.timeStep = config_->timeStep;
    // 설정 목록 + GameplayWorldSubsystem(client 소유 — World가 Shutdown까지 Span을 쥔다).
    desc.subsystems = Span<const WorldSubsystemType>(services_->worldSubsystems.Data(),
                                                     services_->worldSubsystems.Size());
    desc.kind = map.kind;
    desc.outerSubsystems = SubsystemLookup();
    desc.beginPlay = false;
    auto created = engine_->CreateWorld(desc);
    if (!created) {
        LogOpenFailure(map.name, "CreateWorld", created.Error());
        return Result<void>::Failure(created.Error());
    }

    World* world = created.Value();
    auto fail = [&](const char* stage, ErrorCode error) noexcept {
        LogOpenFailure(map.name, stage, error);
        auto destroyed = engine_->DestroyWorld(world, EndPlayReason::Quit);
        (void)destroyed;
        return Result<void>::Failure(error);
    };

    // 권한(Client가 아니면)을 알린다. GameplayWorldSubsystem은 Preview·Inactive World에는 없다.
    auto* gameplay = world->GetSubsystem<GameplayWorldSubsystem>();
    if (gameplay != nullptr) {
        gameplay->hasAuthority_ = netMode_ != NetMode::Client;
    }

    // GameDirector는 배치 Actor보다 먼저다(21.2절 6단계).
    const char* stage = "Director";
    if (auto directed = SpawnDirector(*world, map, stage); !directed) {
        return fail(stage, directed.Error());
    }

    if (map.populate != nullptr) {
        // 이 동안 Spawn한 Pawn은 배치다(20.8절).
        if (gameplay != nullptr) {
            gameplay->populating_ = true;
        }

        auto populated = map.populate(*world, map.populateContext);
        if (gameplay != nullptr) {
            gameplay->populating_ = false;
        }

        if (!populated) {
            return fail("Populate", populated.Error());
        }
    }

    // 21.2절 8–9단계: LocalPlayer마다 Login(Director가 있을 때만).
    for (ObjectHandle player : localPlayers_.View()) {
        if (auto logged = LoginPlayer(*world, player); !logged) {
            return fail("Login", logged.Error());
        }
    }

    // 편집·비활성 World는 시작하지 않는다(16.4절).
    if (map.kind != WorldKind::Editor && map.kind != WorldKind::Inactive) {
        if (auto begun = world->BeginPlay(); !begun) {
            return fail("BeginPlay", begun.Error());
        }
    }

    currentWorld_ = world;
    Log::Info(LogGameplay, "Opened world {Map}", map.name);
    return Result<void>::Success();
}

Result<void> GameInstance::SpawnDirector(World& world, const MapDesc& map,
                                         const char*& stage) noexcept {
    // 19.3절: Game·Tryout World이고 Client가 아닐 때만 만든다(UE SetGameMode — 서버·standalone).
    if ((map.kind != WorldKind::Game && map.kind != WorldKind::Tryout) ||
        netMode_ == NetMode::Client) {
        return Result<void>::Success();
    }

    // 타입 우선순위: 맵 → 프로젝트 기본 → GameDirector, 마지막에 GameInstance override.
    stage = "Director";
    TypeId type = map.directorType != TypeId{}               ? map.directorType
                  : config_->defaultDirectorType != TypeId{} ? config_->defaultDirectorType
                                                             : ObjectTypeIdOf<GameDirector>();
    type = OverrideDirectorType(type);
    auto derived = engine_->Types().IsA(type, ObjectTypeIdOf<GameDirector>());
    if (!derived || !derived.Value()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto spawned = world.SpawnActorDeferred(type);
    if (!spawned) {
        return Result<void>::Failure(spawned.Error());
    }

    // options를 먼저 보관하고 FinishSpawning 전에 InitGame(UE InitGame — GameState는 아직 없다).
    stage = "InitGame";
    auto initialized =
        world.With<GameDirector>(spawned.Value(), [&](GameDirector& director) noexcept {
            if (auto copied = director.options_.TryAssign(map.options); !copied) {
                return copied;
            }

            return director.InitGame(map.name, director.Options());
        });
    if (!initialized) {
        return initialized;
    }

    stage = "FinishSpawning";
    return world.FinishSpawning(spawned.Value());
}

GameInstanceClient::GameInstanceClient(Allocator& allocator,
                                       const GameInstanceConfig& config) noexcept
    : config_(config), services_(allocator) {
}

GameInstanceClient::~GameInstanceClient() {
    if (instance_.IsValid()) {
        platform::Fatal("GameInstanceClient destroyed before the engine shut it down");
    }
}

Result<void> GameInstanceClient::OnEngineStart(Engine& engine) noexcept {
    engine_ = &engine;
    services_.engine = &engine;
    // OpenWorld가 만드는 World의 Subsystem 목록이다: 설정 목록 + GameplayWorldSubsystem(19.5절 —
    // 설정에 이미 있으면 더하지 않는다).
    services_.worldSubsystems.Clear();
    bool hasGameplay = false;
    for (const WorldSubsystemType& type : config_.worldSubsystems) {
        hasGameplay = hasGameplay || type.key == SubsystemKeyOf<GameplayWorldSubsystem>();
    }

    auto listed = services_.worldSubsystems.TryReserve(config_.worldSubsystems.Size() + 1);
    for (usize index = 0; listed && index < config_.worldSubsystems.Size(); ++index) {
        listed = services_.worldSubsystems.TryPushBack(config_.worldSubsystems[index]);
    }

    if (listed && !hasGameplay) {
        listed = services_.worldSubsystems.TryPushBack(GameplayWorldSubsystem::Type());
    }

    if (!listed) {
        engine_ = nullptr;
        return listed;
    }

    ObjectRegistry& objects = engine.Objects();
    const TypeId type = config_.gameInstanceType != TypeId{} ? config_.gameInstanceType
                                                             : ObjectTypeIdOf<GameInstance>();
    // 1) GameInstance 생성.
    auto created = objects.NewObject<GameInstance>(objects.OwnerAccess(), type);
    if (!created) {
        Log::Error(LogGameplay, "GameInstance could not be created: {Error}",
                   ErrorCodeName(created.Error()));
        engine_ = nullptr;
        return Result<void>::Failure(created.Error());
    }

    instance_ = Move(created.Value());
    // 실패한 단계까지 한 것을 역순으로 되돌린다(엔진 초기화 실패 경로라 OnEngineShutdown은 없다).
    auto started = WithInstance([&](GameInstance& game) noexcept -> Result<void> {
        game.engine_ = &engine;
        game.config_ = &config_;
        game.services_ = &services_;
        game.netMode_ = config_.netMode;
        // 2) OnInit(UE ReceiveInit) → 3) GameInstance Subsystem → 4) OnPostInit.
        if (auto init = game.OnInit(); !init) {
            Log::Error(LogGameplay, "GameInstance OnInit failed: {Error}",
                       ErrorCodeName(init.Error()));
            return init;
        }

        detail::GameInstanceServices& services = *game.services_;
        if (auto subsystems = services.subsystems.Initialize(config_.subsystems, game);
            !subsystems) {
            Log::Error(LogGameplay, "GameInstance subsystems failed to initialize: {Error}",
                       ErrorCodeName(subsystems.Error()));
            return subsystems;
        }

        services.visible = true;
        for (const SkippedSubsystem& skipped : services.subsystems.Skipped()) {
            if (skipped.reason == SubsystemSkipReason::MissingDependency) {
                Log::Warning(LogGameplay,
                             "Skipped game instance subsystem {Subsystem}: dependency "
                             "{Dependency} was not created",
                             skipped.name, skipped.missing);
            }
        }

        if (auto postInit = game.OnPostInit(); !postInit) {
            Log::Error(LogGameplay, "GameInstance OnPostInit failed: {Error}",
                       ErrorCodeName(postInit.Error()));
            services.visible = false;
            services.subsystems.Deinitialize(game);
            return postInit;
        }

        // 5) 첫 LocalPlayer(UE CreateInitialPlayer — 맵 전, 서버에는 없다).
        if (!game.IsDedicatedServer()) {
            if (auto player = game.CreateLocalPlayer(0); !player) {
                Log::Error(LogGameplay, "Initial local player could not be created: {Error}",
                           ErrorCodeName(player.Error()));
                services.visible = false;
                services.subsystems.Deinitialize(game);
                return Result<void>::Failure(player.Error());
            }
        }

        return Result<void>::Success();
    });
    if (!started) {
        instance_.Reset();
        engine_ = nullptr;
        return started;
    }

    Log::Info(LogGameplay, "GameInstance started ({Mode})", NetModeName(config_.netMode));
    return Result<void>::Success();
}

Result<void> GameInstanceClient::OnEngineReady(Engine&) noexcept {
    // 6) StartGameInstance: 기본 맵 → 7) OnStart(UE GameInstance.cpp:670–740).
    return WithInstance([this](GameInstance& game) noexcept -> Result<void> {
        if (config_.defaultMap.name.IsEmpty()) {
            Log::Error(LogGameplay, "GameInstance has no default map to open");
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto opened = game.OpenWorld(config_.defaultMap); !opened) {
            return opened;
        }

        game.OnStart();
        return Result<void>::Success();
    });
}

void GameInstanceClient::OnWorldDestroyed(Engine&, World& world) noexcept {
    if (!instance_.IsValid()) {
        return;
    }

    // 누가 DestroyWorld를 불렀든 현재 World를 비운다(남은 포인터 방지 — 18.6절).
    auto cleared = WithInstance([&world](GameInstance& game) noexcept {
        if (game.currentWorld_ == &world) {
            game.currentWorld_ = nullptr;
        }
    });
    (void)cleared;
}

void GameInstanceClient::OnEngineTick(Engine&) noexcept {
    auto ran = WithInstance([](GameInstance& game) noexcept {
        game.inEngineTick_ = true;
        auto opened = game.RunPendingOpen();
        game.inEngineTick_ = false;
        // 실패는 OpenWorld가 기록했다. 현재 World는 없음이다.
        (void)opened;
    });
    (void)ran;
}

void GameInstanceClient::OnEngineShutdown(Engine&) noexcept {
    if (!instance_.IsValid()) {
        return;
    }

    auto stopped = WithInstance([](GameInstance& game) noexcept {
        game.hasPendingOpen_ = false;
        // 1) 현재 World 끝(EndPlay(Quit) — 파괴는 Engine이 이 뒤에 한다) → 2) OnShutdown →
        // 3) LocalPlayer 역순 제거 → 4) GameInstance Subsystem 정리(UE GameEngine.cpp:1374–1381,
        // GameInstance.cpp:147–177).
        if (game.currentWorld_ != nullptr) {
            // 21.3절: Logout이 GameDirector가 살아 있을 때 돌도록 EndPlay(Quit) 전에 정리한다.
            game.CleanupPlayers(*game.currentWorld_);
            auto ended = game.currentWorld_->EndPlay(EndPlayReason::Quit);
            (void)ended;
            game.currentWorld_ = nullptr;
        }

        game.OnShutdown();
        game.RemoveAllLocalPlayers();
        // 정리 전에 조회 창에서 감춘다(Shutdown까지 창이 열린 Editor·Inactive World 대비).
        game.services_->visible = false;
        game.services_->subsystems.Deinitialize(game);
    });
    (void)stopped;
    // 5) 루트를 반환한다. 객체는 엔진 Registry 최종 GC가 회수한다.
    instance_.Reset();
    engine_ = nullptr;
    Log::Info(LogGameplay, "GameInstance shut down");
}

} // namespace rocks
