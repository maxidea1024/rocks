# 엔진 런타임 — 29. 레벨 편입

[목차](README.ko.md)

## 29. 레벨 편입

설계일 2026-11-25. [4절](04-ObjectSystem.ko.md) "레벨 편입 계약"의 상세 설계다. 저장한 Level 그래프(graph v2)를 다시 읽어 World의 장부에 넣고 수명 단계로 진행한다. UE의 레벨 로드(`ULevel` 직렬화 뒤 `UWorld::AddToWorld` → `ULevel::InitializeNetworkActors`·`RouteActorInitialize`)에 해당한다. 레벨 패키지 형식(에셋 시스템)과 streaming(CR22)은 이 장의 범위가 아니다. 이 장은 World 쪽 API만 다룬다.

### 29.1 경계

- **로드는 호출자가 한다.** 호출자는 레벨 패키지 단계나 시험이다. `LoadObjectGraph(allocator, world.Objects(), types, world.Access(), bytes)`로 World registry에 직접 읽는다.
  - 4절의 옛 문장("편입 전 로드 객체는 별도 ObjectRegistry에")은 이 장이 대신한다. registry 사이에는 객체를 옮길 수 없다. 별도 registry에서 검증하려면 같은 byte를 한 번 더 읽어야 한다.
  - 편입 전 객체는 장부 밖이다(4절 규칙). World API는 그 객체에 `InvalidArgument`·`StaleHandle`을 돌려주고, Tick·hook을 받지 않는다. 호출자의 `RootRef`가 Level을 GC에서 지킨다.
  - World 모듈은 ObjectSerialization에 의존하지 않는다(3절 의존 방향).
- **편입:** `World::AdoptLevel(RootRef<Object>&& loaded) → Result<LevelAdoption>`.
  - `LevelAdoption { ObjectHandle level; uint32 actors; uint32 failedActors; uint32 droppedEntries; }`
    - `actors`는 장부에 넣은 Actor 수다.
    - `failedActors`는 FinishSpawning이 실패한 Actor 수다(그 Actor는 Spawn 실패와 같이 정리된다).
    - `droppedEntries`는 정리한 null·잘못된 원소 수다.
  - 성공하면 `loaded`의 소유가 World의 Level 레코드로 넘어간다. 실패하면 호출자의 `RootRef`는 놓인다. 검증 실패면 객체는 그대로 장부 밖에 남고 GC가 회수한다.

### 29.2 순서(4절 (1)–(6))

1. **전제:**
   - World가 바꿀 수 있는 상태다(`CanMutate` — Created면 `InvalidArgument`, 종료 중이면 `ShuttingDown`). hook 안이면 `Busy`다(Spawn과 같다).
   - 루트가 이 registry의 `Level`이 아니면 `TypeMismatch`다.
   - 이미 장부에 있는 Level(`world_` 설정·레코드 있음)이면 `AlreadyExists`다.
2. **검증(바꾸지 않는다).** 하나라도 어기면 `InvalidArgument`이고 아무것도 바꾸지 않는다.
   - Actor·Component는 장부 밖이어야 한다(`world_`가 null). 한 Actor가 Level 배열에 두 번 있거나 한 Component가 두 번(두 Actor 또는 한 Actor에) 있으면 안 된다.
   - SceneComponent의 `relativeTransform`은 `SetRelativeTransform` 규칙(유한, 단위 quaternion — `Transform::IsValid`)을 지켜야 한다.
   - 부착 부모(`attachParent` — weak)를 확인한다.
     - 부모가 이 Level의 다른 SceneComponent가 아니면 그 부착을 끊는다(null로 정리). 그래프 밖 weak가 null이 되는 것과 같은 규칙이다.
     - Static 자식이 Static이 아닌 부모에 붙어 있어도 끊는다(UE `AttachToComponent`가 거부하는 조합 — 등록 뒤 `AttachTo`와 같은 규칙).
     - 부모 없이 socket 이름만 남은 손상 데이터는 socket을 비운다(세지 않는다).
     - 조상 사슬에 순환이 있거나 `MaxHierarchyDepth`를 넘으면 `InvalidArgument`다.
3. **정리.** 장부·타입 규칙이 끝난 뒤(4) 아래 원소를 Registry 참조 API로 정리한다(`droppedEntries`에 센다). 순서를 지키는 제거라 정리 뒤 배열 순서가 검증 순서와 같다(그래서 `levelPosition`·spawnOrder·자식 순서가 모두 배열 순서다).
   - Level `actors`와 Actor `components`의 null 원소를 뗀다.
   - Actor `rootComponent`가 `components` 밖이면 비운다.
4. **장부(4절 (1)·(2)) — 정리(3)보다 먼저 한다.**
   - Level 레코드를 만든다. `world_`를 연결하고, persistent는 아니며, 클러스터에 넣지 않는다(로드한 Level은 클러스터 루트가 아니다 — 13.3절 GC 클러스터는 레벨 패키지 단계에서).
   - Actor마다 `ActorRecord`를 만든다.
     - 상태 Constructed, `levelPosition`은 정리 뒤 배열 자리(null을 뺀 검증 순서), spawnOrder는 배열 순서다. Spawn Transform은 없다(저장한 root 상대 값을 쓴다).
     - `world_`·`level_`을 연결하고 색인에 넣는다.
   - Component마다 `ComponentRecord`를 만든다. `world_`·소유 Actor를 연결한다.
   - Actor의 Component가 다 들어간 뒤 Component마다 타입 규칙(23.2절 `CheckComponentRules` — EditorOnly 포함)을 본다. 어기면 편입 전체가 그 오류다.
   - 이 단계의 실패(타입 규칙·할당)는 만든 레코드·색인·Level 레코드를 직접 되돌린다. 정리(3) 전이라 그래프는 그대로이고, 아무것도 바뀌지 않은 채 오류다. `DestroyLevel`은 쓰지 않는다(Level 배열에 레코드 없는 Actor가 남으면 파괴 경로가 Fatal이다).
   - 정리(3)의 Registry 참조 API는 관찰자 없는 World registry에서 할당하지 않아 실패하지 않는다. 실패하면 장부와 그래프가 어긋나므로 Fatal이다.
5. **부착 재구성(4절 (3)).** 정리된 `attachParent`에서 NATIVE `parent_`·부모 `children_`을 만든다.
   - 순서는 Level 배열 → Actor `components` 순이다.
   - 부착 Tick 간선은 `AttachTo`와 같은 `UpdateAttachmentTicks`로 건다(시작 전 선언 — 15절).
   - socket 이름은 저장 값 그대로다.
   - 월드 행렬은 dirty로 둔다(처음 읽을 때 계산한다).
6. **수명(4절 (6)).** Level 배열 순서로 Actor마다 `FinishSpawning`을 한다. 앞 Actor의 hook이 뒤 Actor를 파괴하거나 직접 진행했으면(레코드 없음·Constructed 아님) 건너뛰고 실패로 세지 않는다. 순서는 등록 → PreAwake → Awake → Enable이고, 시작은 시작 큐다(17절).
   - 실패한 Actor는 Spawn 실패와 같이 정리된다. 편입은 계속하고 `failedActors`에 센다(UE 레벨 로드도 Actor 하나의 실패로 레벨을 버리지 않는다).
   - LevelScript Actor를 마지막에 하는 규칙([LevelScript](../LevelScript.ko.md) LD8)은 Level에 `levelScript` 필드가 생길 때 넣는다. 지금 Level에는 그 필드가 없다.

### 29.3 결정·한계

| ID | 항목 | 결정 | 근거·버린 대안 |
|---|---|---|---|
| ERD57 | 로드 위치 | 호출자가 World registry에 읽고 World는 편입만 | registry 사이에 객체를 옮길 수 없다. 별도 registry에서 복사하려면 범용 참조 재연결 복사가 필요하다(버림 — prefab 단계의 몫) |
| ERD58 | Actor 실패 | 그 Actor만 정리하고 계속 | UE 레벨 로드와 같다. 레벨 하나가 Actor 하나 때문에 버려지지 않는다 |
| ERD59 | 잘못된 부착 부모 | 끊는다(null) | 그래프 밖 weak가 null이 되는 규칙과 같다. 순환만 데이터 손상으로 거부한다 |

- 한계:
  - 편입한 Level은 GC 클러스터가 아니다.
  - Actor `owner`·`instigator`(NATIVE)는 저장되지 않으므로 비어 있다.
  - Tick 설정(NATIVE)은 클래스 기본값이다(4절 — A5에서 리플렉션으로).
- 시험(`LevelAdoption_` 접두 — 손상 그래프는 `Private/LevelGraphTestAccess.h`로 만든다):
  - 저장 → 같은 World registry로 로드 → 편입: 레코드·소유·부착·수명이 Spawn한 것과 같다(상태, `GetComponents`, `RootComponent`, 부모·자식, 월드 위치, 부모를 옮기면 자식이 따라감, 엔진 Actor root 재사용, BeginPlay·DestroyLevel·Shutdown).
  - 정리: Level null·Component null·root 밖을 순서를 지키며 정리(`droppedEntries` 3), 앞 Actor 파괴 뒤 바꿔 떼기 위치가 맞음.
  - 부착: 다른 Actor Component에 붙은 부착 재구성(socket 유지), Level 밖 부모 끊기, Static 자식·Movable 부모 끊기, 부모 없는 socket 비우기.
  - 거부(아무것도 바뀌지 않음): 중복 Actor, 중복 Component, 잘못된 Transform, 부착 순환, Level이 아닌 루트, 장부 안 Level, 두 번 편입, Created World.
  - Actor 하나의 실패(PreAwake가 거부하는 root)를 `failedActors`로 센다. Playing 중 편입한 Actor는 다음 Tick에 시작한다.
  - 미시험: 타입 규칙 위반의 되돌림 경로. 저장할 수 있는(리플렉션) Component 가운데 타입 규칙·EditorOnly를 가진 것이 아직 없다. 그런 Component가 생기면 더한다.
