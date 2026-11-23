# 엔진 런타임 — 18. GameInstance·LocalPlayer(S10)

[목차](README.ko.md)

## 18. GameInstance·LocalPlayer(S10)

설계일 2026-11-23. UE 경로 약어: `E/` = `Engine/Source/Runtime/Engine/`. 이 절부터 21절까지의 타입은 새 모듈 `Gameplay`(`Engine/Source/Runtime/Gameplay`, 의존 Engine·World·Input — 3절 표)에 둔다.

### 18.1 UE 5.8 계약

- `UGameEngine::Init`이 GameInstance 클래스(프로젝트 설정, 없으면 기본)를 만들고 `InitializeStandalone`(Game WorldContext·dummy World) → `Init`을 부른다. `Init`은 BP hook(`ReceiveInit`)을 먼저, GameInstance Subsystem 초기화를 **마지막**에 한다. C++ 파생은 보통 `Super::Init()` 뒤에 자기 코드를 둬 Subsystem을 쓴다(`E/Private/GameEngine.cpp:1256–1267`, `E/Private/GameInstance.cpp:100–129, 190–202`).
- 첫 LocalPlayer는 맵 전에 만들고(`CreateInitialPlayer` → `CreateLocalPlayer(bSpawnPlayerController=false)`), `PlayerAdded`에서 LocalPlayer Subsystem을 초기화한다(`GameInstance.cpp:832–835, 896–913, 948–971`, `E/Private/LocalPlayer.cpp:262, 270`). dedicated server에는 LocalPlayer가 없다(`GameInstance.cpp:856–859`).
- `UGameEngine::Start` → `StartGameInstance` → 기본 맵 `Browse` → `LoadMap`, 성공하면 `OnStart`(`GameEngine.cpp:1333–1338`, `GameInstance.cpp:670–740`). `Start`는 엔진 `Init`과 PostEngineInit 모듈 시작이 끝난 **뒤**다.
- `LoadMap`은 이전 World를 정리(Pawn → PlayerController 파괴, `EndPlay(LevelTransition)`, `CleanupWorld`)한 뒤 새 World를 만들고 게임 흐름(21절)을 실행한다(`E/Private/UnrealEngine.cpp:16108–16665`).
- 종료(`UGameEngine::PreExit`): World마다 `EndPlay(Quit)` → `GameInstance::Shutdown`(LocalPlayer 역순 제거 → GameInstance Subsystem 정리) → `CleanupWorld`(`GameEngine.cpp:1352–1394`, `GameInstance.cpp:147–177`).

### 18.2 GameInstance

- 엔진 Registry(4.1절)의 리플렉션 클래스 `GameInstance : Object`다. 게임은 파생 클래스를 만들고, 실행 파일이 `EngineConfig::client`에 `GameInstanceClient`(Gameplay 모듈의 `EngineClient` 구현, 6절)를 넘기며 GameInstance 타입을 지정한다.
- 소유: LocalPlayer 목록(`RootRef`), GameInstance Subsystem 묶음, 현재 게임 World의 Engine World 식별자, 기본 맵 설정(18.5절).
- hook(사용자 파생이 재정의): `OnInit()`(GameInstance Subsystem 전 — UE `ReceiveInit` 자리), `OnPostInit()`(GameInstance Subsystem 뒤 — C++ 파생이 `Super::Init()` 뒤에 두던 코드 자리), `OnStart()`, `OnShutdown()`. 엔진 쪽 순서는 아래로 고정한다.
- `EngineClient`에 hook `OnEngineReady()`를 더한다(Engine 변경, S10). Engine이 Running이 되고 PostEngineInit 모듈이 시작된 **뒤** 한 번 부른다. `OnEngineStart`는 Engine이 Initializing일 때 불리고(`Engine.cpp:293–325`) `CreateWorld`는 Running에서만 되므로(`Engine.cpp:386–389`), 맵 열기는 `OnEngineReady`로 간다(UE `UGameEngine::Start`와 같은 자리).

| `EngineClient` 호출 | GameInstance가 하는 일(순서대로) |
|---|---|
| `OnEngineStart`(Initializing) | ① GameInstance 생성 → ② `OnInit` → ③ GameInstance Subsystem 초기화 → ④ `OnPostInit` → ⑤ 첫 LocalPlayer 생성(서버 실행이 아니면 — 18.4절) |
| `OnEngineReady`(Running) | ⑥ `StartGameInstance`: 기본 맵으로 `OpenWorld`(18.5절) → ⑦ 성공하면 `OnStart` |
| `OnEngineTick` | 미룬 `OpenWorld` 요청 처리(프레임 사이에서만 World를 바꾼다) |
| `OnEngineShutdown` | ① 게임 World가 있으면 PlayerController 정리(21.3절 — Logout 포함)와 `EndPlay(Quit)` → ② `OnShutdown` → ③ LocalPlayer 역순 제거(LocalPlayer Subsystem 정리) → ④ GameInstance Subsystem 정리 → ⑤ GameInstance 루트 반환. World 저장소 파괴는 그 뒤 Engine이 한다(6절 순서와 같다) |

- `OnEngineStart`의 한 단계가 실패하면 앞 단계를 역순 정리하고 실패를 돌려준다(Engine 초기화 실패 경로, 6절).
- `OnEngineReady`가 실패하면(기본 맵 열기 실패 등) Engine은 `Initialize`의 실패로 돌려주고 정상 종료 순서(`OnEngineShutdown`부터)로 정리한다. 반쯤 연 World는 `OpenWorld`가 이미 정리했다(18.5절).
- UE와 다른 점: UE 종료는 `EndPlay(Quit)` 뒤 `RemoveLocalPlayer`가 PlayerController를 파괴한다(`GameEngine.cpp:1374–1381`, `GameInstance.cpp:999`). Rocks는 GameDirector가 살아 있을 때 `Logout`이 돌도록 PlayerController를 `EndPlay(Quit)` **전**에 정리한다(문서화된 차이).

### 18.3 서버·클라이언트 구분

GameInstance는 실행 모드를 `NetMode`로 안다: `Standalone`, `DedicatedServer`, `ListenServer`, `Client`(UE `ENetMode`). 이 단계에서 네트워크는 없으므로 `Standalone`과 `DedicatedServer`(LocalPlayer 없음, 렌더·입력 hook 없음 — [전용 서버](../BuildTargets/03-Design/03.07-DedicatedServer.ko.md) 3.7절)만 실행 경로가 있다. `Client`는 GameDirector를 만들지 않는 경로(19.3절)로만 쓰이며 복제는 네트워크 단계(계획 17절)가 붙인다.

### 18.4 LocalPlayer

- 엔진 Registry의 리플렉션 클래스 `LocalPlayer : Object`. GameInstance가 소유하고 World를 넘어 산다.
- 가진 것: 로컬 번호(0부터), 입력 장치 번호(`controllerId`), 입력 원천(`InputSource` — 실행 파일이 넘기는 프레임 입력 상태 `InputState`의 읽기 창, 9절), 현재 World의 PlayerController handle, LocalPlayer Subsystem 묶음.
- `GameInstance::CreateLocalPlayer(controllerId, spawnPlayerController)`: 생성 → 목록 추가 → LocalPlayer Subsystem 초기화(UE `PlayerAdded`) → 게임 World가 Playing이고 `spawnPlayerController`면 바로 Login(21.2절 8–9단계). `RemoveLocalPlayer`: PlayerController 파괴 → LocalPlayer Subsystem 정리 → 목록에서 뺌.
- 화면 분할·viewport는 렌더 단계(화면 마일스톤)에서 LocalPlayer에 붙인다. 지금은 viewport 자리(빈 식별자)만 둔다.

### 18.5 World 열기(OpenWorld, UE LoadMap)

```cpp
struct MapDesc {
    StringView name;
    WorldKind kind = WorldKind::Game;          // 16.4절
    TypeId directorType;                        // 비면 GameInstance 기본(19.3절)
    StringView options;                         // "?key=value" 형식, InitGame에 넘긴다
    Function<Result<void>(World&)> populate;    // 배치 Actor 생성(레벨 에셋 적재가 생기면 대체)
};
Result<void> GameInstance::OpenWorld(const MapDesc& map) noexcept;   // 프레임 사이에서 실행
```

- 프레임 안에서 부르면 요청을 기록하고 다음 `OnEngineTick`에서 실행한다(마지막 요청만 남긴다).
- 순서: (1) 이전 게임 World가 있으면 21.3절 정리(Pawn·PlayerController `DestroyActor` — Logout), `EndPlay(LevelTransition)`, `Engine::DestroyWorld`(즉시 — 아래) → (2) 새 World 생성·`Initialize`(WorldSubsystem, 목록에 `GameplayWorldSubsystem`을 넣는다 — 19.5절) → (3) 21.2절 6–10단계(GameDirector·배치 Actor·Login·BeginPlay). (2)–(3)이 실패하면 새 World를 정리하고 실패를 돌려준다(이전 World는 이미 없다 — UE `LoadMap` 실패도 같다).
- **World 파괴 시점(Engine 변경, S10).** 지금 `Engine::DestroyWorld`는 프레임 안이면 프레임 끝으로 미룬다(`Engine.cpp:489–491`). 이를 "World Tick 중(6절 RunFrame ④)에만 미룬다"로 바꾼다. `OnEngineTick`(③)·`OnEngineReady`·`OnEngineShutdown`에서는 즉시 파괴하므로, 이전 World의 `PreDeinitialize`·`Deinitialize`가 새 World 생성 **전**에 끝난다.
- LocalPlayer와 GameInstance Subsystem은 World를 넘어 유지된다. PlayerState 이어받기(UE seamless travel `CopyProperties`)는 후속이다(19.6절).
- `populate`는 레벨 에셋 적재(에셋 A 단계·Level 패키지)가 생기기 전 배치 Actor를 만드는 자리다. 생기면 맵 에셋 id로 바꾼다.

### 18.6 시험(완료 조건)

- `OnEngineStart` 순서(①–⑥)와 각 단계 실패 시 역순 정리.
- DedicatedServer: LocalPlayer 없음, Login 없음, World는 BeginPlay.
- LocalPlayer 추가·제거 시점의 Subsystem 초기화·정리, BeginPlay 뒤 추가 시 즉시 Login.
- OpenWorld: 프레임 안 요청 지연, 이전 World 정리 순서(Pawn → PlayerController → EndPlay(LevelTransition) → 파괴), LocalPlayer·GameInstance Subsystem 유지.
- `OnEngineShutdown` 순서(①–⑤)와 Engine Subsystem 정리가 그 뒤.
