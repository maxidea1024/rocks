# 엔진 런타임 — 19. GameDirector·GameState·PlayerState(S11)

[목차](README.ko.md)

## 19. GameDirector·GameState·PlayerState(S11)

설계일 2026-11-23. UE 경로 약어: `E/` = `Engine/Source/Runtime/Engine/`. 이름은 2절(GameDirector = UE `AGameModeBase`, GameState = `AGameStateBase`, PlayerState = `APlayerState`).

### 19.1 UE 5.8 계약

- GameMode는 Actor 초기화 전에 spawn한다(`SetGameMode` — 서버·standalone만, `E/Private/World.cpp:5927–5941`). 클래스 우선순위: WorldSettings 기본 → URL `?Game=` → 맵 접두 별칭 → 프로젝트 기본 → `AGameModeBase`, 마지막에 GameInstance `OverrideGameModeClass`(`E/Private/GameInstance.cpp:1514–1618`).
- `InitGame`은 Actor 초기화(`RouteActorInitialize`) 전에 부르고 이때 GameState는 아직 없다(`World.cpp:6032–6034`, `E/Private/GameModeBase.cpp:82–105`). GameState는 GameMode의 `PreInitializeComponents`에서 spawn하고 `InitGameState`로 클래스 정보를 넘긴다(`GameModeBase.cpp:116–144`). GameState의 PostInit이 World에 자신을 등록하고 이미 있는 PlayerState를 모은다(`E/Private/GameStateBase.cpp:54–75`).
- `StartPlay` → `GameState::HandleBeginPlay` → `WorldSettings::NotifyBeginPlay`(모든 Actor `DispatchBeginPlay`, `SetBegunPlay(true)`) → `NotifyMatchStarted`(`GameModeBase.cpp:204–207`, `GameStateBase.cpp:205–211`, `E/Private/WorldSettings.cpp:363–386`).
- PlayerState는 Controller가 PostInit에서 spawn(Owner = Controller)하고(`E/Private/Controller.cpp:652–694`, `E/Private/PlayerController.cpp:1073–1077`), PlayerState PostInit에서 `GameState::AddPlayerState`, Destroyed에서 `RemovePlayerState`(`E/Private/PlayerState.cpp:139–150, 204–210`). Pawn이 죽거나 possession이 바뀌어도 유지되고 Controller 파괴 때 `CleanupPlayerState`로 없앤다.
- GameMode가 없는 World(client·편집)는 `InitGame`·`StartPlay`가 없고, client는 GameState 복제가 BeginPlay를 트리거한다(`GameStateBase.cpp:196–203`).

### 19.2 GameDirector

`GameDirector : Actor`(배치 불가, 서버·standalone 전용). 가진 것과 hook:

| 항목 | 뜻 | UE |
|---|---|---|
| `gameStateType`·`playerControllerType`·`playerStateType`·`defaultPawnType`·`aiControllerType` | 만들 클래스(TypeId, 기본은 엔진 기본 클래스). 파생은 생성자에서 정한다 | `GameStateClass` 등 |
| `InitGame(MapDesc options)` | FinishSpawning 전에 부른다. options 해석 | `InitGame` |
| `OnFrameworkPreAwake`(17.8절, 사용자 `OnAwake` 전) | GameState spawn → `InitGameState` | `PreInitializeComponents` |
| `Login(LocalPlayer&) → Result<ObjectHandle>` | PlayerController spawn(지연) → FinishSpawning → `InitNewPlayer` | `Login` |
| `PostLogin(PlayerController&)` | `OnPostLogin` hook → `HandleStartingNewPlayer` | `PostLogin` |
| `HandleStartingNewPlayer`·`RestartPlayer`·`FindPlayerStart`·`SpawnDefaultPawnFor`·`FinishRestartPlayer` | 21.2절 9단계 | 같은 이름 |
| `Logout(Controller&)` | PlayerController 파괴 때(EndPlay 전) | `Logout` |
| `StartPlay()` | `GameState::HandleBeginPlay`를 부른다 | `StartPlay` |

- 사용자 재정의는 UE와 같은 이름의 `virtual`로 두되, 엔진 순서를 깨지 않도록 각 단계의 기본 구현은 `protected` 도우미로 노출한다(파생이 기본 흐름을 그대로 부를 수 있게).
- 경기 상태 기계(UE `AGameMode`의 MatchState — WaitingToStart·InProgress·WaitingPostMatch)는 후속이다. GameDirector는 `AGameModeBase` 범위다.
- **PlayerStart 선택.** `PlayerStart : Actor`(배치 표지, root Transform만). `FindPlayerStart`는 이름 일치(`options`의 `?start=`) → PlayerStart 중 **Spawn 순번**(World가 Actor마다 매기는 단조 증가 번호 — `populate` 순서가 그대로 남는다. Level `actors_`는 순서를 보장하지 않으므로 쓰지 않는다 — 7.2절)이 가장 작은 것 → 원점이다. UE `ChoosePlayerStart`는 겹치지 않은 자리 중 무작위를 쓴다. Rocks는 재현성(결정적 시험)을 위해 첫 번째로 두고(문서화된 차이, 무작위가 필요하면 파생이 재정의), 겹침 판정은 물리 질의(CR16)가 생기기 전에는 하지 않는다.

### 19.3 클래스 선택과 생성 조건

- GameDirector는 World 종류가 `Game`·`Tryout`이고 `NetMode`가 `Client`가 아닐 때만 만든다(UE: 서버·standalone만, 편집 World 없음).
- 타입 우선순위: `MapDesc::directorType` → GameInstance `defaultDirectorType`(프로젝트 기본) → `GameDirector`. 마지막에 GameInstance `virtual OverrideDirectorType(TypeId)`를 거친다. `?Game=` 옵션과 맵 접두 별칭은 문자열로 타입을 찾는 레지스트리 조회가 필요해 후속이다.
- 선택한 타입이 `GameDirector` 파생이 아니면 `InvalidArgument`로 OpenWorld가 실패한다.

### 19.4 GameState

`GameState : Actor`(배치 불가, 모든 참여자에게 복제할 경기 상태 — 계획 20.5절). 가진 것: `directorType`(정보용), `playerStates`(PlayerState handle 배열, 추가 순서), `hasBegunPlay`, `matchStarted`.

- `OnFrameworkPostAwake`: `GameplayWorldSubsystem`(19.5절)에 자신을 등록하고 이미 있는 PlayerState를 모은다.
- `HandleBeginPlay()`: `hasBegunPlay = true` → `World::DispatchBeginPlay()`(21.2절 10단계) → `matchStarted = true`.
- `AddPlayerState`·`RemovePlayerState`: 중복 추가 무시, 없는 것 제거 무시(UE `AddUnique`·`Remove`).

### 19.5 World와의 연결

World 모듈은 Gameplay를 모른다(3절 의존 방향). Gameplay 모듈의 `GameplayWorldSubsystem`(GameInstance의 `OpenWorld`가 World Subsystem 목록에 넣을 때만 생긴다 — World 종류만으로 기본 생성하지 않는다. 그래서 GameInstance 없이 만든 Game World에는 없다, 21.4절)이 GameDirector·GameState handle과 Controller 목록을 든다. World 쪽에는 시작 위임 자리 하나만 더한다.

- `World::SetBeginPlayDispatcher(BeginPlayDispatcher*)`: 있으면 `World::BeginPlay`가 Subsystem `OnWorldBeginPlay` 뒤 `dispatcher->StartPlay(world)`를 부르고, dispatcher가 정확히 한 번 `World::DispatchBeginPlay()`(상태를 Playing으로 바꾸고 대기 Actor를 시작)를 불러야 한다. 안 부르거나 StartPlay가 그 전에 실패하면 `BeginPlay`가 실패하고(미호출은 `NotReady`) `OnWorldBeginPlay`를 받은 Subsystem에 `OnWorldEndPlay`로 짝을 맞춘 뒤 Initialized에 남으며(다시 시도할 수 있다), 두 번째 호출과 `StartPlay` 밖의 호출은 `WrongContext`다(진단 `BeginPlayDispatchContractViolated`). 없으면 `BeginPlay`가 바로 `DispatchBeginPlay`한다 — **Director 없는 World 시나리오**(21.4절).
- `GameplayWorldSubsystem`이 dispatcher이며 `StartPlay`에서 `GameDirector::StartPlay()`를 부른다. GameDirector가 없는 Game World(Client)면 바로 `DispatchBeginPlay`한다(복제 트리거는 네트워크 단계).

### 19.6 PlayerState

`PlayerState : Actor`(배치 불가). Controller가 `OnFrameworkPostAwake`에서 spawn하고(Owner = Controller, 타입은 GameDirector `playerStateType`, Director 없으면 기본), PlayerState `OnFrameworkPostAwake`에서 GameState에 추가, `OnFrameworkEndPlay`에서 제거한다.

- 가진 것: `playerId`(GameDirector가 World 안에서 1부터 매기는 번호), `playerName`, `isSpectator`, `isBot`, `pawn`(현재 Pawn handle, Possess가 고친다).
- Pawn 파괴·possession 변경에도 유지되고 Controller 파괴 때 함께 파괴한다(UE `CleanupPlayerState`).
- 비활성 PlayerState(재접속 복구)와 World 전환 이어받기(`CopyProperties`)는 네트워크·travel 단계 후속이다. 모듈식 게임플레이 `PlayerStateComponent`(CR48)는 이 클래스를 소유자로 쓴다.

### 19.7 시험(완료 조건)

- Director 생성 조건(종류·NetMode)과 타입 우선순위·override hook·잘못된 타입 거부.
- InitGame이 FinishSpawning 전, GameState가 Director OnAwake에서 생성·등록.
- StartPlay → HandleBeginPlay → DispatchBeginPlay 순서, dispatcher 계약 위반(미호출·중복) 진단.
- PlayerState를 직접 spawn했을 때 GameState 등록·제거와 playerId 순번(S11). Controller가 만드는 경로·Pawn 파괴에도 유지·Controller 파괴 때 제거는 S12 시험이다.
- PlayerStart 선택 규칙(이름 일치 → Spawn 순번 최소 → 원점), Level 배열 순서와 무관.
