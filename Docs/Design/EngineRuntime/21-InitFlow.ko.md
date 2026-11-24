# 엔진 런타임 — 21. 초기화·종료 흐름과 시나리오(S13)

[목차](README.ko.md)

## 21. 초기화·종료 흐름과 시나리오(S13)

설계일 2026-11-23. 1절 표의 S7–S14 완료 기준 "붙임 계획 24절의 14단계 시나리오"는 저장소에 원문이 없다(계획 24절은 Unreal 참고 체계 추가 도입이다). 이 절이 그 시나리오를 UE 5.8 순서(18–20절의 UE 계약)로 다시 정한 정본이다.

### 21.1 원칙

- 단계 순서는 UE standalone 경로와 같다: Login·Possess가 World BeginPlay **전**에 끝나고, BeginPlay는 GameDirector `StartPlay` → GameState `HandleBeginPlay` → Actor 시작이다(UE `E/Private/UnrealEngine.cpp:16620–16661`, `E/Private/World.cpp:6153–6187`).
- 문서화된 UE와의 차이(이 흐름에 보이는 것):
  - BeginPlay 뒤 Spawn한 Actor의 `OnStart`가 다음 프레임 시작(7.2절).
  - Subsystem 정리·`OnWorldEndPlay`·`PreDeinitialize`가 역순, 필수 의존이 없는 Subsystem은 생략(ERD8·ERD9).
  - `OnAwake`가 성공한 객체는 `OnStart` 전이어도 `OnDestroy`를 받음(17.2절).
  - 사용자 Tick 의존의 순환은 호출 때 거부, 단계 늦춤은 프레임 시작에 정적으로(ERD2·ERD4).
  - PlayerStart는 Spawn 순번 첫 번째(ERD20).
  - Director 없는 World는 `BeginPlay`가 바로 Actor를 시작(21.4절).
  - 종료 때 PlayerController를 `EndPlay(Quit)` 전에 정리(18.2절), 전환 때 배치 Pawn은 파괴하지 않음(21.3절).

### 21.2 14단계 시나리오(standalone)

| 단계 | 하는 일 | 관찰할 사건(시험 로그) |
|---|---|---|
| 1 | `Engine::Initialize`: 엔진 Registry → EngineSubsystem 초기화(의존 순서) | `EngineSubsystem.Initialize` |
| 2 | `GameInstanceClient::OnEngineStart`(Engine Initializing): GameInstance 생성 → `OnInit` → GameInstance Subsystem 초기화 → `OnPostInit` | `GameInstance.OnInit`, `GiSubsystem.Initialize`, `GameInstance.OnPostInit` |
| 3 | 첫 LocalPlayer 생성 → LocalPlayer Subsystem 초기화 | `LocalPlayer.Added`, `LpSubsystem.Initialize` |
| 4 | Engine Running·PostEngineInit 모듈 시작 뒤 `OnEngineReady` → `StartGameInstance` → 기본 맵 `OpenWorld` | `OpenWorld(begin)` |
| 5 | World 생성·`Initialize`: WorldSubsystem `Initialize` → `PostInitialize` | `WorldSubsystem.Initialize`, `.PostInitialize` |
| 6 | GameDirector 선택·`SpawnActorDeferred` → `InitGame` → `FinishSpawning`(`OnFrameworkPreAwake`: GameState spawn — GameState는 자기 `OnFrameworkPostAwake`에서 등록 → Director `OnAwake`) | `Director.InitGame`, `GameState.Awake`, `Director.Awake` |
| 7 | 배치 Actor `populate`(PlayerStart, 배치 Pawn — `autoPossessAi`면 AiController spawn·Possess) | `Actor.Awake`…, `AiController.Possess` |
| 8 | LocalPlayer마다 `Login`: PlayerController 지연 spawn → FinishSpawning(PlayerState spawn·GameState 추가, CameraDirector spawn, Spectating) → `InitNewPlayer` | `PlayerController.Awake`, `PlayerState.Added`, `CameraDirector.Awake` |
| 9 | `SetPlayer`(LocalPlayer↔PlayerController) → `PostLogin` → `HandleStartingNewPlayer` → `RestartPlayer`: `FindPlayerStart` → `SpawnDefaultPawnFor`(PlayerStart Transform) → `FinishRestartPlayer` → `Possess`(PossessedBy·SetPawn·ControlRotation·Restart) | `Director.PostLogin`, `Pawn.Possessed`, `Pawn.Restart` |
| 10 | `World::BeginPlay`: WorldSubsystem `OnWorldBeginPlay` → `GameDirector::StartPlay` → `GameState::HandleBeginPlay` → `DispatchBeginPlay`(BeginPlay 전 Spawn한 모든 Actor·Component `OnStart`, hook 순서는 World.h) → `matchStarted` | `WorldSubsystem.OnWorldBeginPlay`, `Director.StartPlay`, `Actor.Start`… |
| 11 | 프레임: 게임 시계 → 대기 `OnStart` → PrePhysics(PlayerController PlayerTick → Pawn Tick) → Physics(고정 스텝, 끝에 엔진 단계) → PostPhysics → LateUpdate(Timers·LifeSpan → tickable Subsystem → CameraUpdate → 등록 항목) → PreRender → 파괴 정리 → GC | `PC.PlayerTick`, `Pawn.Tick`, `Camera.Update` |
| 12 | 게임 중 Pawn 파괴: `OnFrameworkEndPlay` → `PawnPendingDestroy`(PlayerController는 Inactive, PlayerState 유지) → 게임이 `RestartPlayer`로 다시 spawn | `Controller.PawnPendingDestroy`, 새 `Pawn.Possessed` |
| 13 | World 전환 `OpenWorld(다음 맵)`: 21.3절 정리(spawn한 Pawn 파괴 → PlayerController 파괴 — `Logout`·PlayerState 제거) → `EndPlay(LevelTransition)`(LevelScript 먼저 Actor 정리 → `OnWorldEndPlay`) → World 즉시 파괴(`PreDeinitialize` → `Deinitialize` 역순) → 새 World에서 5–10단계. LocalPlayer·GameInstance Subsystem은 유지 | `Director.Logout`, `EndPlay(LevelTransition)`, 새 `Director.InitGame` |
| 14 | 종료: `Engine::RequestExit` → `OnEngineShutdown`: 21.3절 정리(`Logout` 포함) → `EndPlay(Quit)` → `OnShutdown` → LocalPlayer 역순 제거(LocalPlayer Subsystem 정리) → GameInstance Subsystem 정리 → Engine이 World 파괴 → EngineSubsystem 정리(역순) | `GameInstance.OnShutdown`, …, `EngineSubsystem.Deinitialize` |

시험 `GameplayFlow_FourteenStepScenario`가 위 사건 로그를 순서대로 단언한다(순서 이외의 시각·경쟁에 기대지 않는다 — 단계마다 명시 호출과 프레임 수). 1–12단계는 사건 로그 전체를 정확히 비교한다. 13·14단계는 그 단계의 사건을 순서 부분열로 비교한다. 정리 경로가 Actor 수만큼 사건을 내고, 그중 이 표가 정한 것은 순서뿐이기 때문이다. 관찰하지 않는 것은 이렇다. 4단계의 OpenWorld(begin)는 5단계 사건이 ClientReady 안에서 나는 것으로 대신한다. 8단계의 CameraDirector.Awake는 타입이 고정이라 사건이 없고, 핸들과 view target으로 확인한다. 11단계의 카메라 순서는 Pawn Tick이 옮긴 위치가 같은 프레임 카메라 결과에 보이는 것으로 확인한다. Pawn Tick이 Controller Tick 뒤라는 간선 자체는 `ControllerPawn_SetPawnOrdersPawnTickAfterController`가 확인한다(시나리오에서는 PlayerController가 먼저 Spawn되어 등록 순서만으로도 통과한다).

### 21.3 종료·전환 정리 규칙

- **전환(OpenWorld).** UE `LoadMap`처럼 LocalPlayer마다 Pawn을 먼저, 그다음 PlayerController를 `DestroyActor`로 지운다(사유 `Destroyed` — UE `UnrealEngine.cpp:16245–16250`, `Actor.cpp:3313`). PlayerController의 `OnFrameworkEndPlay`가 `Logout`을 부른다(UE `Controller.cpp:595–605` — Logout이 PlayerController EndPlay보다 먼저).
  - Pawn은 Spawn한 것(`SpawnDefaultPawnFor` 등)만 지운다. Level에 배치된 Pawn은 UnPossess만 하고 `EndPlay`에 맡긴다 — LevelScript가 자기 Level의 다른 Actor보다 먼저 정리돼야 하므로(LD8). UE는 possess 중인 Pawn을 배치 여부와 관계없이 지운다(문서화된 차이).
- **종료(OnEngineShutdown).** 같은 정리를 `EndPlay(Quit)` 전에 한다. UE는 `EndPlay(Quit)` 뒤 `RemoveLocalPlayer`가 PlayerController를 지운다(18.2절). Rocks는 GameDirector가 살아 있을 때 `Logout`이 돌게 앞에 둔다(문서화된 차이).
- 남은 Actor는 `World::EndPlay`가 그 사유(`LevelTransition`·`Quit`)로 LevelScript 먼저 정리한다(17.6절).
- 이 정리는 World Tick 밖(`OnEngineTick`·`OnEngineShutdown`)에서 하고, 그 자리에서 끝난다(안전 지점을 기다리지 않는다). World 저장소 파괴도 즉시다(18.5절 Engine 변경).

### 21.4 Director 없는 World 시나리오

편집·미리보기·Client World와 GameInstance 없이 World만 쓰는 도구·시험(지금의 Sandbox·WorldBench)이다.

1. `Engine::CreateWorld`(kind = Editor·Preview, 또는 GameInstance 없는 Game) → WorldSubsystem 초기화. `GameplayWorldSubsystem`은 GameInstance `OpenWorld`가 목록에 넣을 때만 생기므로(19.5절) dispatcher가 없다.
2. Actor Spawn은 지금처럼 동작한다(Initialized까지).
3. `World::BeginPlay`(부르는 쪽이 원할 때만) → `OnWorldBeginPlay` → 바로 `DispatchBeginPlay`. Login·Pawn·카메라 없음.
4. `World::Shutdown` → `EndPlay(Quit)`(Playing이었으면) → 정리.

시험 `GameplayFlow_WorldWithoutDirector`가 단언하고, 기존 World 시험 전부가 이 경로의 회귀 시험이다.

### 21.5 Sandbox 연결

Sandbox는 `GameInstanceClient`를 쓰도록 바꾼다: 기본 맵 = PlayerStart 하나와 기본 Pawn, 창 입력을 LocalPlayer `InputSource`로 넘긴다. 마우스·키보드로 Pawn이 받는 입력을 로그로 확인한다(엔진 1차 목표 입력 확인 — 화면 출력은 화면 마일스톤이 붙인다).

S13 구현(`Programs/Sandbox/Private/SandboxGame.*`):
- `--engine`이 `GameInstanceClient`로 돈다. 기본 맵 "Main"에는 원점 PlayerStart가 있고, `SandboxDirector`가 첫 LocalPlayer에 `SandboxPawn`을 준다.
- `SandboxPawn`은 Controller 입력 스냅숏에서 새로 눌린 W·A·S·D·Space와 마우스 왼쪽·오른쪽 버튼을 Info 로그(`Sandbox.Game`)로 남긴다.
- `SandboxPawn`은 마우스 버튼의 로그에 커서 위치(창 client 픽셀)를 함께 남기고, 첫 게임패드의 South·East 버튼과 휠도 남긴다.
- 시험은 `Engine/Source/Runtime/Gameplay/Tests/GameplayFlowTests.cpp`의 `GameplayFlow_FourteenStepScenario`·`GameplayFlow_WorldWithoutDirector`·`GameplayFlow_EngineDestroyWorldPassesTheReason`다.

입력 연결(2026-11-24, 엔진 1차 목표 입력 확인):
- 호스트의 엔진 루프 hook(`HostLoopBridge`)이 `input::InputQueue`(용량 256)와 `input::InputState`를 소유한다. 프레임 1단계 `PumpEvents`에서 순서대로: 종료 요청 다리 → 창 펌프(`Window::PumpEvents`) → 게임패드 폴링(`GamepadBackend::Poll`) → 시험 주입 → `InputState::BeginFrame(queue.Events())` → `queue.Clear()`. UE에서 `FEngineLoop::Tick`의 `PumpMessages`가 Slate 입력을 모은 뒤 World Tick의 `PlayerController::TickPlayerInput`이 읽는 순서와 같다.
- `engine.Initialize` 뒤 `GameInstanceClient::WithInstance`로 첫 LocalPlayer에 `SetLocalPlayerInput(player, &state)`를 건다. LocalPlayer가 없으면(DedicatedServer) 걸지 않는다. 입력 원천은 엔진 `Shutdown`이 끝날 때까지 산다(hook과 같은 스택 범위).
- `--window`면 `platform::Window`("Rocks Sandbox", 1280×720)를 만든다. 닫기 버튼은 `RequestExit`로 정상 종료한다. 렌더가 없어 present 대기가 없으므로 hook의 `EndFrame`이 프레임 간격을 약 60 Hz로 맞춘다(남은 시간만 `SleepFor` — UE `t.MaxFPS`와 같은 호스트 상한이다). 실제 시간 delta를 쓴다(`fixedFrameDeltaSeconds` 0). `--frames`를 주지 않으면 창이 닫힐 때까지 돈다.
- 창을 만들지 못하면(데스크톱 없음 등) `WindowCreateReport`를 Error로 남기고 실패 코드 1로 끝난다. 게임패드 backend가 없으면 Info 한 줄만 남기고 키보드·마우스로 돈다.
- 헤드리스 `--engine`도 같은 큐·상태를 쓴다(이벤트가 없을 뿐이다). 그래서 창 없이 입력 경로를 확인할 수 있다: 시험용 `--inject-key-at-frame N`은 N번째 `PumpEvents`에서 큐에 `KeyDown(W)`를 넣는다(창 펌프가 넣는 이벤트와 같은 자리). 순서는 프레임 번호로 정해진다(시간·경쟁 무관).
- 자체 시험(`RunEngineHostSelfTest`)이 `--engine --frames 4 --inject-key-at-frame 2`를 띄워 "[Info][Sandbox.Game] Pawn received key W"를 확인한다. 실제 창·마우스·패드는 사람이 `Sandbox --engine --window`로 확인한다(자동 시험에서 OS 입력을 합성하지 않는다 — 포커스·데스크톱 상태에 따라 결과가 달라지므로).
- 버린 대안: 엔진이 창을 소유하는 것. 창·swapchain 소유는 화면 마일스톤의 렌더 호스트(뷰포트 클라이언트) 몫이라 지금은 호스트 hook에 둔다. 그때 hook의 창 펌프가 뷰포트 쪽으로 옮겨 간다.
