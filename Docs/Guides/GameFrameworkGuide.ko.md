# Rocks 게임 프레임워크 안내

이 안내는 게임 세션을 여는 방법과 게임 규칙·플레이어를 잇는 방법을 설명한다. 다루는 것은 GameInstance·GameDirector·Controller·Pawn이다. 설계 정본은 [엔진 런타임](../Design/EngineRuntime/README.ko.md) 18–22장이고, 이 안내는 쓰는 방법만 모은다. 컴포넌트·액터·레벨 스크립트에 동작을 넣는 방법은 [게임플레이 작업 안내](GameplayWorkflowGuide.ko.md)를 본다.

기준일: 2026-11-24. 모든 API는 지금 코드에 있다(`Engine/Source/Runtime/Gameplay`, `Engine/Source/Runtime/World`, `Engine/Source/Runtime/Engine`).

| 알고 싶은 것 | 읽을 절 |
|---|---|
| 무엇이 무엇을 소유하고 얼마나 사나 | 1 |
| 실행 파일에서 게임을 시작하는 법 | 2 |
| 맵 열기·전환 | 3 |
| 게임 규칙(GameDirector)·경기 상태(GameState)·플레이어 상태(PlayerState) | 4 |
| 조종(Controller)·몸(Pawn)·입력·카메라 | 5 |
| Subsystem 계층과 찾는 법 | 6 |
| 시작부터 종료까지 순서 | 7 |
| 진단 명령 | 8 |
| UE 이름 대응 | 9 |

## 1. 구조와 수명

```text
Engine ─ EngineSubsystem
  └ GameInstanceClient(EngineClient) ─ GameInstance Subsystem 묶음
       └ GameInstance(엔진 Registry 객체) ─ LocalPlayer들 ─ LocalPlayer Subsystem
       └ 현재 게임 World ─ GameplayWorldSubsystem
            ├ GameDirector ─ GameState ─ PlayerState 목록
            └ PlayerController ─ PlayerState, CameraDirector, Pawn
```

- GameInstance와 LocalPlayer는 엔진 Registry 객체다. Engine이 도는 동안 살고 World 전환을 넘어 유지된다. 반드시 빌림 범위에서만 쓴다: `GameInstanceClient::WithInstance`, `GameInstance::WithLocalPlayer`, hook·Subsystem 안.
- GameDirector·GameState·PlayerState·Controller·Pawn·CameraDirector는 World의 Actor다. World가 사라지면 함께 사라진다. 서로 handle로 잇고 `World::With`로 빌린다.
- Subsystem은 평범한 C++ 객체다. 묶음이 정리될 때까지 포인터가 유효하다.

## 2. 게임 시작하기

```cpp
// 기본 맵: PlayerStart 하나.
Result<void> PopulateArena(World& world, void*) noexcept {
    auto start = world.SpawnActor<PlayerStart>(SpawnParams::At(Transform::Identity()));
    return start ? Result<void>::Success() : Result<void>::Failure(start.Error());
}

GameInstanceConfig game;
game.defaultDirectorType = ObjectTypeIdOf<ArenaDirector>();   // 비면 GameDirector
game.subsystems = gameSubsystems;                              // GameInstance Subsystem
game.localPlayerSubsystems = playerSubsystems;
game.worldSubsystems = worldSubsystems;                        // OpenWorld가 만드는 World
game.timeStep = {1.0 / 60.0, 0.25, 8};
game.defaultMap.name = "Arena";
game.defaultMap.populate = &PopulateArena;

GameInstanceClient client(allocator, game);   // allocator·game은 client보다 오래 산다
EngineConfig config;
config.client = &client;
config.registerTypes = &RegisterMyGameTypes;  // ArenaDirector 등 native 파생
engine.Initialize(config);                    // 여기서 Arena가 BeginPlay까지 열린다
engine.Run();
engine.Shutdown();
```

- `Initialize` 안에서 순서는 이렇다: GameInstance 생성 → `OnInit` → GameInstance Subsystem → `OnPostInit` → 첫 LocalPlayer(입력 장치 0). Engine이 Running이 되면 `OnEngineReady`에서 기본 맵을 연 뒤 `OnStart`를 부른다.
- 기본 맵을 열지 못하면 `Initialize`가 실패하고 Engine이 정상 종료 순서로 정리한다.
- 전용 서버는 `game.netMode = NetMode::DedicatedServer`로 둔다. 이때 LocalPlayer·Login이 없다.
- 실제 예는 Sandbox다(`Programs/Sandbox/Private/SandboxGame.*`). `Sandbox --engine`은 헤드리스, `Sandbox --engine --window`는 창을 열고 키보드·마우스·게임패드 입력이 Pawn 로그(`Sandbox.Game`)로 찍힌다. 호스트가 입력을 거는 법은 `Programs/Sandbox/Private/EngineHost.cpp`의 `RunEngine`(`SetLocalPlayerInput`)을 본다.

GameInstance를 파생하면 hook을 쓸 수 있다.

```cpp
class MyGame final : public GameInstance {
    ROCKS_NATIVE_TYPE(MyGame, GameInstance, 0x..., 0x...)
public:
    explicit MyGame(Allocator& allocator) noexcept : GameInstance(allocator) {}
protected:
    Result<void> OnPostInit() noexcept override;   // Subsystem이 이미 있다
    void OnStart() noexcept override;              // 기본 맵이 열린 뒤
    void OnShutdown() noexcept override;           // 현재 World EndPlay 뒤
    TypeId OverrideDirectorType(TypeId type) noexcept override;  // 마지막으로 Director 타입 바꾸기
};
// game.gameInstanceType = ObjectTypeIdOf<MyGame>();
```

## 3. 맵 열기·전환

```cpp
MapDesc next;
next.name = "Dungeon";
next.options = "?start=North?rounds=3";        // GameDirector::Options·FindMapOption
next.directorType = ObjectTypeIdOf<DungeonDirector>();   // 비면 프로젝트 기본
next.populate = &PopulateDungeon;
client.WithInstance([&](GameInstance& game) noexcept { return game.OpenWorld(next); });
```

- 프레임 안(World Tick·loop hook)에서 부르면 다음 `OnEngineTick`에 연다. 마지막 요청만 남는다. 이름과 options는 복사해 둔다.
- 현재 World를 정리하는 순서는 이렇다: Spawn한 Pawn → PlayerController(이때 `Logout`) → `EndPlay(LevelTransition)` → World 즉시 파괴. 그다음 새 World에서 GameDirector → `populate` → Login → BeginPlay 순으로 연다.
- 맵에 배치한 Pawn(`populate` 중 Spawn)은 전환 정리에서 먼저 지우지 않는다. `EndPlay`의 일반 정리에서 지워진다.
- LocalPlayer와 GameInstance Subsystem은 전환을 넘어 그대로다.
- 게임 코드가 `Engine::DestroyWorld`로 현재 World를 지워도 GameInstance가 알아서 비운다(`CurrentWorld()`가 null).

## 4. 규칙과 상태

### 4.1 GameDirector(UE GameMode)

```cpp
class ArenaDirector final : public GameDirector {
    ROCKS_NATIVE_TYPE(ArenaDirector, GameDirector, 0x..., 0x...)
public:
    explicit ArenaDirector(Allocator& allocator) noexcept : GameDirector(allocator) {
        gameStateType = ObjectTypeIdOf<ArenaState>();
        playerControllerType = ObjectTypeIdOf<ArenaPlayerController>();
        defaultPawnType = ObjectTypeIdOf<Runner>();
    }

    Result<void> InitGame(StringView map, StringView options) noexcept override {
        StringView rounds;
        if (FindMapOption(options, "rounds", rounds)) { /* ... */ }
        return Result<void>::Success();
    }

    void PostLogin(ObjectHandle controller) noexcept override {
        // 환영 메시지 등. 기본 흐름(Pawn 주기)은 그대로 부른다.
        GameDirector::PostLogin(controller);
    }
};
```

- 다음 상황에서만 생긴다: World 종류가 Game·Tryout이고, NetMode가 Client가 아닐 때. 타입 우선순위는 맵 → `defaultDirectorType` → GameDirector 순이고, 마지막에 GameInstance의 `OverrideDirectorType`을 거친다. GameDirector 파생이 아니면 OpenWorld가 실패한다(진단 `InvalidDirectorType`).
- 재정의할 수 있는 단계: `InitGame` · `Login` · `InitNewPlayer` · `PostLogin` · `HandleStartingNewPlayer` · `RestartPlayer` · `SpawnDefaultPawnFor` · `FinishRestartPlayer` · `Logout` · `StartPlay` · `FindPlayerStart`. 재정의한 함수는 기본 흐름을 `GameDirector::이름(...)`으로 부른다. 특히 `StartPlay`를 재정의하고 기본을 부르지 않으면 BeginPlay가 실패한다(`NotReady`).
- 시작 위치 선택(`FindPlayerStart`): 먼저 이름표(`PlayerStart::SetPlayerStartTag`)가 맞는 것, 없으면 아무 PlayerStart 중 먼저 Spawn한 것이다. 이름이 비면 options `?start=`를 쓴다. 무작위가 필요하면 재정의한다(UE와 다른 점 — 재현성).

### 4.2 GameState·PlayerState

- GameState는 Director가 만든다. `PlayerStates()`(추가 순서), `HasBegunPlay()`, `HasMatchStarted()`를 준다.
- PlayerState는 PlayerController가 만든다. AiController는 `wantsPlayerState`가 켜졌을 때만 만든다. `PlayerId()`는 1부터이고 `PlayerName()`·`IsSpectator()`·`IsBot()`·`PawnHandle()`이 있다. Pawn이 죽거나 바뀌어도 남고, Controller가 파괴될 때 함께 없어진다.
- World의 GameState는 `GetWorld().GetSubsystem<GameplayWorldSubsystem>()->GameStateHandle()`로 찾는다.

## 5. 조종·몸·입력·카메라

### 5.1 Controller와 Pawn

```cpp
world.With<Controller>(controller, [pawn](Controller& value) noexcept {
    return value.Possess(pawn);   // 기존 연결을 풀고 PossessedBy → Tick 간선 → 회전 → Restart
});
```

- Pawn hook: `OnPossessed(controller)`, `OnUnpossessed()`, `OnControllerChanged(old, new)`, `OnRestart()`. Pawn이 바라보는 방향은 `GetControlRotation()`(Controller의 controlRotation)이다.
- Controller hook: `OnPossessedPawnChanged(old, new)`. `OnPossess`·`OnUnPossess`를 재정의하면 기본을 부른다.
- Pawn이 파괴되면 Controller는 Inactive가 되고 PlayerState는 남는다. 다시 태우려면 `RestartPlayer`를 부른다. PlayerState가 없는 AI Controller는 Pawn과 함께 사라진다.
- Possess하면 Pawn `OnTick`이 Controller `OnTick` 뒤로 정렬된다(같은 단계). 둘 다 Tick을 켰을 때만이다.
- AI 자동 Possess는 `autoPossessAi`로 정한다(기본 `PlacedInWorld` — UE와 같다). 값은 `Disabled`·`PlacedInWorld`·`Spawned`·`PlacedInWorldOrSpawned`이다. Game·Tryout World의 권한 있는 쪽에서만 한다. 타입은 Pawn의 `aiControllerType` → Director의 `aiControllerType` → AiController 순이다.
- Client World에서 Possess하면 `InvalidArgument`다(진단 `PossessDeniedNoAuthority`).

### 5.2 PlayerController와 입력

```cpp
class ArenaPlayerController final : public PlayerController {
    ROCKS_NATIVE_TYPE(ArenaPlayerController, PlayerController, 0x..., 0x...)
public:
    explicit ArenaPlayerController(Allocator& allocator) noexcept : PlayerController(allocator) {}
protected:
    void OnPlayerTick(const TickContext&) noexcept override {   // pause 중에는 불리지 않는다
        if (PlayerInput().WasPressed(input::Key::Space)) { /* 점프 요청 */ }
    }
    void OnControllerTick(const TickContext& context) noexcept override {   // pause 중에도
        if (context.paused) { /* pause 해제 입력 */ }
    }
};
```

- 호스트(창)는 프레임 입력 상태를 이렇게 넘긴다: `game.SetLocalPlayerInput(player, &inputState)`. 이 호출은 LocalPlayer의 입력 원천과 현재 PlayerController를 함께 바꾼다.
- PlayerController는 Tick마다 입력을 스냅숏으로 복사한다. Pawn은 Controller의 `PlayerInput()`을 읽는다.
- 게임 중 `CreateLocalPlayer(1)`을 부르면 곧바로 Login한다(분할 화면용 둘째 플레이어). `RemoveLocalPlayer`는 그 PlayerController와 Pawn을 지운다.

### 5.3 CameraDirector

- PlayerController가 만들고, LateUpdate 시작(엔진 단계 `CameraUpdate`)에서 갱신한다. pause 중에는 갱신하지 않고 단일 스텝 프레임에는 갱신한다.
- 결과는 `View()`(`CameraView{location, rotation, fov = 90}`)다. 기본 view target은 PlayerController이고, Possess하면 Pawn으로 바뀐다. 위치는 대상 root의 월드 위치, 회전은 controlRotation이다.
- 결과를 바꾸려면 `OnUpdateView(CameraView&)`를 재정의한다(CameraDirector 파생).
- view target Actor에 켜진 `CameraComponent`(World 모듈)가 있으면 Component 순서로 첫 것의 `GetCameraView`를 쓴다(UE `CalcCamera`). 위치·회전은 그 Component의 월드 Transform이고, 시야·투영·직교 폭·종횡비는 그 필드다. `SetUsePawnControlRotation(true)`면 회전은 Pawn의 controlRotation이다(1인칭).

```cpp
// Pawn OnAwake: 눈높이 카메라
auto camera = GetWorld().AddComponent<CameraComponent>(Handle());
GetWorld().With<CameraComponent>(camera.Value(), [&](CameraComponent& value) noexcept {
    value.SetUsePawnControlRotation(true);
    auto attached = value.AttachTo(RootComponent());
    auto eye = value.SetRelativeLocation(Vec3d(0.0, 0.0, 64.0));
    return attached && eye ? value.SetFieldOfView(100.0) : Result<void>::Failure(ErrorCode::InvalidArgument);
});
```

## 6. Subsystem 계층

| 계층 | 기반 | 만드는 때 | 찾기 |
|---|---|---|---|
| Engine | `EngineSubsystem` | Engine 초기화 | `engine.GetSubsystem<T>()`, World 안 `GetEngineSubsystem<T>(world)` |
| GameInstance | `GameInstanceSubsystem` | `OnInit` 뒤 | `game.GetSubsystem<T>()`, World 안 `GetGameInstanceSubsystem<T>(world)` |
| LocalPlayer | `LocalPlayerSubsystem` | LocalPlayer 추가 | `player.GetSubsystem<T>()` |
| World | `WorldSubsystem` | World 초기화 | `world.GetSubsystem<T>()` |

- 의존은 타입 정의에 선언한다(`MakeSubsystemType<Base, T>("이름", 필수 의존, 선택 의존, shouldCreate)`). 정리는 초기화의 역순이다.
- 필수 의존이 없으면 그 Subsystem은 만들지 않는다(진단 `SubsystemSkippedMissingDependency`).
- World 안에서 바깥 계층을 찾는 창은 World `EndPlay`가 끝나면 닫힌다. 그래서 `PreDeinitialize`·`Deinitialize` 안에서는 null이다. 바깥 Subsystem이 필요한 정리는 `OnWorldEndPlay`에서 끝낸다.

## 7. 시작부터 종료까지

1. Engine 초기화 → EngineSubsystem
2. GameInstance 생성 → `OnInit` → GameInstance Subsystem → `OnPostInit`
3. 첫 LocalPlayer → LocalPlayer Subsystem
4. Running → 기본 맵 OpenWorld
5. World 생성 → WorldSubsystem `Initialize` → `PostInitialize`
6. GameDirector Spawn → `InitGame` → GameState Spawn → Director `OnAwake`
7. `populate`(배치 Actor, 배치 Pawn의 AI 자동 Possess)
8. LocalPlayer마다 Login(PlayerController → PlayerState·CameraDirector)
9. `PostLogin` → `RestartPlayer` → 시작 위치에 Pawn → Possess
10. BeginPlay: `OnWorldBeginPlay` → `StartPlay` → `HandleBeginPlay` → 모든 Actor `OnStart`
11. 프레임: PlayerController `OnPlayerTick` → Pawn Tick → … → LateUpdate 시작(LifeSpan·tickable Subsystem·카메라)
12. Pawn 파괴 → Controller Inactive → `RestartPlayer`로 새 Pawn
13. 전환: Spawn한 Pawn → PlayerController(`Logout`) → `EndPlay(LevelTransition)` → World 파괴 → 새 World 5–10
14. 종료: 같은 정리(`Logout`) → `EndPlay(Quit)` → `OnShutdown` → LocalPlayer 역순 → GameInstance Subsystem → World 파괴 → EngineSubsystem

정본과 시험은 [엔진 런타임 21장](../Design/EngineRuntime/21-InitFlow.ko.md)과 `GameplayFlow_FourteenStepScenario`다.

GameInstance 없이 World만 쓰는 도구·시험도 있다(`Engine::CreateWorld`). 이 경우 GameplayWorldSubsystem·Director·Login이 없고, `BeginPlay`가 바로 Actor를 시작한다.

## 8. 진단

| 콘솔 명령 | 출력 |
|---|---|
| `engine.dumpsubsystems` | Engine → GameInstance → LocalPlayer → World마다 초기화 순서·생략 이유 |
| `world.dumpticks [World 이름]` | 단계마다 실행 순서대로 Tick 항목(hook·priority·interval·늦춤), 선행, 엔진 단계 자리 |
| `game.dumpframework` | GameInstance → LocalPlayer → PlayerController(state) → Pawn·PlayerState·CameraDirector, GameDirector(수 통계)·GameState |

- 명령은 설정 문맥(`EngineConfig::settings`)이 있을 때 등록된다. 출력은 콘솔 sink와 Info 로그에 같은 내용으로 남는다. 한 설정 문맥을 Engine 여럿이 쓰면 마지막에 시작한 Engine이 대상이다.
- 같은 내용은 코드에서도 얻을 수 있다: `Engine::DumpSubsystems`, `Engine::DumpTickGraphs`, `World::DumpTickGraph`, `GameInstanceClient::DumpFramework`.
- 통계는 `World::Stats()`와 `GameplayWorldSubsystem::Stats(world)`에 있다. `World::Stats()`에는 늦춘 Tick 수·엔진 단계 시간·LifeSpan 대기 수가, `GameplayWorldSubsystem::Stats(world)`에는 Controller·Pawn·PlayerState 수가 있다.
- 로그의 `{Code}` 속성으로 진단을 찾는다: `SubsystemSkippedMissingDependency` · `AutoTickPrerequisiteCycleSkipped` · `BeginPlayDispatchContractViolated` · `InvalidDirectorType` · `PossessDeniedNoAuthority`.

## 9. UE 이름 대응

| Rocks | UE 5.8 |
|---|---|
| GameInstance·GameInstanceClient | UGameInstance(+ UGameEngine의 시작·종료 부분) |
| LocalPlayer | ULocalPlayer |
| GameDirector | AGameModeBase |
| GameState·PlayerState | AGameStateBase·APlayerState |
| Controller·PlayerController·AiController | AController·APlayerController·AAIController |
| Pawn | APawn |
| CameraDirector | APlayerCameraManager |
| OpenWorld·MapDesc | LoadMap·FURL |
| OnInit·OnPostInit·OnStart·OnShutdown | ReceiveInit·Init 뒤·OnStart·Shutdown |
| OnAwake·OnStart·OnDestroy | PostInitializeComponents 무렵·BeginPlay·EndPlay |
