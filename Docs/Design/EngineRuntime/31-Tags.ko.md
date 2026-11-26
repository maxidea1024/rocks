# 엔진 런타임 — 31. 엔진 이름 pool과 Actor·Component 태그(CR5 나머지)

[목차](README.ko.md)

## 31. 엔진 이름 pool과 Actor·Component 태그(CR5 나머지)

설계일 2026-11-26. [컴포넌트 도입](../ComponentCatalog/04-Prerequisites.ko.md) CR5 가운데 CM0이 미룬 태그다(23.2절 "태그는 미룬다"). 미룬 까닭은 World Registry에 NamePool이 없다는 것이었다. 이 장은 엔진 수명의 이름 pool을 두고, 그 위에 UE `AActor::Tags`·`UActorComponent::ComponentTags`(`TArray<FName>`)와 같은 태그를 넣는다. UE 경로 약어: `E/` = `Engine/Source/Runtime/Engine/`(UE 5.8.4).

범위 밖:
- 계층 태그 사전(UE GameplayTags — [엔진 구현 계획](../EngineImplementationPlan.ko.md) 20.1절 `TagId`·`TagSet`·`TagQuery`)은 따로다. UE도 `Tags`(FName 배열)와 GameplayTags를 따로 둔다.
- 쿠킹 쪽 CR5(EditorOnly 클래스 지정자·패키지 Exports 비트)는 에셋 세션 몫이다(23.2절).

### 31.1 단계

| 단계 | 내용 | 시험 |
|---|---|---|
| TG1 | 엔진 이름 pool(31.2절) | Engine 초기화·종료 순서, 엔진 Registry와 World의 pool이 같음, `WorldCreateDesc`가 준 pool 우선 |
| TG2 | Component 생성자 allocator(31.3절), Actor·Component 태그와 찾기(31.4·31.5절) | 31.7절 |

### 31.2 엔진 이름 pool

- `Engine`이 `NamePool` 하나를 갖는다(`Engine::Names()`). UE의 전역 FName 표와 같은 역할이다. 같은 Engine 안의 Registry들이 같은 토큰을 쓰므로 Name 값을 엔진 Registry와 World 사이에 그대로 옮길 수 있다. 에셋 객체 Registry도 이 pool을 빌리면 같아진다(EngineAssets 연결 — 에셋 세션 몫, 아래).
- 한도: `EngineConfig::names`(`NamePoolConfig` — 기본 65,536 entry·16 MiB)다. entry는 엔진 종료까지 회수하지 않는다(NamePool 계약). 한도에 닿으면 새 이름 intern(레벨 로드의 Name 필드 decode 포함)이 `CapacityExceeded`로 실패하고 이미 있는 이름은 그대로 쓴다. 기본값은 NamePool 기본을 따른다. 큰 프로젝트는 설정으로 늘린다(UE FName 표는 고정 상한 없이 자라지만 Rocks pool은 예산을 둔다 — 신뢰할 수 없는 데이터가 메모리를 끝없이 쓰지 않게).
- 수명:
  - `Engine::Initialize`가 엔진 Registry를 만들기 전에 pool을 `Initialize`한다(기본 `NamePoolConfig`). 실패하면 초기화 실패다(그 오류).
  - `Engine::Shutdown`이 World·Subsystem·엔진 Registry를 모두 정리한 뒤 마지막에 pool을 `Shutdown`한다. `Busy`(남은 lease)면 다른 Registry 종료 실패와 같이 실패를 기록하고(`LogShutdownFailure`) 그 오류를 돌려준다. 다시 부르면 pool 닫기만 다시 한다. 끝내 닫지 못한 채 Engine을 파괴하면 Fatal이다.
- 엔진 Registry: `RegistryOptions.names = &names_`.
- World: `Engine::CreateWorld`는 `WorldCreateDesc::objects.names`가 null이면 엔진 pool을 넣는다. 호출자가 준 pool은 그대로 쓴다(시험 — 그 pool은 World보다 오래 살아야 한다).
- 다른 Subsystem(EngineAssets의 에셋 객체 Registry 등)은 `Engine::Names()`를 빌려 쓸 수 있다. EngineAssets 연결은 에셋 세션 몫이다(알림).
- 스레드: `NamePool`은 스레드 안전하다(gate — 엔진 allocator가 스레드 안전하다는 전제). `Engine::Names()`는 엔진 수명 동안 어느 스레드에서도 부를 수 있다.
- 버린 대안:
  - World마다 pool. World·에셋 사이에 Name 값을 옮길 수 없고, 같은 이름이 World마다 entry를 다시 만든다.
  - 프로세스 전역 static pool. 시험이 Engine을 여러 번 만들고, 종료 순서를 Engine이 정할 수 없다.

### 31.3 Component 생성자

- `Component`는 `explicit Component(Allocator& allocator)`다(태그 배열의 allocator). `Component()` 기본 생성자는 없앤다. Actor는 이미 allocator를 받는다.
- 파생은 모두 allocator를 받아 넘긴다. `SceneComponent`·`MovementComponent` 같은 World 기반 클래스, EngineAudio `AudioComponent`, WorldIntegration 메시 컴포넌트, 시험 fixture와 시험 안 파생이다. 생성 코드(리플렉션 construct)는 allocator 생성자를 고른다.

### 31.4 태그(UE `AActor::Tags` — `E/Classes/GameFramework/Actor.h:1342-1344`, `UActorComponent::ComponentTags` — `E/Classes/Components/ActorComponent.h:181`)

필드:
- Actor `tags: Array<Name>`, Component `componentTags: Array<Name>`. `ROCKS_PROPERTY(ReadOnly)` 영속 필드이고 바꾸기는 아래 setter로 한다(24.1절 규칙 — UE는 BlueprintReadWrite 배열을 직접 고친다). 순서를 지키고 중복을 허용한다(UE `TArray`와 같다).

조회(const, 읽기):
- `HasTag(Name tag) → bool`(UE `ActorHasTag` — `E/Private/Actor.cpp:2940-2943`, `ComponentHasTag` — `E/Private/Components/ActorComponent.cpp:1121-1124`): None이면 거짓, 아니면 들어 있는지다.
- `Tags() → Span<const Name>`(Actor), `ComponentTags() → Span<const Name>`(Component).

바꾸기(쓰기 — 실패하면 값 그대로):
- `AddTag(Name tag)`: 이미 있으면 아무것도 하지 않고 성공한다(UE 사용처의 `AddUnique`와 같다). 없으면 끝에 더한다.
- `RemoveTag(Name tag)`: 같은 태그를 모두 뺀다(UE `TArray::Remove`). 없으면 아무것도 하지 않고 성공한다.
- `SetTags(Span<const Name> tags)`: 통째로 바꾼다(중복은 그대로 둔다).
- 검사: None 태그는 `InvalidArgument`다(UE는 None 태그를 넣을 수 있지만 `ActorHasTag`가 늘 거짓이라 쓸모가 없다 — 버린 대안: 받아 두기). 태그는 그 객체 Registry의 이름 pool에서 발급한 토큰이어야 한다. `NamePool::Acquire`가 `StaleHandle`로 거부하는 토큰(다른 pool·옛 토큰)은 `InvalidArgument`이고, 그 밖의 Acquire 오류(닫힌 pool `ShuttingDown` 등)는 그대로다. Registry에 pool이 없으면 `Unsupported`다(엔진 밖에서 pool 없이 만든 World).
- 대상이 파괴 대기면 `Destroyed`다(7.2절 — 변경 API 규칙). World 장부 밖 객체(그래프 로드 등 — `HasWorld()` 거짓)는 그 객체 Registry를 모르므로 `InvalidArgument`다(`Destroy`와 같다). 할당 실패는 `OutOfMemory`다. 검사 순서: 장부 밖 → 파괴 대기 → None → pool 없음 → 토큰.
- 바꾸기는 다시 보내기를 표시하지 않는다(태그는 렌더·물리 상태가 아니다).
- 스레드: World 게임 스레드(접근 문맥)다.
- 저장: Name 필드 wire 규칙 그대로다(문자열로 쓰고 읽을 때 그 Registry pool에 intern — 리플렉션 B9). 레벨 저장 왕복을 시험한다.
- 클래스 기본값 태그는 지원하지 않는다. UE는 생성자에서 `Tags.Add(TEXT("Enemy"))`로 CDO 태그를 주지만, Rocks 생성자는 allocator만 받아 이름 pool에 닿지 않는다. 태그를 넣는 길은 장부 편입 뒤 setter, 레벨·prefab 데이터(decode), 편집기 Details(CR8 편집 메타 뒤 — 필드가 ReadOnly라 편집기 쓰기 경로가 그때 생긴다)다.
- 스크립트 노출(Sparks `ScriptCallable`·`ScriptReadOnly`)은 이 단계에서 붙이지 않는다. Sparks 엔진 연동(EI) 단계가 정한다(UE는 `ActorHasTag`·`GetComponentsByTag`를 Blueprint에 낸다).

### 31.5 찾기

- `World::FindActorsWithTag(Name tag, Array<ObjectHandle>& out)`(UE `UGameplayStatics::GetAllActorsWithTag` — `E/Private/GameplayStatics.cpp:1140-1160`):
  - out을 비우고 태그를 가진 Actor handle을 담는다. 순서는 정하지 않는다(World 레코드 순 — UE `FActorIterator`도 순서를 약속하지 않는다). 모든 Level의 Actor를 본다. UE `FActorIterator` 기본 flag는 `OnlyActiveLevels | SkipPendingKill`(`Engine/Source/Runtime/Engine/Public/EngineUtils.h:510`)이라 보이지 않는 Level도 뺀다 — Rocks는 Level 스트리밍 가시성(CR22)이 없어 그때 같은 거르기를 더한다. 파괴 대기 Actor는 뺀다. Constructed(지연 Spawn 중) Actor는 넣는다.
  - None이면 비운 채 성공한다(UE "태그가 없으면 모든 Actor가 아니라 아무것도").
  - 할당 실패는 `OutOfMemory`이고 out은 빈다.
- `World::FindComponentsByTag(ObjectHandle actor, TypeId type, Name tag, Array<ObjectHandle>& out)`(UE `AActor::GetComponentsByTag(TSubclassOf<UActorComponent>, FName)` — `E/Classes/GameFramework/Actor.h:3815`): type이 null이면 모든 Component, 아니면 그 타입(파생 포함)만이다(UE는 null 클래스를 받지 않는다 — `IsA(nullptr)`가 `checkfSlow`, `UObjectBaseUtility.h:641`. Rocks는 null을 "모두"로 받는다). 순서는 Actor `components` 순서다(UE `GetComponents`는 순서를 약속하지 않는다). 파괴 대기 Component는 뺀다. None이면 비운 채 성공이다. 이 World의 Actor가 아니면 `StaleHandle`이다. World에 두는 까닭: 기존 `World::FindComponent<T>(actor)`와 같은 자리이고, Actor를 빌린 채 다른 객체를 빌리지 않는다.
- `World::FindComponentByTag(ObjectHandle actor, TypeId type, Name tag) → ObjectHandle`(UE `FindComponentByTag` — `:3811`): 첫째, 없으면(이 World의 Actor가 아님 포함) null이다.
- 색인은 두지 않는다. UE도 선형으로 훑는다. 버린 대안: 태그 → 객체 역색인(엔진 구현 계획 20.1절). 쓰기마다 색인을 고쳐야 하고, 요구·측정이 아직 없다. 필요해지면 계층 태그 사전과 함께 둔다.

### 31.6 결정

| ID | 항목 | 결정 | 근거 |
|---|---|---|---|
| ERD65 | 이름 pool 소유 | Engine 하나, 엔진 Registry·World에 빌려준다 | UE 전역 FName 표, Registry 사이 Name 값 이동 |
| ERD66 | 태그 표현 | `Array<Name>`(순서·중복 그대로), 계층 사전은 따로 | UE `Tags`·`ComponentTags`, GameplayTags는 별개 |
| ERD67 | 태그 쓰기 | setter(Add 중복 무시·Remove 모두·Set), None 거부, pool 검사, 클래스 기본값 태그 없음, 스크립트 노출은 EI 단계 | 24.1절 setter 규칙, None은 UE에서도 쓸모 없음, 생성자에 pool 없음 |
| ERD70 | 이름 pool 한도 | `EngineConfig::names`, 기본은 NamePool 기본, 고갈은 새 intern만 `CapacityExceeded` | 회수 없는 pool의 예산, 큰 프로젝트는 설정으로 |
| ERD68 | 찾기 | 선형, 파괴 대기 제외, None은 빈 결과 | UE GetAllActorsWithTag·GetComponentsByTag |
| ERD69 | Component 생성자 | `Component(Allocator&)`, 기본 생성자 없앰 | 태그 배열 allocator(23.2절이 예고) |

### 31.7 시험

`World/Tests/WorldTagTests.cpp`(`WorldTags_` 접두)와 Engine 시험:
1. 엔진 pool: Engine이 만든 World와 엔진 Registry가 같은 pool이다(한쪽에서 intern한 Name이 다른 쪽에서 Acquire된다). `WorldCreateDesc`가 준 pool은 그대로 쓴다. 새 Engine의 pool은 앞 Engine의 토큰을 받지 않는다(옛 토큰 `StaleHandle`).
2. 태그 조회·바꾸기: Add(새·이미 있음), Remove(여러 개·없음), Set(중복 보존), HasTag None 거짓, None·다른 pool 토큰·닫힌 pool의 옛 토큰 거부(값 그대로), pool 없는 World `Unsupported`, 파괴 대기 `Destroyed`, 장부 밖 `InvalidArgument`, 할당 실패(실패 주입 allocator가 없으므로 할당을 먼저 마치는 순서로 지킨다 — 30.7절과 같다).
3. 찾기: Level 여러 개·지연 Spawn(Constructed) 포함·파괴 대기 제외·None 빈 결과, 타입 필터(파생 포함)·null 타입, 첫째 찾기(null 타입 포함)와 없음.
4. 리플렉션 형태(필드 이름·ReadOnly)와 레벨 저장 왕복(태그 문자열이 다시 읽은 World pool의 Name이 된다).
