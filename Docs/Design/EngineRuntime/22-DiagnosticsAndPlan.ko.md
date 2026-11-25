# 엔진 런타임 — 22. 진단·결정·작업 순서(S14)

[목차](README.ko.md)

## 22. 진단·결정·작업 순서(S14)

설계일 2026-11-23.

### 22.1 진단

| 진단 | 내용 | 출력 |
|---|---|---|
| `DumpTickGraph(World&)` | 단계마다 실행 순서대로 항목(타입 이름·handle·hook·priority·interval·켜짐), 선행 목록, 늦춰진 항목의 원래 단계, 엔진 단계 자리 등록 여부 | 텍스트(로그 `LogWorldTick` Display, 콘솔 명령 `world.dumpticks`) |
| `DumpSubsystems` | 계층(Engine·GameInstance·LocalPlayer·World)마다 초기화 순서, 생략된 것과 이유(조건 거짓·의존 누락) | 로그, `engine.dumpsubsystems` |
| `DumpGameplay` | GameInstance → LocalPlayer → PlayerController(state) → Pawn·PlayerState·CameraDirector(view target·CameraView), GameDirector·GameState(playerStates·hasBegunPlay) | 로그, `game.dumpframework` |
| 통계 | `WorldStats`에 늦춰진 항목 수·엔진 단계 시간·LifeSpan 대기 수, `GameplayStats`에 Controller·Pawn·PlayerState 수 | 기존 통계 경로 |
| 진단 코드 | `SubsystemSkippedMissingDependency`, `AutoTickPrerequisiteCycleSkipped`, `BeginPlayDispatchContractViolated`, `InvalidDirectorType`, `PossessDeniedNoAuthority` | 로그 + Result 오류 |

- 콘솔 명령은 CVar 시스템의 명령 등록([CVar 시스템](../CVarSystem.ko.md))으로 둔다. 로그 문구는 영어다.
- 문서: GameInstance·GameDirector·Controller·Pawn 사용법과 14단계 흐름 요약은 새 안내 [게임 프레임워크 안내](../../Guides/GameFrameworkGuide.ko.md)에 둔다. [게임플레이 작업 안내](../../Guides/GameplayWorkflowGuide.ko.md)는 이미 500줄을 넘었다. 그 문서를 고치면 장별 분리 이동을 먼저 해야 하는데([문서 배치 규칙](../../Guides/DocumentLayout.ko.md) 4절), 그 문서는 다른 세션과 같이 쓴다. 그래서 내용은 새 안내에 두고 서로 링크한다.

S14 구현:
- `World::DumpTickGraph(String&)`는 TickScheduler의 실제 단계 순서 배열을 읽기만 한다(`TickScheduler::ForEachInPhase`). 덤프 때문에 순서를 다시 만들면 다음 단계 경계에 바뀔 변경이 앞당겨지기 때문이다. 아직 순서에 들지 않은 항목은 끝에 `pending`으로, 다음 실행 때 다시 만들 단계는 `(order rebuilt at next run)`으로 표시한다. 이름은 `타입#slot`이다(Actor·Component에는 이름이 없다).
- `World::DumpSubsystems`·`Engine::DumpSubsystems`는 `SubsystemCollection::Describe`(이름 목록 `NameAt`)를 쓴다. Engine이 모르는 계층(GameInstance·LocalPlayer)은 `EngineClient::DescribeSubsystems`로 client가 붙인다.
- `GameInstanceClient::DumpFramework`와 `GameplayStats`(`GameplayWorldSubsystem::Stats(world)`)를 둔다. `WorldStats`에는 `ticks.demoted`·`engineStageNanoseconds`·`pendingLifeSpans`를 더한다.
- 콘솔 명령은 Engine이 설정 문맥에 `engine`·`world` 영역을 더하고 `engine.dumpsubsystems`·`world.dumpticks`를 묶는다. `game.dumpframework`는 GameInstanceClient가 묶는다(영역 `game`은 기본 표). 둘 다 종료 전에 푼다. 출력은 콘솔 sink와 Info 로그다. 설정 문맥 하나를 Engine 여럿이 같이 쓰면 마지막에 묶은 Engine(과 그 client)이 명령 대상이다. 그 Engine이 먼저 끝나면 명령은 대상 없음(`NotReady`)이 된다. 대상 목록은 두지 않는다 — 한 프로세스에 Engine이 여럿인 것은 시험뿐이기 때문이다.
- 리뷰 반영(S14): `TickStats.demoted`는 마지막 재계산 값이다. 덤프는 재계산 대기를 머리줄에 표시하고, 순서 배열에 없는 항목(재구성 할당 실패 포함)을 `pending`으로 빠짐없이 쓴다. 엔진 단계 자리는 실행 위치대로 쓴다(LateUpdate는 Tick 항목 앞 `at start`, Physics는 뒤 `at end`, `PhysicsSimulate`는 `after each fixed step`). 진단 덤프의 낡은 handle은 건너뛰고 할당 실패만 오류로 돌린다.
- 진단 코드는 로그의 `{Code}` 속성으로 싣는다(World `WorldLog.h`, Gameplay `GameplayTypes.h` 상수). 따로 진단 수집기는 두지 않는다. 지금은 로그가 진단 경로이기 때문이다.
- 시험: `WorldTick_DumpTickGraphListsOrderPrerequisitesAndDemotion`, `WorldSubsystem_SkipChainsAndOptionalCycles`(생략 줄), `GameplayDiagnostics_ConsoleDumpsDescribeEveryLayer`.

### 22.2 결정(권장안)

| ID | 항목 | 권장안 | 근거 |
|---|---|---|---|
| ERD1 | Component Tick 자동 간선 | 소유 Actor 뒤 자동 간선을 없애고 UE처럼 부착 관계(자식 → 부모) 간선만 둔다. 같은 priority면 등록 순서로 지금처럼 Actor가 먼저다. 8절의 "UE와 같다" 서술을 고친다 | UE에 Actor → Component 기본 의존이 없다(15.2절) |
| ERD2 | 순환 | 사용자 호출은 선언 그래프 전체에서 검사해 거부, 자동 간선은 생략 + Warning | 원인 호출에서 실패, 자동 간선은 UE 결과와 같게 |
| ERD3 | 단계를 넘는 의존 | 모든 간선을 한 그래프에, 실효 단계 = max(요청, 선행 실효)를 프레임 시작에 계산(UE demotion). Fixed↔다른 hook 거부, Update·Late는 Physics 외 어느 단계로든 늦춰질 수 있음 | UE `TickTaskManager.cpp:2674–2684`. 단계 경계 이동은 두 번 실행·누락 위험 |
| ERD4 | 꺼진 선행 | 실행만 건너뛰고 간선 유지. 단계 늦춤은 선행이 꺼져도 유지(UE와 다름) | 같은 단계는 관찰 결과 같음, 단계는 프레임마다 흔들리지 않게 |
| ERD5 | 시작 전 선언 | 허용, World 선언 그래프에 두고 등록 때 적용 | UE는 등록 전 선언 허용 |
| ERD6 | 실행 중 변경 | priority·interval은 다음 단계 경계, 단계는 다음 프레임 시작 | UE `SetTickGroup` 등 |
| ERD7 | 엔진 단계 자리 | Physics 고정 스텝마다 1개(`PhysicsSimulate` — 그 스텝의 사용자 Fixed hook 뒤), Physics 끝 2개, LateUpdate 시작 3개(Timers → tickable → Camera). Camera는 pause 중 생략 | UE `LevelTick.cpp` 순서. 스텝마다 자리는 UE async 물리의 `FAsyncPhysicsTickCallback`(step 직전 async tick 컴포넌트·Actor — `PhysicsEngine/Experimental/PhysScene_Chaos.cpp:145–157`)처럼 사용자 고정 tick 다음에 step이다 |
| ERD8 | Subsystem 의존 | 정적 선언·생성 전 검사·역순 정리 유지 | UE와 다름(순환 미감지·정방향) — 안전 |
| ERD9 | 조건부 생성 | `shouldCreate`, 필수 의존 누락 시 의존자 생략(진단), 선택 의존 | UE는 의존자를 그래도 만든다 — 필수 의존 계약 우선(차이) |
| ERD10 | 계층 조회 | World에는 타입 지운 조회 창만, 타입 도우미는 Engine·Gameplay 자유 함수. 조회 창은 EndPlay 끝에 닫힘 | 3절 의존 방향, GameInstance Subsystem이 World 정리보다 먼저 사라짐 |
| ERD11 | 새 Subsystem 종류 | GameInstance·LocalPlayer, UE 시점 | UE |
| ERD12 | WorldSubsystem hook | PostInitialize·OnWorldEndPlay·PreDeinitialize·tickable | UE |
| ERD13 | EndPlay 사유 | `OnDestroy(EndPlayReason)`, 다섯 값, OnAwake 성공 객체는 시작 전이어도 받음 | UE 값 + 7.3절 규칙 유지 |
| ERD14 | Spawn 인자 | transform·owner·instigator, 이름은 Core 이름 체계 뒤 | UE `FActorSpawnParameters` |
| ERD15 | Owner | 고리 거부, Children 배열 없음 | UE `SetOwner` |
| ERD16 | LifeSpan | LateUpdate 시작 Timers 자리, 게임 시간, (만료 시각, 설정 순번) 순 | UE 타이머 위치 |
| ERD17 | World EndPlay | `World::EndPlay(reason)` 분리, Level마다 LevelScript 먼저, Shutdown이 먼저 부름 | UE `UWorld::EndPlay`, LD8·LD16 |
| ERD18 | Component 시작 준비 | `IsReadyToStart`, Actor는 켜진 Component 모두 준비까지 대기 | CR4·SE-W2 |
| ERD19 | 프레임워크 내부 hook | `OnFrameworkPreAwake`·`OnFrameworkPostAwake`·`OnFrameworkEndPlay`(프레임워크 클래스만, final, 17.8절) | Super 호출 의존 제거, UE Pre/PostInitializeComponents 자리 |
| ERD20 | PlayerStart 선택 | Spawn 순번 첫 번째(결정적), 겹침 판정은 물리 뒤 | 재현성 — UE는 무작위, Level 배열은 무순서 |
| ERD21 | 경기 상태 기계·seamless travel·입력 스택 | 후속 | 각각 AGameMode·네트워크·InputComponent 단계 |
| ERD22 | Engine 변경 | `EngineClient::OnEngineReady`(Running 뒤 맵 열기), `DestroyWorld`는 World Tick 중에만 미룸, `DestroyWorld(reason)`, `WorldCreateDesc`의 `kind`·바깥 조회 창 | `OnEngineStart`는 Initializing이라 World를 못 만든다, 전환 때 이전 World 정리가 새 World보다 먼저 |
| ERD23 | 전환·종료 정리 | PlayerController·spawn한 Pawn을 `DestroyActor`로(Logout), 배치 Pawn은 남김, 종료도 `EndPlay(Quit)` 전에 같은 정리 | UE LoadMap, LD8, Director가 살아 있을 때 Logout |
| ERD24 | GameInstance hook | `OnInit`(Subsystem 전)·`OnPostInit`(Subsystem 뒤) | UE BP `ReceiveInit`·C++ `Super::Init` 뒤 자리 |

### 22.3 작업 순서

단계마다 구현 → 바뀐 시험만 실행 → 리뷰어 1명 → 반영·커밋·푸시(작업은 worktree `.claude/worktrees/runtime-s7`, 브랜치 `feature/engine-runtime-s7` — 준비되면 engine-runtime에 병합). 전체 시험 체인·커버리지는 S14 끝에 한 번.

| 단계 | 내용 | 막는 것 |
|---|---|---|
| S7a | TickScheduler: 단계와 무관한 한 그래프·전체 순환 검사, 실효 단계(프레임 시작 계산·이동), Fixed 혼합 거부, `SetPhase`·`SetPriority`·`SetInterval`, 실효 단계 조회 | — |
| S7b | World 의존 API(Actor·Component·hook), 선언 그래프·시작 전 선언·제거, 실행 중 변경, 부착 간선, 자동 Actor → Component 간선 제거(ERD1), 엔진 단계 자리(15.4절) | S7a |
| S8 | Subsystem `shouldCreate`·선택 의존·WorldKind(`WorldDesc`·`WorldCreateDesc`)·바깥 조회 창·`GetEngineSubsystem`·WorldSubsystem hook·tickable | S7b(엔진 단계 자리) |
| S9 | EndPlay 사유·SpawnParams·Owner·Component 준비·LifeSpan·World EndPlay(LevelScript 먼저)·프레임워크 hook 셋·`SetBeginPlayDispatcher`·`DispatchBeginPlay`·`DestroyWorld(reason)` | S7b, S8 |
| S10 | Gameplay 모듈, GameInstance·LocalPlayer·GameInstanceClient(`OnEngineReady` 포함)·OpenWorld, `DestroyWorld` 즉시 규칙, GameInstance·LocalPlayer Subsystem, `GetGameInstanceSubsystem` | S8, S9 |
| S11 | GameDirector·GameState·PlayerState(직접 spawn 경로)·PlayerStart·GameplayWorldSubsystem | S10 |
| S12 | Controller·Pawn·PlayerController·CameraDirector·AiController, PlayerState 수명 시험 | S11 |
| S13 | 14단계 시나리오·Director 없는 World 시나리오 시험, Sandbox 연결 | S12 |
| S14 | 진단 덤프·콘솔 명령·Guide 갱신, 전체 시험·커버리지 | S13 |

### 22.4 다른 문서의 개정 요청(받은 것)

받은 요청은 여기에 적고, 해당 절을 고친 뒤 상태를 바꾼다.

| 요청한 문서 | 고칠 절 | 요청 | 필요한 때 | 상태 |
|---|---|---|---|---|
| [물리 모듈](../PhysicsModule/07-StagesDecisions.ko.md) 7.4절 | 8·15절 | Physics 단계 고정 스텝마다 사용자 Fixed hook 뒤에 엔진 자리 `PhysicsSimulate`를 하나 더한다(지금은 단계 끝 자리 PhysicsResults·SkeletalPhysicsFinalize 둘). 등록은 `SetEngineStage`와 같은 방식 | PH1 전 | 반영함(2026-11-25 — 8·15.4절, rocks-45 요청으로 CR21 중간에 앞당김) |
| 같음 | 23절 | 즉시 다시 만들기 `RecreateSystemStateNow(kind)`(UE `EnsurePhysicsStateCreated`). ERD25의 PreRender 일괄과 별도이고 실패 정책은 같다(값 변경은 성공, Warning `SystemStateRecreateFailed`) | PH2 전 | 계획됨 — CR21 뒤 |
| 같음 | 23·24절(World 코드) | `World::GetPhysicsScene`·`SetPhysicsScene`(World 모듈 추상 접점 `PhysicsScene`), PrimitiveComponent 물리 시스템 상태 판정·생성, `OnTransformUpdated` hook의 body 이동, 이동 API `TeleportMode` 인자, simulate를 켤 때 부모에서 떼기, `MoveComponent` sweep·`UpdateOverlaps` | PH2·PH4 때 | 맡을 곳은 그때 정한다 |
