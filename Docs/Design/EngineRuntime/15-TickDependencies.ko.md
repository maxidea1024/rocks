# 엔진 런타임 — 15. Tick 의존성 완성(S7)

[목차](README.ko.md)

## 15. Tick 의존성 완성(S7)

8절 TickScheduler 위에 UE `FTickFunction` 의존 계약을 채운다. 설계일 2026-11-23. UE 경로 약어: `E/` = `Engine/Source/Runtime/Engine/`.

### 15.1 지금 있는 것과 빈 곳

| 있음 | 없음 |
|---|---|
| 같은 단계 안 DAG, 등록 때 순환 거부(`CycleDetected`) | Component·Fixed·Late hook 사이 의존 API, 의존 제거 |
| Actor 주 Tick(OnTick) 사이 의존(`AddActorTickPrerequisite`, 둘 다 Playing일 때만) | 시작 전(Spawn 직후) 선언, 시작 뒤 단계·priority·interval 변경 |
| Component Tick이 소유 Actor Tick 뒤(자동) | 단계를 넘는 의존, 부착 관계 의존, 엔진 단계 자리(Physics 끝·LateUpdate 시작) |

### 15.2 UE 5.8 계약

- `AddPrerequisite`는 양쪽이 tick 가능할 때만 간선을 더한다(`E/Private/TickTaskManager.cpp:2481–2495`, Actor·Component 래퍼 `E/Private/Actor.cpp:841–855`, `E/Private/Components/ActorComponent.cpp:2617, 2625`). 등록 전에도 선언할 수 있고 등록 때 그대로 쓴다.
- 실행 큐잉은 선행을 깊이 우선으로 먼저 넣는다. 등록 안 된·꺼진·cooldown 중인 선행은 무시한다. 이번 프레임에 방문했지만 아직 큐에 없으면 순환으로 보고 Warning을 남긴 뒤 그 간선만 무시한다(`TickTaskManager.cpp:2622–2707`, 2655–2663).
- 선행이 더 늦은 tick group이면 의존하는 쪽을 그 그룹으로 늦춘다(demotion). 늦춘 결과가 `TG_StartPhysics`·`TG_EndPhysics`면 다음 그룹으로 더 민다(`TickTaskManager.cpp:2674–2684, 136–145`).
- Actor → Component 기본 의존은 **없다**(`ActorComponent.cpp:1739–1753, 1800–1818`). 대신 SceneComponent를 부착하면 자식 Tick에 부모 Tick 선행을 더하고 떼면 뺀다(`E/Private/Components/SceneComponent.cpp:2490–2491, 2702`).
- 실행 중 새로 등록된 Tick은 현재 그룹이 끝난 뒤 다음 그룹 문맥으로 실행한다(`TickTaskManager.cpp:1684–1690, 2133–2164`).
- `SetTickGroup`·`SetTickInterval`·우선순위는 실행 중에도 바꿀 수 있다.

### 15.3 Rocks 계약

**의존 API.** Tick 대상은 `TickTarget{object handle, hook}`이다. hook은 `Update`(OnTick)·`Fixed`(OnFixedTick)·`Late`(OnLateTick)다.

```cpp
// World 공개 API(S7). 대상은 Actor·Component 어느 쪽이든 된다.
Result<void> AddTickPrerequisite(TickTarget dependent, TickTarget prerequisite) noexcept;
Result<void> RemoveTickPrerequisite(TickTarget dependent, TickTarget prerequisite) noexcept;
Result<void> SetTickPhase(TickTarget target, TickPhase phase) noexcept;      // Update hook만
Result<void> SetTickPriority(ObjectHandle object, int32 priority) noexcept;
Result<void> SetTickInterval(ObjectHandle object, double seconds) noexcept;
```

- **시작 전 선언(ERD5).** Constructed·Initialized 객체에도 선언할 수 있다. World가 모든 선언을 **선언 그래프**(TickTarget 사이 간선, 등록 여부와 무관) 하나에 두고 호출 시점에 그 그래프에서 순환을 검사한다(ERD2와 같은 시점). 양쪽이 등록되는 시점(OnStart 직후 등록)에 TickScheduler 간선으로 옮긴다. 한쪽이 끝내 등록되지 않으면 간선은 효력이 없다(UE의 "등록 안 된 선행 무시"와 같다). 해당 hook을 TickSettings에서 켜지 않은 대상은 `InvalidArgument`다(UE `bCanEverTick` 검사).
- **꺼진 선행(ERD4).** 꺼졌거나(`SetTickEnabled(false)`) interval 대기 중인 선행은 실행만 건너뛰고 순서 간선은 남는다. 같은 단계 안은 순차 실행이라 UE의 "무시"와 관찰 결과가 같다. 단계를 넘는 경우는 다르다: UE는 꺼지거나 cooldown 중인 선행을 늦춤 계산에서도 뺀다(`TickTaskManager.cpp:2668–2674`). Rocks는 실효 단계를 프레임 시작에 정적으로 정하므로 선행이 꺼져 있어도 의존하는 쪽은 늦춰진 단계에 남는다(문서화된 차이 — 단계가 프레임마다 흔들리지 않는다).
- **단계를 넘는 의존(ERD3).** TickScheduler는 단계와 상관없이 모든 간선을 **한 그래프**에 둔다(지금의 같은 단계 제한을 없앤다). 순환 검사(`Reaches`)도 전체 그래프에서 한다. 항목마다 요청 단계(`TickSettings::phase`)와 **실효 단계**를 두고, 실효 단계 = max(요청 단계, 모든 선행의 실효 단계)를 전체 그래프 위상 순서로 계산한다(UE demotion). 선행이 더 이른 단계인 간선도 지우지 않는다 — 나중에 선행이 늦춰지거나 단계가 바뀌면 계산에 다시 쓰인다. 같은 실효 단계 안의 간선만 실행 순서 정렬에 쓰인다.
  - 반영 시점: 간선·요청 단계가 바뀌면 표시만 하고 **프레임의 첫 단계 실행 전**(`BeginFrame` 뒤 첫 `RunPhase`)에 실효 단계를 다시 계산해 항목을 단계 배열 사이로 옮긴다. 그 프레임의 첫 단계 전에 생긴 변경(시작 처리에서 등록된 항목의 선언 포함)은 그 프레임에, 단계가 하나라도 실행된 뒤의 변경은 다음 프레임에 반영된다. 단계 경계에서 옮기면 한 프레임에 두 번 실행되거나 빠질 수 있어서다.
  - Physics(고정 스텝)는 늦춤 대상이 아니다. Fixed hook(요청 단계 Physics)과 다른 hook 사이 간선은 고정 스텝 횟수가 달라 뜻이 없으므로 `InvalidArgument`다. 그래서 Update·Late hook의 실효 단계는 Physics가 될 수 없다(계산 결과가 Physics면 PostPhysics — UE도 Start/EndPhysics를 건너뛴다).
  - hook별 실효 단계: Fixed는 Physics만, Update·Late는 Physics를 뺀 어느 단계든 될 수 있다(예: LateUpdate 선행을 가진 Update hook은 LateUpdate에서, PreRender 선행을 가진 Late hook은 PreRender에서 실행). `IsValidTickSettings`는 사용자가 정한 요청 단계만 검사한다.
- **순환(ERD2).** 사용자 호출이 순환을 만들면 지금처럼 `CycleDetected`로 거부한다(UE는 실행 때 Warning 후 무시한다 — 원인 호출에서 실패하는 쪽이 진단이 쉽다). 엔진이 자동으로 거는 간선(부착·Controller→Pawn)이 순환을 만들면 그 간선만 걸지 않고 Warning을 남긴다(UE와 같은 결과).
- **자동 간선(ERD1).** Component Tick이 소유 Actor Tick 뒤에 오는 지금의 자동 간선을 **없앤다**. 8절은 이것이 "UE의 기본 순서와 같다"고 적었으나 UE에는 그런 기본이 없다(15.2절). 대신 UE처럼 SceneComponent 부착 때 자식의 각 hook에 부모의 같은 hook 선행을 걸고 떼면 뺀다. 같은 단계의 Actor·Component 순서는 (priority, 등록 순서)를 따른다. 시작 처리가 Actor Tick을 Component Tick보다 먼저 등록하므로(`WorldStarts.cpp:128`·`:142`) 같은 priority면 간선이 없어도 지금처럼 Actor가 먼저다. 달라지는 것은 Component가 더 작은 priority로 Actor보다 앞설 수 있게 된다는 점이다.
- **실행 중 변경(ERD6).** priority·interval 변경은 즉시 기록하고 그 단계의 다음 경계에서 반영한다(실행 중 배열을 바꾸지 않는 8절 규칙). 단계 변경은 단계를 넘는 간선과 같이 다음 프레임 시작에 반영한다. interval 변경은 누적을 0으로 되돌린다. Physics로 바꾸거나 Physics에서 다른 단계로 바꾸는 것은 `InvalidArgument`다(hook 종류가 바뀌는 것이라서).
- **실행 중 새 등록.** 지금처럼 다음 단계 경계에서 반영한다(UE NewlySpawned와 같은 결과 — 현재 단계가 이미 지났으면 다음 프레임).

### 15.4 엔진 단계 자리(ERD7)

사용자 등록이 아닌 엔진 단계를 단계 경계에 고정 순서로 둔다. World가 소유하는 `EngineStage` 콜백 표이며, 모듈(물리·애니메이션·Gameplay)이 World 생성 때 등록한다.

| 자리 | 순서 | UE 대응 |
|---|---|---|
| Physics 단계 끝(고정 스텝 뒤, 프레임에 한 번) | (1) `PhysicsResults` 물리 결과 반영 → (2) `SkeletalPhysicsFinalize` | `TG_EndPhysics`(8절 표) |
| LateUpdate 단계 시작 | (1) `Timers` 타이머·LifeSpan(17.5절) → (2) `TickableSubsystems`(16.5절) → (3) `CameraUpdate`(20.6절) | `LevelTick.cpp:1778–1883`: PostPhysics 뒤 타이머 → tickable → `UpdateCameraManager` → PostUpdateWork |

- 자리마다 등록은 최대 하나이고(중복은 `AlreadyExists`), 비어 있으면 건너뛴다. 콜백은 `Result`를 돌려주며 실패는 그 프레임 Tick 실패로 집계한다(6절 Tick 실패 요약).
- pause 중에는 `Timers`(게임 시계가 멈춤)·`TickableSubsystems`(16.5절)가 pause 정책을 따르고, `CameraUpdate`는 실행하지 않는다. UE는 pause 중 `!bIsPaused || ShouldPerformFullTickWhenPaused()`일 때만 `UpdateCameraManager`를 부른다(`LevelTick.cpp:1844–1851`, 기본 거짓). 디버그 단일 스텝 프레임은 pause가 아닌 것으로 본다(8절).

### 15.5 시험(완료 조건)

- hook 세 종류 × Actor·Component 조합의 의존 순서, 의존 제거 뒤 원래 순서 복귀(재구성 결과 = 처음부터 정렬한 결과).
- 시작 전 선언: Spawn 직후 선언 → 시작 뒤 순서 보장, 한쪽 미등록이면 무효, 시작 전 선언끼리의 순환을 호출 시점에 거부.
- 늦춤: PrePhysics 대상이 PostPhysics 선행을 가지면 다음 프레임부터 PostPhysics에서 그 뒤에 실행(같은 프레임에 두 번 실행·누락 없음), 선행 제거 뒤 다음 프레임에 PrePhysics로 복귀, 전이 늦춤, 늦춤 뒤 역방향 간선의 순환 거부(단계를 넘는 순환), Fixed↔Update 의존 거부, Update hook의 LateUpdate·Late hook의 PreRender 늦춤.
- 순환: 사용자 호출 거부, 자동 간선은 Warning과 함께 생략.
- 부착 의존: 부착·떼기·다시 붙이기, 부모 파괴 뒤 자식 실행.
- 실행 중 변경: 단계·priority·interval 변경이 다음 경계에 반영, 실행 중 배열 불변.
- 엔진 단계: 자리 순서, 중복 등록 거부, 실패 집계, pause 중 동작(Camera 생략, 단일 스텝 프레임은 실행).
- 8절의 기존 TickScheduler 시험은 그대로 통과한다(자동 Actor→Component 간선을 쓰던 시험은 ERD1에 맞게 고친다).
