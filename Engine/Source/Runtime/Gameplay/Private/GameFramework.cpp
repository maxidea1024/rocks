#include <Core/Diagnostics/Log.h>
#include <Gameplay/GameDirector.generated.h>
#include <Gameplay/GameDirector.h>
#include <Gameplay/GameState.generated.h>
#include <Gameplay/GameState.h>
#include <Gameplay/GameplayWorldSubsystem.h>
#include <World/Actor.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/SceneComponent.h>

// GameplayWorldSubsystem·GameDirector·GameState·PlayerState·PlayerStart다(Docs/Design/EngineRuntime
// 19절, S11).
ROCKS_DEFINE_NATIVE_TYPE_KEY(rocks::GameplayWorldSubsystem)

namespace rocks {

namespace {

// 초기화를 마쳤고 파괴 대기가 아닌 Actor인지다(프레임워크 목록·선택 대상). 지연 Spawn
// 중(Constructed)은 뺀다 — 아직 프레임워크 hook을 받지 않아 FinishSpawning 전에 파괴되면 정리
// hook도 없다(그 Actor는 자기 PostAwake에서 스스로 들어온다).
bool IsLiveActor(World& world, ObjectHandle actor) noexcept {
    auto state = world.GetActorState(actor);
    return state &&
           (state.Value() == ActorState::Initialized || state.Value() == ActorState::Playing);
}

// Spawn 순번이다(레코드가 없으면 가장 큰 값 — 고르지 않는다).
uint64 SpawnOrderOf(World& world, ObjectHandle actor) noexcept {
    auto order = world.GetSpawnOrder(actor);
    return order ? order.Value() : NumericLimits<uint64>::Max();
}

} // namespace

// --- GameplayWorldSubsystem ---

Result<void> GameplayWorldSubsystem::Initialize(World& world) noexcept {
    // World BeginPlay의 시작을 맡는다(19.5절). Initialize는 BeginPlay 전이다.
    return world.SetBeginPlayDispatcher(this);
}

Result<void> GameplayWorldSubsystem::StartPlay(World& world) noexcept {
    // Director가 없으면(Client·편집·아직 없음) 바로 시작한다(21.4절).
    if (director_.IsNull()) {
        return world.DispatchBeginPlay();
    }

    return world.With<GameDirector>(
        director_, [](GameDirector& director) noexcept { return director.StartPlay(); });
}

// --- PlayerStart ---

PlayerStart::PlayerStart(Allocator& allocator) noexcept : Actor(allocator), tag_(allocator) {
}

PlayerStart::~PlayerStart() noexcept = default;

Result<void> PlayerStart::OnFrameworkPreAwake() noexcept {
    // 위치 표지다: root SceneComponent 하나(Spawn Transform이 FinishSpawning에서 붙는다 — 17.3절).
    auto root = GetWorld().AddComponent<SceneComponent>(Handle());
    if (!root) {
        return Result<void>::Failure(root.Error());
    }

    return SetRootComponent(root.Value());
}

// --- GameDirector ---

GameDirector::GameDirector(Allocator& allocator) noexcept : Actor(allocator), options_(allocator) {
}

GameDirector::~GameDirector() noexcept = default;

Result<void> GameDirector::InitGame(StringView, StringView) noexcept {
    return Result<void>::Success();
}

Result<void> GameDirector::StartPlay() noexcept {
    // UE StartPlay → GameState HandleBeginPlay(GameModeBase.cpp:204–207).
    if (gameState_.IsNull()) {
        return GetWorld().DispatchBeginPlay();
    }

    return GetWorld().With<GameState>(
        gameState_, [](GameState& state) noexcept { return state.HandleBeginPlay(); });
}

ObjectHandle GameDirector::FindPlayerStart(ObjectHandle, StringView incomingName) noexcept {
    StringView wanted = incomingName;
    if (wanted.IsEmpty()) {
        StringView option;
        if (FindMapOption(Options(), "start", option)) {
            wanted = option;
        }
    }

    // 이름 일치 중 Spawn 순번 최소 → PlayerStart 중 Spawn 순번 최소 → null(19.2절). Level 배열이나
    // 레코드 순회 순서에 기대지 않는다.
    World& world = GetWorld();
    ObjectHandle named;
    ObjectHandle first;
    uint64 namedOrder = NumericLimits<uint64>::Max();
    uint64 firstOrder = NumericLimits<uint64>::Max();
    world.ForEachActor([&](ObjectHandle actor) noexcept {
        if (!IsLiveActor(world, actor)) {
            return;
        }

        auto matched = world.With<PlayerStart>(actor, [wanted](PlayerStart& start) noexcept {
            return !wanted.IsEmpty() && start.PlayerStartTag() == wanted;
        });
        if (!matched) {
            return;
        }

        const uint64 order = SpawnOrderOf(world, actor);
        if (order < firstOrder) {
            firstOrder = order;
            first = actor;
        }

        if (matched.Value() && order < namedOrder) {
            namedOrder = order;
            named = actor;
        }
    });
    return !named.IsNull() ? named : first;
}

Result<void> GameDirector::OnFrameworkPreAwake() noexcept {
    // GameState를 만들고 InitGameState(UE PreInitializeComponents — GameModeBase.cpp:116–144).
    World& world = GetWorld();
    const TypeId type = gameStateType != TypeId{} ? gameStateType : ObjectTypeIdOf<GameState>();
    const TypeId directorType = GetType().Id();
    auto spawned = world.SpawnActorDeferred(type);
    if (!spawned) {
        // Actor가 아니거나 등록되지 않은 타입이다(19.7절 — InvalidArgument).
        Log::Error(LogGameplay, "GameDirector gameStateType could not be spawned: {Error}",
                   ErrorCodeName(spawned.Error()));
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // World는 TypeRegistry를 내보이지 않으므로 빌림으로 파생을 판정한다(19.7절).
    auto initialized =
        world.With<GameState>(spawned.Value(), [directorType](GameState& state) noexcept {
            state.directorType_ = directorType;
        });
    if (!initialized) {
        auto destroyed = world.DestroyActor(spawned.Value());
        (void)destroyed;
        Log::Error(LogGameplay, "GameDirector gameStateType is not a GameState");
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto finished = world.FinishSpawning(spawned.Value()); !finished) {
        return finished;
    }

    gameState_ = spawned.Value();
    return Result<void>::Success();
}

Result<void> GameDirector::OnFrameworkPostAwake() noexcept {
    if (auto* gameplay = GetWorld().GetSubsystem<GameplayWorldSubsystem>()) {
        gameplay->director_ = Handle();
    }

    postAwake_ = true;
    return Result<void>::Success();
}

void GameDirector::OnFrameworkEndPlay(EndPlayReason) noexcept {
    World& world = GetWorld();
    auto* gameplay = world.GetSubsystem<GameplayWorldSubsystem>();
    if (gameplay != nullptr && gameplay->director_ == Handle()) {
        gameplay->director_ = {};
    }

    // PreAwake가 만든 GameState를 Spawn 실패(PostAwake 전 실패)에서 되돌린다(17.8절 — 이 자리가
    // 있는 이유). 정상 정리에서는 GameState가 자기 수명대로 남는다(UE도 GameMode 파괴가 GameState를
    // 지우지 않는다).
    if (!postAwake_ && !gameState_.IsNull()) {
        auto destroyed = world.DestroyActor(gameState_);
        (void)destroyed;
        gameState_ = {};
    }
}

// --- GameState ---

GameState::GameState(Allocator& allocator) noexcept : Actor(allocator), playerStates_(allocator) {
}

GameState::~GameState() noexcept = default;

Result<void> GameState::AddPlayerState(ObjectHandle playerState) noexcept {
    for (ObjectHandle existing : playerStates_) {
        if (existing == playerState) {
            return Result<void>::Success();
        }
    }

    return playerStates_.TryPushBack(playerState);
}

void GameState::RemovePlayerState(ObjectHandle playerState) noexcept {
    for (usize index = 0; index < playerStates_.Size(); ++index) {
        if (playerStates_[index] == playerState) {
            auto removed = playerStates_.TryRemoveAt(index);
            (void)removed;
            return;
        }
    }
}

Result<void> GameState::HandleBeginPlay() noexcept {
    // 이미 시작했으면 다시 시작하지 않는다(DispatchBeginPlay도 WrongContext다 — 상태 불변).
    if (hasBegunPlay_) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    // UE HandleBeginPlay(GameStateBase.cpp:205–211): bReplicatedHasBegunPlay → NotifyBeginPlay
    // (Actor DispatchBeginPlay) → NotifyMatchStarted.
    hasBegunPlay_ = true;
    if (auto dispatched = GetWorld().DispatchBeginPlay(); !dispatched) {
        hasBegunPlay_ = false;
        return dispatched;
    }

    matchStarted_ = true;
    return Result<void>::Success();
}

Result<void> GameState::OnFrameworkPostAwake() noexcept {
    World& world = GetWorld();
    if (auto* gameplay = world.GetSubsystem<GameplayWorldSubsystem>()) {
        gameplay->gameState_ = Handle();
    }

    // 이미 있는 PlayerState를 Spawn 순번으로 모은다(UE GameStateBase.cpp:54–75 — UE는 순회 순서).
    // 새 GameState라 목록은 비어 있다.
    Result<void> collected = Result<void>::Success();
    world.ForEachActor([&](ObjectHandle actor) noexcept {
        if (!collected || !IsLiveActor(world, actor) ||
            !world.With<PlayerState>(actor, [](PlayerState&) noexcept {})) {
            return;
        }

        collected = AddPlayerState(actor);
    });
    if (!collected) {
        return collected;
    }

    // 작은 목록이라 삽입 정렬이다.
    for (usize index = 1; index < playerStates_.Size(); ++index) {
        const ObjectHandle current = playerStates_[index];
        const uint64 order = SpawnOrderOf(world, current);
        usize at = index;
        while (at > 0 && SpawnOrderOf(world, playerStates_[at - 1]) > order) {
            playerStates_[at] = playerStates_[at - 1];
            --at;
        }

        playerStates_[at] = current;
    }

    return Result<void>::Success();
}

void GameState::OnFrameworkEndPlay(EndPlayReason) noexcept {
    World& world = GetWorld();
    auto* gameplay = world.GetSubsystem<GameplayWorldSubsystem>();
    if (gameplay == nullptr) {
        return;
    }

    if (gameplay->gameState_ == Handle()) {
        gameplay->gameState_ = {};
    }

    // Director의 handle도 비운다(GameStateHandle "없으면 null" 계약).
    const ObjectHandle self = Handle();
    if (!gameplay->director_.IsNull()) {
        auto cleared =
            world.With<GameDirector>(gameplay->director_, [self](GameDirector& director) noexcept {
                if (director.gameState_ == self) {
                    director.gameState_ = {};
                }
            });
        (void)cleared;
    }
}

// --- PlayerState ---

PlayerState::PlayerState(Allocator& allocator) noexcept : Actor(allocator), playerName_(allocator) {
}

PlayerState::~PlayerState() noexcept = default;

Result<void> PlayerState::OnFrameworkPostAwake() noexcept {
    World& world = GetWorld();
    auto* gameplay = world.GetSubsystem<GameplayWorldSubsystem>();
    if (gameplay == nullptr) {
        return Result<void>::Success();
    }

    // Director가 번호를 매기고(1부터) GameState 목록에 들어간다(UE PlayerState.cpp:139–150).
    if (!gameplay->Director().IsNull()) {
        auto id =
            world.With<GameDirector>(gameplay->Director(), [](GameDirector& director) noexcept {
                return director.AllocatePlayerId();
            });
        if (id) {
            playerId_ = id.Value();
        }
    }

    if (gameplay->GameStateHandle().IsNull()) {
        return Result<void>::Success();
    }

    const ObjectHandle self = Handle();
    return world.With<GameState>(gameplay->GameStateHandle(), [self](GameState& state) noexcept {
        return state.AddPlayerState(self);
    });
}

void PlayerState::OnFrameworkEndPlay(EndPlayReason) noexcept {
    World& world = GetWorld();
    auto* gameplay = world.GetSubsystem<GameplayWorldSubsystem>();
    if (gameplay == nullptr || gameplay->GameStateHandle().IsNull()) {
        return;
    }

    const ObjectHandle self = Handle();
    auto removed =
        world.With<GameState>(gameplay->GameStateHandle(),
                              [self](GameState& state) noexcept { state.RemovePlayerState(self); });
    (void)removed;
}

} // namespace rocks
