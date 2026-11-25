# 엔진 런타임 — 8. Tick Scheduler

[목차](README.ko.md)

## 8. Tick Scheduler

- **UE 단계 대응(2026-11-23 결정, 권장안 — [컴포넌트 도입](../ComponentCatalog/04-Prerequisites.ko.md) CR8a·CD11).** 단계를 더하지 않는다. UE `ETickingGroup`(`Engine/Classes/Engine/EngineBaseTypes.h:86–104`)과 계획 20.2절의 옛 그룹 이름(PhysicsSync·AnimationFinalize)은 아래처럼 대응한다. UE 경로는 `Engine/Source/Runtime/Engine/` 기준이다.

  | Rocks | UE 5.8 | 근거·규칙 |
  |---|---|---|
  | PrePhysics | `TG_PrePhysics` | Actor 기본(`Private/Actor.cpp:276`), skeletal mesh 기본(`Private/Components/SkeletalMeshComponent.cpp:441`). UE 컴포넌트 기본은 `TG_DuringPhysics`(`Private/Components/ActorComponent.cpp:548`)이나 Rocks에는 그 단계가 없으므로 컴포넌트 기본도 PrePhysics다. DuringPhysics 항목은 그 프레임 물리 결과를 보지 못하므로 PrePhysics와 관찰 결과가 같다 |
  | Physics(고정 스텝) | `TG_StartPhysics`·`TG_EndPhysics`(UE도 Hidden — 엔진 전용) | 사용자 등록 단계는 그대로이고, phase 끝에 엔진 단계 둘을 이 순서로 둔다. (1) **물리 결과 반영**(옛 PhysicsSync): body transform을 컴포넌트에 쓰고 고정 스텝 동안 쌓인 충돌·trigger 통지를 스텝 순서대로 발행한다. 고정 스텝이 여러 번이어도 반영·발행은 프레임에 한 번이다(UE `FinishPhysicsSim` — `Private/PhysicsEngine/PhysLevel.cpp:170, 225–234`, 충돌 통지가 EndPhysics에서 나간다 — `Private/LevelTick.cpp:1767`). (2) **skeletal mesh 물리 마무리**(옛 AnimationFinalize): ragdoll·물리 blend 결과를 본 transform에 반영한다(UE `EndPhysicsTickFunction` = `TG_EndPhysics` — `SkeletalMeshComponent.cpp:452`, `Private/SkeletalMeshComponentPhysics.cpp:3419`) |
  | PostPhysics | `TG_PostPhysics` | 물리 결과가 필요한 항목(UE spring arm — `Private/GameFramework/SpringArmComponent.cpp:22`) |
  | LateUpdate | 카메라 갱신 + `TG_PostUpdateWork` | UE는 PostPhysics 뒤 타이머·tickable → 카메라 갱신 → PostUpdateWork 순이다(`Private/LevelTick.cpp:1835, 1877`). LateUpdate 시작에 엔진의 카메라 갱신(PlayerController 카메라 관리, S12)을 두고 등록 항목은 그 뒤에 실행한다 |
  | PreRender | end-of-frame 갱신(렌더 상태 전송, `Private/LevelTick.cpp:1826`) | 렌더 추출 준비 |
  | (없음) | `TG_LastDemotable`·`TG_NewlySpawned` | 같은 프레임에 새로 생긴 항목은 시작 큐 규칙(7.2절)대로 다음 프레임이다 |

  - 애니메이션 pose 평가는 skeletal mesh의 PrePhysics Tick에서 한다. 병렬 평가(병렬 Tick과 함께, 후속)를 들이면 물리 body가 있는 mesh는 PrePhysics 끝에서 합류하고, 없는 mesh는 PostPhysics 끝까지 합류를 늦출 수 있다(UE end tick group — `SkeletalMeshComponent.cpp:2084–2086`).
  - cloth는 스펙이 없다. UE는 PrePhysics에서 시작해 PostPhysics에서 끝나는 병렬 작업이다(`SkeletalMeshComponent.cpp:457–458`). cloth 스펙이 같은 자리로 정한다.
  - UE DuringPhysics의 "물리와 병렬" 등록 종류는 물리를 비동기(Jolt job)로 돌리고 병렬 Tick을 들일 때 다시 본다.
- 등록 기반이다. 등록 단위는 `(대상 핸들, 종류, hook, phase, priority, interval, pause 정책)`이며 `TickHandle`(slot+generation)로 식별한다.
- 단계 순서(열거 순서 = 실행 순서)는 `PrePhysics → Physics(고정 스텝 수만큼) → PostPhysics → LateUpdate → PreRender`다. LateUpdate(카메라 추적 등 후처리)가 PreRender(렌더 추출 준비)보다 먼저 와야 같은 프레임 렌더에 반영된다.
- phase마다 밀집 배열을 두고, 등록 집합이 바뀐 phase만 다시 정렬한다. 정렬은 (priority, 등록 순서) 최소 힙을 쓰는 위상 정렬이다. 선행 의존으로 묶이지 않은 항목끼리는 항상 (priority, 등록 순서)를 따른다. 순회 중에는 할당하지 않는다(실행 중 해제도 표시만 하고 단계 경계에서 지운다).
- 순서 배열은 흔한 변경에서 다시 만들지 않고 고친다(2026-11-18 — 재구성이 모든 틱에 비례해 틱 Actor 하나를 만든 프레임마다 Actor 10K에서 1.6ms, 100K에서 22ms가 더 들었다). 결과 순서는 처음부터 재구성한 순서와 같다:
  - 새 등록은 선행이 없고 등록 순서가 가장 늦다. priority가 그 단계 순서 배열의 최댓값 이상이면 재구성해도 맨 끝이므로 끝에 붙인다. 실행 중에는 배열을 바꾸지 않는다(다음 경계의 재구성).
  - 해제는 남은 항목의 순서를 바꾸지 않으므로 순서 배열에 남겨 실행이 건너뛴다(세대가 다른 handle). 남은 칸이 배열의 절반을 넘거나, 해제한 항목을 선행으로 둔 항목이 있으면(항목마다 `dependents` 계수) 재구성한다 — 풀린 의존이 순서를 바꾸기 때문이다.
  - 선행 추가는 지금 순서에서 이미 선행이 앞이면 재구성하지 않는다(그 간선이 막는 선택은 원래 순서에서도 일어나지 않았다). 선행 제거는 재구성한다.
  - 사이클 검사(`Reaches`)는 호출 번호 표시로 방문한 칸만 본다(호출마다 표시 배열을 비우지 않는다 — 번호가 한 바퀴 돌 때만 비운다). 실행 중 미룬 해제는 목록으로 지운다(전 항목을 훑지 않는다 — 목록 용량은 등록 때 확보).
  - 한계: priority가 최댓값보다 낮은 등록과 선행 제거는 지금처럼 단계 전체를 다시 만든다.
- 선행 의존은 같은 phase 안에서만 허용한다. 사이클은 `AddPrerequisite` 호출 시점에 방문 표시 탐색(선형 시간)으로 검출해 거부한다. 다른 phase 간 순서는 phase 순서로 보장된다.
- Component Tick은 같은 hook의 소유 Actor Tick 뒤에 오도록 World가 선행 의존을 자동으로 건다. **S7b에서 바뀌었다(ERD1, 15.3절)**: UE에는 Actor → Component 기본 의존이 없으므로 이 간선을 없애고, SceneComponent 부착 관계(자식 → 부모) 간선을 건다. 같은 priority면 Actor Tick이 먼저 등록되므로 지금처럼 Actor가 먼저다.
- interval은 대상별 누적으로 판정하고 상대 허용오차(1e-9)로 합산 오차를 흡수한다. 남은 누적은 다음 주기로 이월해 평균 주기를 interval에 맞추되 한 번에 여러 주기를 따라잡지 않는다. delta는 마지막 실행 이후 흐른 시간이다.
- pause 정책: 기본은 World pause 시 정지, `TickWhenPaused`는 unscaled delta로 계속 실행한다. 디버그 단일 스텝 프레임은 pause가 아닌 것처럼 실행한다.
- 병렬 Tick은 후속이다. 등록 단위가 읽기·쓰기 선언을 추가할 수 있도록 구조를 남긴다.
