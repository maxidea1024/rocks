# 엔진 런타임 — 7. World·Actor·Component

[목차](README.ko.md)

## 7. World·Actor·Component

### 7.1 World 상태

`Created → Initialized(WorldSubsystem 초기화) → Playing(BeginPlay 이후) → TearingDown → Destroyed`. 초기화와 게임플레이 시작을 분리한다. TearingDown 이후 Spawn·Tick 등록은 `ShuttingDown`으로 거부한다.

### 7.2 Actor 수명

| 상태 | 뜻 | 진입 |
|---|---|---|
| `Constructed` | 존재함. Component 추가 가능. Tick 없음 | `SpawnActorDeferred` |
| `Initialized` | Component 등록·`OnAwake` 완료 | `FinishSpawning` |
| `Playing` | 의존성 준비 완료, `OnStart` 호출, Tick 참여 | World가 Playing이고 `IsReadyToStart()`가 참일 때 프레임 시작 처리 |
| `PendingDestroy` | 파괴 요청됨. Tick 중지. 일반 API 거부 | `DestroyActor` |
| (레코드 제거) | `OnDestroy`·Component 해제·`RequestDestroy`·루트 반환 | 프레임의 안전 지점 |

- 활성(`enabled`)은 상태와 별개 플래그이며 `OnEnable`/`OnDisable`을 부른다.
- `PendingDestroy`인 Actor(와 그 Component)는 Registry 상태로는 프레임 끝 정리까지 살아 있다. 그래서 함수 리플렉션 호출([함수 리플렉션](../FunctionReflection.ko.md) FD8)의 대상 해석을 통과한다. 이 사이의 동작은 World가 정한다. 변경 API는 `Destroyed`로 거부한다(지금 World API와 같다 — `SetOwner`·`AddComponent`·Tick 의존 등). 읽기(`Const`) 함수는 마지막 값을 돌려준다(SL 10.2절 "PendingDestroy 대상에서의 동작"의 "읽기 허용"). `ROCKS_FUNCTION`으로 노출하는 변경 함수는 노출할 때 이 거부를 시험한다.
- Level의 `actors_`(`Level::Actors()`)는 순서를 보장하지 않는다. Actor 레코드가 배열 위치를 기억하고, 파괴는 `RemoveReferenceAtSwap`으로 끝 원소와 바꿔 O(1)로 뗀 뒤 옮긴 Actor의 위치를 고친다. 위치와 배열이 어긋나면 Fatal이다(실패를 버리지 않는다). 측정(실험 16절): Actor 하나의 파괴가 Actor 10K → 100K에서 5.0 → 43.7us이던 것이 0.5–1.0us로 같다.
- 꺼진(`enabled`가 거짓) 채 시작을 기다리는 Actor는 시작 큐에서 빼고 `startDeferred`로 표시한다. 활성화(`EnableActor`)가 표시를 보고 큐에 다시 넣는다(hook 안 활성화 포함 — 큐 용량은 Spawn에서 Actor마다 확보했다). 프레임마다 꺼진 Actor를 다시 훑지 않는다. 시작 순서는 다시 넣은 순서다.
- `DestroyLevel`은 그 Level의 `actors_`만 훑는다(World 전체 Actor가 아니라 — 작은 Level 파괴 21 → 716us이던 것이 1.3–1.5us).
- `DestroyLevel`(World 종료·travel 포함)은 Level의 `levelScript`를 먼저 처리한다: 간선을 끊고 LevelScript를 다른 Actor보다 먼저 파괴 대기에 넣어 프레임 끝 정리에서 LevelScript의 Disable·Destroy가 그 Level의 다른 Actor보다 먼저 실행된다([LevelScript](../LevelScript.ko.md) LD8). LevelScript는 `DestroyActor`로 파괴할 수 없다(`InvalidArgument`, 진단 `LevelScriptDestroyRejected` — LD16). 간선을 끊고 파괴하는 길은 `DestroyLevel`과 편집 World의 `SetLevelScript`, 엔진 내부 예외(OnAwake 실패, 우회 파괴 복구 — 계약 위반으로 Error 기록, 핫 리로드 R3 재생성)뿐이고 모두 간선을 먼저 끊으므로, 강한 간선이 파괴 요청된 Actor를 붙잡아 회수되지 않는 일(7.4절)이 없다. fault 격리는 파괴가 아니므로 간선을 유지한다. `SpawnActor`는 LevelScript 파생 클래스를 `InvalidArgument`(`LevelScriptSpawnRejected`)로 거부한다(레벨당 0 또는 1 — LD5).
- `SpawnActor`는 `SpawnActorDeferred` + `FinishSpawning`이다. 같은 프레임에 FinishSpawning한 Actor들의 `OnAwake`가 모두 끝난 뒤에 `OnStart`를 시작한다.
- 의존성 준비(`IsReadyToStart`)는 매 프레임 확인하며 기다리느라 게임 스레드를 막지 않는다. 비동기 에셋 로딩은 이 지점에 연결한다.
- `OnAwake`가 실패하면 그 Actor는 즉시 파괴 경로로 들어가고 `FinishSpawning`이 오류를 반환한다.

### 7.3 Component

- `Component`는 Actor에 속하고 `OnRegister`/`OnUnregister`(엔진 내부·backend 등록)와 위 hook을 가진다. Actor의 `FinishSpawning`이 Component를 등록한다(OnRegister → Actor OnAwake → Component OnAwake → OnEnable). 이미 초기화된 Actor에 추가된 Component는 추가 즉시 OnRegister·OnAwake·OnEnable을 받고 OnStart는 다음 프레임 시작에 받는다(다른 객체의 hook 실행 중 OnStart 재진입 방지). 정리는 OnDisable(Component 역순 → Actor) → OnDestroy(Component 역순 → Actor) → OnUnregister 순이며, OnDestroy는 OnAwake가 성공한 객체만 받는다.
- `SceneComponent`는 부모·자식 계층, 상대 Transform, 월드 행렬(`Mat4d` affine)을 가진다. 좌표·단위는 UE 5.8과 같다(왼손 +X 전방·+Y 오른쪽·+Z 위, 위치 cm, 회전 단위 quaternion — 도 단위 `Rotator`는 `ToQuaternion`으로 바꾼다, scale 무차원). 월드 행렬 = 상대 TRS 행렬 × 부모 월드 행렬(행 벡터 — UE `NewRelativeTransform * ParentToWorld`, 2026-11-17 전에는 column vector의 부모 × 상대)이며 dirty 플래그로 지연 계산한다. 행렬로 저장하므로 비균일 scale 부모 아래 회전한 자식(shear)도 정확하고, TRS가 필요할 때만 `TryWorldTransform`이 분해한다(shear면 Unsupported). 상대 Transform·부모 변경 시 하위 전체를 dirty로 표시한다. `AttachTo`는 순환(자기 자신·조상)과 장부 밖 객체를 `InvalidArgument`로 거부하며 같은 World 안에서만 붙인다. 부모가 파괴되면 자식은 루트가 된다. 영속 상태는 상대 Transform과 부모(`attachParent`)이고 자식 목록·월드 행렬은 저장하지 않는다(4.6절 — 편입이 다시 만든다, 29절). prefab 추적 인스턴스에서는 부착 부모 없는 최상위 root의 부모 행렬 자리에 인스턴스 앵커 행렬이 온다([Prefab](../Prefab/04-StoragePropagationRuntime.ko.md) 3.10절 D10 — PF5에서 도입).
- **보강 예정(설계만, 미구현).** UE 5.8 엔진 컴포넌트를 들이기 전에 이 절을 넓힌다. 요구의 정본은 [컴포넌트 도입](../ComponentCatalog/04-Prerequisites.ko.md) 4.1절이고 착수는 그 문서 CM0(화면 마일스톤 첫 단계)이다.
  - CR1 시스템 상태 수명: 등록 안에서 렌더·물리·오디오 상태를 만들고 지우는 hook과 "다시 보내기" 표시(UE `CreateRenderState`·`MarkRenderStateDirty`·`CreatePhysicsState` 대응), 등록 실패 시 부분 등록 해제, 대량 등록 묶음. 지금은 `OnRegister`·`OnUnregister` 하나뿐이다.
  - CR2 SceneComponent: 부착 규칙(KeepRelative·KeepWorld·SnapToTarget), socket 부착과 socket Transform, 절대 location·rotation·scale, Mobility(Static·Stationary·Movable — Static 이동은 진단과 함께 거부), 가시성(자식 전파·게임 중 숨김), 속도, bounds 계산과 갱신, Transform 변경 통지(구독형 — 이벤트 slot 표).
  - 그 밖에 Component 시작 준비 hook(CR4 — SE-W2), EditorOnly 컴포넌트와 태그(CR5), 컴포넌트 타입 규칙(CR6)이다.

### 7.4 지연 작업과 안전 지점

- Tick 순회 중 파괴·Tick 등록 변경·Component 제거는 큐에 넣는다. 안전 지점은 **각 phase 경계**(Tick 등록 변경 반영)와 **World Tick 끝**(파괴 완료)이다.
- 같은 객체의 중복 파괴 요청은 성공으로 처리하고 한 번만 실행한다. 큐에 남은 핸들이 이미 무효이면 건너뛴다.
- World Tick 끝에 GC 일정(`WorldDesc::gcSchedule`)에 따라 파괴된 객체 저장소를 회수한다. 기본은 제한된 예산의 증분 GC이고, `RequestFullCollection`(레벨 전환·로딩·메모리 압박)이나 `WorldGcMode::FullCollection`(주기)은 STW 병렬 전체 수집 뒤 회수를 프레임마다 시간 예산으로 나눈다. 표식 병렬 scheduler는 엔진이 소유해 World에 빌려준다(GC 설계 17.3·17.4절).
- World API 밖에서 파괴(`RequestDestroy`·`Object::Destroy`)된 Actor·Component·Level은 **매 프레임 GC step 전**(World Tick 끝, 파괴 정리 뒤 — `RepairBypassedObjects`)에 Registry의 파괴 요청 목록(`ObjectRegistry::TakeDestroyRequests` — 지난 호출 뒤 DestroyRequested가 된 객체, GC 설계 14절)에서 찾아 World 파괴로 돌린다. Actor는 `DestroyActor`, Component는 파괴 큐(정리 단계가 소유 Actor 목록·Tick 등록에서 뗀다), Level은 `DestroyLevel`(소속 Actor 전부 정리 후 회수)이다. 영구 Level의 우회 파괴는 복구할 수 없는 계약 위반이라 Fatal이다(World API로도 파괴할 수 없고 Spawn 기본 대상이다). 목록의 객체가 World 레코드·색인에 없거나 이미 PendingDestroy면(World가 요청한 파괴) 건너뛴다. 가려내기는 모두 색인으로 한다: Actor는 Actor 레코드 색인, Level은 슬롯 번호 집합(`levelSlots_`), Component는 Component 색인(`componentRecords_` — 슬롯 번호 → Component handle·소유 Actor·Tick handle 사본, AddComponent가 넣고 World 정리가 회수 요청 직전에 뺀다). Component 색인은 정리 단계가 우회 파괴된 Component(Borrow로 읽을 수 없다)의 소유 Actor와 Tick 등록을 찾는 데도 쓴다 — 처음 구현은 그때 모든 Actor를 Borrow로 훑었다(Actor 수 × Borrow). 비용은 그 사이 파괴 수에 비례하고(Actor 수가 아니다) 목록이 비면 잠금 한 번이다. 복구한 수는 `WorldStats::repairedBypasses`다.
  - Component 단독 우회 파괴는 처음 구현에서 복구되지 않았다(리뷰 지적, 시험 `World_BypassedComponentIsRepaired`로 확인 — 소유 Actor 목록·Actor 수에 남고 회수되지 않았다).
  - 복구가 늦어도 안전한 이유: World 객체의 회수는 World 루트(Level 레코드)·Level 배열·Actor의 `components` 간선에서 끊긴 뒤에만 일어나고(DestroyRequested여도 강한 간선이 붙잡으면 회수되지 않는다), 그 간선을 끊는 것은 World 정리뿐이다. 그래서 우회 파괴가 언제 일어나든(Tick 밖, Tick hook·OnDestroy hook 안 포함) 다음 복구 전에 회수되지 않는다. 매 프레임인 이유는 cycle 시작 전에만 보던 처음 방식이 World 쪽 파괴가 없으면 GC가 돌지 않아 우회 파괴된 Actor·Level을 영영 정리하지 않았기 때문이다(2026-11-18 리뷰 반영 시험 `World_BypassedLevelDestroyIsRepaired`가 드러냄 — Level 우회 파괴는 아예 복구하지 않았다). 앞 개정의 "cycle 도중 우회 파괴가 정리 없이 회수된다"는 근거는 틀렸다(리뷰 지적).
  - 처음 구현은 cycle마다 모든 Actor를 `InspectLifecycle`로 조회했다(cycle 시작 프레임 Actor 10K → 100K: 207 → 2,390us, 파괴 요청 목록으로 2–13us).
  - 비용(2026-11-19, `WorldBench` — GC 실험 23절): 우회 파괴 하나의 복구·정리(Registry `RequestDestroy` 200개 뒤 한 프레임)는 Actor 10K·100K 모두 Actor당 0.30–0.34us(World API `DestroyActor`는 0.42–0.51us)이고, 변화 없는 프레임은 0.3us(목록이 비면 잠금 한 번)다. Actor 수와 무관하다.
  - Level 배열에서 뗄 때(`DetachFromLevel`)는 `RemoveReferenceAtSwap`(4.5절)을 쓴다. 옮겨 오는 끝 Actor도 우회 파괴로 DestroyRequested일 수 있는데, 위치 교체 `SetReferenceAt`은 그 원소를 새로 쓰는 것이라 재검증에서 실패하고 그 실패를 버리면 지운 Actor가 배열에 좀비로 남았다(리뷰 지적). 이 경로의 실패와 Actor 레코드 위치·배열 불일치는 일어날 수 없으므로 Fatal이다. 단 공개 API로 닿는 실패 하나가 있다: Level을 `PinForEncode`로 고정한 채 Tick하면 그 프레임의 Actor 정리에서 떼기가 Busy다. World 계약은 Borrow와 같이 encode 고정도 Tick·Shutdown 전에 반환하는 것이고(World.h 소유·수명 규약), 위반하면 그 계약을 밝히는 메시지로 Fatal이다(떼기 시점의 Busy로만 판정 — 매 프레임 Level을 훑지 않는다). 미뤄 두는 대기 목록은 버렸다: 고정이 풀릴 때 알 길이 없어 매 프레임 다시 시도해야 하고, 그동안 Level 배열과 Actor 레코드가 어긋난 채 남는다. 예외는 Level 객체 자체가 우회 파괴돼(DestroyRequested — Registry가 빌려 주지 않는다) 복구 전인 경우다. 그 배열은 Level과 함께 사라지므로 뗄 것이 없고, 복구의 `DestroyLevel`은 Level 배열 대신 World의 Actor 레코드에서 소속 Actor를 찾는다(이 계약 위반의 복구에서만 Actor 수에 비례).

### 7.5 소스 파일 구성

`World` 클래스 하나의 정의를 기능별 파일로 나눈다(`Engine/Source/Runtime/World/Private/`, 2026-11-18 — 한 파일 약 2,000줄을 동작 불변 이동으로 나눔). 공개 선언은 `World/World.h` 하나이고, 파일 사이 공용 helper가 필요하면 `Private/` 내부 헤더에 둔다(지금은 없다).

| 파일 | 담당 | 새 기능을 넣는 곳 |
|---|---|---|
| `World.cpp` | 생성·소멸, `Initialize`·`BeginPlay`·`Shutdown`(Subsystem 초기화·종료 포함), 상태 이름·Tick 설정 검사·타입 등록, 레코드 조회(`FindActorRecord`·`Get*`·`HasActor`), `Stats` | World 수명 단계, 조회 API, 통계 |
| `WorldLevels.cpp` | Level 생성·파괴·회수(`CreateLevel`·`DestroyLevel`·`ReleaseLevel`·`FinishLevelDestroys`), Level 배열에서 떼기(`DetachFromLevel` — 바꿔 떼기) | Level 스트리밍·Level 단위 기능 |
| `WorldActors.cpp` | Actor Spawn(`SpawnActorDeferred`·`FinishSpawning`), `DestroyActor` 요청, 클러스터 가입 조건(`JoinsCluster`) | Actor 생성 방식·Spawn 인자 |
| `WorldComponents.cpp` | Component 추가·진행(`AddComponent`·`AdvanceComponent`)·`DestroyComponent` 요청 | Component 종류별 등록 |
| `WorldActivation.cpp` | 활성·Tick 활성 전환(`SetActorEnabled`·`EnableActor` 등, 시작 큐 재등록) | 활성 정책 |
| `WorldStarts.cpp` | 시작 큐와 `OnStart` 처리(`ProcessStarts`·`StartComponent`) | 시작 순서·준비 조건 |
| `WorldTicks.cpp` | Tick 등록·해제(`RegisterTicks` 등)와 프레임 실행(`ExecuteTick`·`Tick`) | 프레임 단계·Tick 연동 |
| `WorldDestroys.cpp` | 안전 지점의 파괴 정리(`FlushDestroys`·`DestroyActorNow`·`DestroyComponentNow`·root 비우기) | 파괴 hook·정리 순서 |
| `WorldTickDependencies.cpp` | Tick 의존 선언 그래프(`AddTickPrerequisite`·`RemoveTickPrerequisite`, 순환 검사, 등록 때 적용, 파괴 정리), 부착 간선, 실행 중 설정 변경(`SetTickPhase` 등), 엔진 단계 자리(`SetEngineStage`·`RunEngineStage`) — 15절 | Tick 의존·엔진 단계 |
| `WorldLevelAdoption.cpp` | 로드한 Level 그래프 편입(29장) | 편입 규칙 |
| `WorldTags.cpp` | 태그 값 도우미(`world_tags` — Actor·Component setter가 쓴다)와 태그 찾기(`FindActorsWithTag`·`FindComponentsByTag`, 31장) | 태그 규칙·찾기 |
| `WorldGarbageCollection.cpp` | GC step(`StepGarbageCollection`), 소유 객체 파괴 요청, 우회 파괴 복구(`RepairBypassedObjects`·Component 색인) | GC 일정·Registry 연동 |

- 파일은 수백 줄 안쪽을 유지한다. 한 파일이 천 줄 가까이 커지면 기능 경계로 다시 나눈다(나눔은 동작 불변 이동 커밋).
- `TickScheduler.cpp`(약 590줄)는 한 클래스의 한 기능이라 나누지 않는다.
