# 엔진 런타임 — 17. Actor 수명 완성(S9)

[목차](README.ko.md)

## 17. Actor 수명 완성(S9)

7.2·7.3절 수명 위에 UE `AActor`의 남은 계약을 채운다. 설계일 2026-11-23. UE 경로 약어: `E/` = `Engine/Source/Runtime/Engine/`.

### 17.1 Rocks와 UE의 대응

| Rocks | UE | 비고 |
|---|---|---|
| `SpawnActorDeferred` | `SpawnActor(bDeferConstruction)` → `PostSpawnInitialize` | Owner·Instigator·Transform은 이때 정한다(17.3절) |
| `FinishSpawning`: Component OnRegister → (`OnFrameworkPreAwake`) → Actor `OnAwake` → Component `OnAwake` → (`OnFrameworkPostAwake`) → `OnEnable` | `FinishSpawning` → `PreInitializeComponents` → `InitializeComponents` → `PostInitializeComponents` | Actor `OnAwake`는 Component 초기화 **전**이라 UE `PreInitializeComponents` 쪽이다. UE `PostInitializeComponents` 자리는 모든 Component `OnAwake` 뒤의 `OnFrameworkPostAwake`다(17.8절) |
| `OnStart`(BeginPlay 전 Spawn은 `BeginPlay` 안, 뒤 Spawn은 다음 프레임 시작) | `DispatchBeginPlay`(BeginPlay 뒤 Spawn은 즉시) | 다음 프레임 시작은 7.2절 결정(다른 hook 실행 중 OnStart 재진입 방지)이며 유지한다 |
| 정리: `OnDisable` → `OnDestroy(reason)` → `OnUnregister` | `Destroyed` → `RouteEndPlay(reason)` → `EndPlay` → unregister | 17.2절 |

hook 이름과 Component·Actor 사이 호출 순서(World.h의 hook 순서)는 2절·계획 20.2절 결정(Unity 체계)이라 바꾸지 않는다.

### 17.2 EndPlay 사유(ERD13)

`OnDestroy`에 사유를 넘긴다: `virtual void OnDestroy(EndPlayReason reason) noexcept`(Actor·Component 모두). 사유는 UE `EEndPlayReason`(`E/Classes/Engine/EngineTypes.h:3670–3685`)과 같다.

| `EndPlayReason` | 언제 | UE |
|---|---|---|
| `Destroyed` | `DestroyActor`·`DestroyComponent`·LifeSpan 만료·우회 파괴 복구 | `Destroyed` |
| `LevelTransition` | World 전환(18.5절)으로 이전 World를 끝낼 때 | `LevelTransition` |
| `RemovedFromWorld` | `DestroyLevel`(Level 내림·스트리밍 아웃) | `RemovedFromWorld` |
| `Quit` | 앱 종료로 World를 끝낼 때 | `Quit` |
| `TryoutEnded` | Tryout 종료 | `EndPlayInEditor` |

- 사유는 정리를 시작한 쪽이 정하고, 파괴 대기 중에 더 큰 정리(Level·World)가 오면 먼저 정한 사유를 유지한다(UE도 첫 `RouteEndPlay`만 부른다).
- `OnStart`를 받지 못한 객체도 `OnAwake`가 성공했으면 `OnDestroy`를 받는다(7.3절 규칙 유지). UE는 BeginPlay 전이면 EndPlay를 건너뛴다 — Rocks는 `OnAwake`에서 얻은 자원을 돌려줄 자리를 보장하려고 지금 규칙을 유지한다(문서화된 차이).

### 17.3 Spawn 인자·Owner·Instigator(ERD14·ERD15)

`SpawnParams`를 넓힌다.

| 필드 | 뜻 | UE |
|---|---|---|
| `level` | 소속 Level(있음) | `OverrideLevel` |
| `transform`·`hasTransform` | `hasTransform`이면 FinishSpawning이 모든 Component `OnAwake` 뒤, `OnFrameworkPostAwake` 전에 root SceneComponent에 쓴다(UE도 `PostInitializeComponents` 전에 적용한다 — 자동 Possess가 Spawn 회전을 보도록, S12 리뷰). root의 월드 Transform = root 상대 Transform(생성자·`OnAwake`에서 정한 기본 offset) × Spawn Transform이다(UE `PostSpawnInitialize`의 `MultiplyWithRoot` — `E/Private/Actor.cpp:4303–4323`). root에 부모가 있으면 부모 월드의 역으로 상대 Transform을 구한다. `OnAwake`에서 만든 root도 받는다. root가 없으면 무시한다(UE도 root 없으면 위치가 없다). 유효하지 않은 Transform(비유한 성분·단위가 아닌 회전, `Transform::IsValid`)은 Spawn 전에 `InvalidArgument`, 합성 실패는 Actor를 파괴하고 Spawn 실패다. `SpawnParams::At(transform, level)` 도우미 | `SpawnTransform` |
| `owner` | 소유 Actor(`ObjectHandle`) | `Owner` |
| `instigator` | 원인 Pawn(`ObjectHandle`, 20절) | `Instigator` |

- `Actor::Owner()`·`SetOwner(handle)`: 소유 고리(자기 자신 포함)는 `InvalidArgument`와 Error 로그로 거부한다(UE `SetOwner` — `E/Private/Actor.cpp:2605–2630`). Owner가 파괴되면 handle은 generation 검사로 무효가 된다(4.2절). UE의 `Children` 배열은 두지 않는다(필요한 조회는 `ForEachActor`).
- Owner·Instigator는 게임 논리 참조이며 수명 소유가 아니다(소유 사슬은 4.2절 그대로). 없는 대상은 `StaleHandle`, 파괴 대기 대상은 `Destroyed`로 거부한다(`SetOwner`·`SetInstigator`와 Spawn 인자 모두). Spawn 인자 검사는 Actor를 만들기 전에 하므로 실패해도 hook·부작용(다른 대기 파괴의 정리 등)이 없다.
- Actor 이름(UE `FName` 고유 이름)은 객체 이름 체계(Core)가 정해질 때 다룬다. 진단은 타입 이름과 handle을 쓴다(22절).

### 17.4 Component 시작 준비(ERD18, CR4)

`Component::IsReadyToStart()`를 더한다(기본 참). 소유 Actor는 자신과 **켜진 모든 Component**가 준비됐을 때 시작한다(Actor 시작이 Component 시작을 묶으므로). 이미 Playing인 Actor에 나중에 추가한 Component(Actor `OnStart` 안에서 추가한 것 포함)는 자기 준비만 기다린다. 매 프레임 확인하고 기다리느라 막지 않는다(7.2절과 같다). 요구 출처는 Sparks 연동 SE-W2([컴포넌트 도입](../ComponentCatalog/04-Prerequisites.ko.md) CR4).

### 17.5 LifeSpan(ERD16)

- `Actor::SetLifeSpan(seconds)`: 게임 시간으로 seconds 뒤 `DestroyActor`(사유 `Destroyed`). 0 이하면 해제한다. 다시 부르면 새 값으로 바꾼다(UE `SetLifeSpan` — `E/Private/Actor.cpp:6577–6604`). 아직 시작하지 않은 Actor(Initialized World·준비 대기)에 건 값은 보관했다가 시작(Playing)할 때부터 센다(UE `InitialLifeSpan`을 `BeginPlay`에서 거는 것과 같다).
- 만료 판정은 LateUpdate 시작 엔진 단계 `Timers`(15.4절)에서 한다(UE 타이머가 PostPhysics 뒤에 돈다 — `LevelTick.cpp:1816`). World가 (만료 시각, 설정 순번) 순 힙으로 들어 만료 시각이 같으면 먼저 설정한 것부터 파괴한다. pause 중에는 게임 시계가 멈추므로 흐르지 않는다.
- 판정은 상대 허용오차 1e-9로 한다(게임 시간이 프레임 delta 합이라 같은 길이도 경로에 따라 아주 조금 다르다 — GameClock·TickScheduler interval과 같다). World의 만료 처리는 `Timers` 자리의 콜백보다 먼저다.
- 한계: 다시 설정·해제한 옛 힙 항목은 그 만료 시각까지 남는다(번호가 달라 무시). 같은 Actor가 긴 LifeSpan을 매 프레임 다시 설정하면 힙이 자란다 — 필요하면 Actor별 한 항목 갱신으로 바꾼다.
- 범용 타이머(UE `FTimerManager`)는 이 단계에서 만들지 않는다. 같은 `Timers` 자리에 나중에 붙인다.

### 17.6 World EndPlay 분리(ERD17)

`World::EndPlay(EndPlayReason)`를 더한다. Playing World에서만 뜻이 있고 두 번째 호출은 성공이다.

1. 상태를 `EndingPlay`(새 `WorldState` 값)로 바꾼다(새 `OnStart` 시작 금지 — Spawn은 된다, 종료 정리용 객체를 위해).
2. Level마다 `DestroyLevel`과 같은 규칙으로 정리한다: `levelScript` 간선을 먼저 끊고 LevelScript를 그 Level의 다른 Actor보다 먼저 파괴 대기에 넣은 뒤(LD8·LD16 — `DestroyActor`는 LevelScript를 거부하므로 이 길을 쓴다) 나머지 Actor를 그 사유로 넣고, 즉시 정리한다(프레임 밖이므로 안전 지점을 기다리지 않는다). Level 자체는 남긴다.
3. WorldSubsystem `OnWorldEndPlay`(역순).
4. 바깥 Subsystem 조회 창을 닫는다(16.3절 ERD10).
5. 상태를 `Initialized`로 돌린다(UE도 EndPlay 뒤 World는 정리 전까지 남는다). 끝난 World는 다시 `BeginPlay`하지 않는다(`InvalidArgument` — UE도 새 World를 만든다). 정리 hook·`OnWorldEndPlay`에서 Spawn한 Actor는 Initialized로 남아 `Shutdown`에서 정리되고, `OnWorldEndPlay`가 요청한 파괴는 EndPlay 안에서 끝낸다.

`Shutdown(reason = Quit)`은 Playing이면 먼저 `EndPlay(reason)`를 부르고 남은 Actor도 그 이유로 정리한다(그 뒤 `Shutdown`은 `OnWorldEndPlay`를 다시 부르지 않는다). LevelScript는 아직 코드에 없어(LevelScript 문서 — 설계만) 2단계의 LevelScript 먼저 규칙은 LevelScript 구현 때 붙인다. `Engine::DestroyWorld(world, reason)`은 `EndPlay(reason)` 뒤 `Shutdown`이다(미룬 파괴는 먼저 정한 이유를 유지). `Engine::DestroyWorld(world, reason)`은 사유를 받는다(기본 `Quit`).

### 17.7 파괴 시점 규칙

- `OnStart` 안의 `DestroyActor`는 지금처럼 프레임 끝 안전 지점에서 정리한다(UE도 BeginPlay 중 파괴를 BeginPlay 뒤로 미룬다 — `E/Private/LevelActor.cpp:893–900`). 그 프레임의 Tick에는 참여하지 않는다.
- `OnAwake` 실패·`OnAwake` 안 파괴는 시작하지 않는다(UE: PostInitializeComponents 안 파괴는 BeginPlay 생략 — `E/Private/Actor.cpp:4506–4508`).
- 같은 대상의 중복 파괴 요청은 성공이다(지금과 같다, UE `IsPendingKillPending` 재진입 무시).

### 17.8 프레임워크 내부 hook(ERD19)

UE는 `PreInitializeComponents`·`PostInitializeComponents`·`Destroyed` 같은 virtual에서 `Super` 호출에 기대 프레임워크 처리를 한다. Rocks 사용자 hook(`OnAwake`·`OnDestroy`)은 기본 구현이 비어 있어 `Super` 호출을 강제하지 않으므로, 프레임워크 클래스(GameDirector·GameState·PlayerState·Controller·Pawn·CameraDirector — 19·20절)용 내부 hook 셋을 Actor에 둔다.

| hook | 시점 | UE 대응 | 쓰는 곳 |
|---|---|---|---|
| `Result<void> OnFrameworkPreAwake()` | Component `OnRegister` 뒤, Actor `OnAwake` 전 | `PreInitializeComponents` | GameDirector의 GameState spawn |
| `Result<void> OnFrameworkPostAwake()` | 모든 Component `OnAwake` 뒤, `OnEnable` 전 | `PostInitializeComponents` | GameState·PlayerState 등록, Controller의 PlayerState·CameraDirector 생성, Pawn 자동 Possess |
| `void OnFrameworkEndPlay(EndPlayReason)` | `OnDisable` 전 | `Destroyed`·`EndPlay`의 엔진 부분 | Logout·UnPossess·등록 해제 |

- 실패는 `OnAwake` 실패와 같이 처리한다(Spawn 실패, OnAwake가 성공한 범위만 `OnDestroy`). `OnFrameworkEndPlay`는 `OnFrameworkPreAwake`가 성공한 Actor면 OnAwake가 실패해도 받는다(PreAwake에서 만든 것을 되돌릴 자리).
- Gameplay 모듈의 프레임워크 클래스만 재정의하고 `final`로 닫는다. 게임 코드는 사용자 hook만 쓴다. Sparks 노출 대상이 아니다.
- 스크립트 재정의([함수 리플렉션](../FunctionReflection.ko.md) FR3·FD13)는 사용자 hook(`OnAwake`·`OnStart`·`OnTick`·`OnEnd` 등)만 대상으로 한다. `ScriptExtensible` 클래스의 `virtual` 함수여야 하고, 프레임워크 hook과 `final`로 닫은 hook은 제외한다. FR3이 들어올 때 hook 선언에 `ScriptOverridable`을 붙인다.
- 이 hook은 World 모듈(S9)이 부르고, 쓰는 쪽은 S11·S12다.

### 17.9 시험(완료 조건)

- 사유 다섯 가지가 각 경로에서 정확히 전달, 먼저 정한 사유 유지.
- Spawn 인자: transform이 root에 반영(`OnAwake`에서 만든 root·기본 offset과 합성), 잘못된 transform 거부, owner 고리 거부, 파괴 대기 owner·instigator 거부(부작용 없음), instigator 보관, Owner 파괴 뒤 무효.
- Component 준비 대기: Actor 시작이 Component 준비까지 기다림, 나중 추가 Component(`OnStart` 안 추가 포함)는 따로.
- LifeSpan: 만료 프레임, 재설정·해제, pause 중 정지, 같은 프레임 다른 파괴와 겹침, 시작 전에 건 값은 시작부터 셈.
- World EndPlay: 순서(Actor 정리 → OnWorldEndPlay), 두 번 호출, EndPlay 뒤 Spawn, Shutdown이 EndPlay(Quit)를 먼저 부름.
- World EndPlay의 LevelScript 먼저 정리(LD8), LifeSpan 동률 순서.
- 프레임워크 hook 세 개의 시점(Component OnAwake 전·후, OnDisable 전)과 실패 처리.
- 기존 수명 시험은 `OnDestroy(reason)` 서명으로 고친 뒤 그대로 통과.
