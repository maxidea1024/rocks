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
- 문서: [게임플레이 작업 안내](../../Guides/GameplayWorkflowGuide.ko.md)에 GameInstance·GameDirector·Controller·Pawn 사용법과 14단계 흐름 요약을 더한다.

### 22.2 결정(권장안)

| ID | 항목 | 권장안 | 근거 |
|---|---|---|---|
| ERD1 | Component Tick 자동 간선 | 소유 Actor 뒤 자동 간선을 없애고 UE처럼 부착 관계(자식 → 부모) 간선만 둔다. 같은 priority면 등록 순서로 지금처럼 Actor가 먼저다. 8절의 "UE와 같다" 서술을 고친다 | UE에 Actor → Component 기본 의존이 없다(15.2절) |
| ERD2 | 순환 | 사용자 호출은 선언 그래프 전체에서 검사해 거부, 자동 간선은 생략 + Warning | 원인 호출에서 실패, 자동 간선은 UE 결과와 같게 |
| ERD3 | 단계를 넘는 의존 | 모든 간선을 한 그래프에, 실효 단계 = max(요청, 선행 실효)를 프레임 시작에 계산(UE demotion). Fixed↔다른 hook 거부, Update·Late는 Physics 외 어느 단계로든 늦춰질 수 있음 | UE `TickTaskManager.cpp:2674–2684`. 단계 경계 이동은 두 번 실행·누락 위험 |
| ERD4 | 꺼진 선행 | 실행만 건너뛰고 간선 유지. 단계 늦춤은 선행이 꺼져도 유지(UE와 다름) | 같은 단계는 관찰 결과 같음, 단계는 프레임마다 흔들리지 않게 |
| ERD5 | 시작 전 선언 | 허용, World 선언 그래프에 두고 등록 때 적용 | UE는 등록 전 선언 허용 |
| ERD6 | 실행 중 변경 | priority·interval은 다음 단계 경계, 단계는 다음 프레임 시작 | UE `SetTickGroup` 등 |
| ERD7 | 엔진 단계 자리 | Physics 끝 2개, LateUpdate 시작 3개(Timers → tickable → Camera). Camera는 pause 중 생략 | UE `LevelTick.cpp` 순서 |
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
