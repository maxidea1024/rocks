# 엔진 런타임 — 4. Object 시스템과의 결합

[목차](README.ko.md)

## 4. Object 시스템과의 결합

### 4.1 Registry 구성

- `TypeRegistry`는 엔진 전체에 하나다. 엔진 초기화가 엔진·월드·게임플레이 타입을 등록한다. 리플렉션 타입은 Engine이 소유하는 `ModuleRegistration` 하나로 모듈 단위 등록한다(World 타입 → 게임 리플렉션 모듈 → native 타입 순서, 6절).
- `ObjectRegistry`는 **World마다 하나**이며 World마다 고유 `ContextId`를 쓴다. Actor·Component는 소속 World의 Registry에서 생성한다. 다른 World의 객체는 핸들 교환만 가능하고 `ObjectAccessScope`가 문맥 오용을 거부한다. World 파괴는 그 Registry의 Shutdown으로 끝나므로 월드 단위 누수가 바로 드러난다.
- 엔진 수준 Registry가 하나 더 있으며 GameInstance처럼 World보다 오래 사는 객체를 둔다. World 객체를 강한 간선으로 참조하지 않는다.

### 4.2 소유권

- 소유 사슬은 **World → Level → Actor → Component**다(UE의 UWorld → ULevel → AActor → UActorComponent와 같은 형태).
  - World는 Level마다 `RootRef<Level>`을 보유한다. World가 가진 객체 루트는 이것뿐이다. 시작할 때 영구 Level을 만들고, 서브 Level은 `CreateLevel`로 추가한다.
  - Level은 소속 Actor를, Actor는 자기 Component를 **GC 추적 강한 간선 목록 `ObjectRefArray<T>`**로 소유한다. 이 목록은 영속 리플렉션 필드(`actors`·`components`)이고 소유자의 생성 Trace가 방문하므로 GC 도달성이 소유 관계를 그대로 따른다.
  - World는 Actor마다 수명 기록(`ActorRecord`: 상태·Tick 핸들·소속 Level)만 `SlotArray`에 둔다. Component의 수명 기록은 Component 객체 자신이 갖는다.
- 파괴는 **명시적**이다. World가 hook을 부른 뒤 소유자의 간선에서 빼고 `RequestDestroy`한다. GC는 그 뒤 저장소를 회수하는 수단이다. Level 파괴는 소속 Actor를 모두 정리한 뒤 Level 루트를 반환한다.
- `ObjectRefArray`는 이번 단계에서 Core(ObjectRuntime)에 추가했다(4.5절). 추가할 때 대상 타입·생존·같은 Registry를 검증하고, 증분 GC Marking 중이면 대상을 shade한 뒤 게시한다(삽입 장벽). 제거에는 장벽이 필요 없다.
- Actor의 root component는 **영속 강한 참조 `ObjectRef<SceneComponent>`**다(UE `AActor::RootComponent`와 같다). root는 늘 자기 `components` 안의 객체라 새 생존 간선을 만들지 않는다. 쓰기는 Registry `SetReference`로만 하고(Marking 중 삽입 장벽), root Component를 파괴하는 모든 경로(정상 정리·Registry 우회 파괴의 수리)에서 비운다 — 강한 참조는 파괴된 대상을 null로 만들지 않아 비우지 않으면 저장소가 좀비로 남는다.
- 소유가 아닌 그 밖의 객체 사이 참조(부모·자식 SceneComponent, Controller↔Pawn)는 `ObjectHandle`이다. 대상 파괴 후 접근은 핸들 generation 검사로 실패한다. 부착 계층의 실행 상태(parent·children)는 NATIVE이고 부모만 영속 `attachParent`로 저장한다(4.6절).
- Actor·Component·Level은 소속 `World*`를 `ROCKS_NATIVE` 멤버로 보관한다. World는 C++ 객체이며 자신의 Registry와 모든 객체보다 오래 산다.
- Level·Actor·Component는 `ROCKS_CLASS(WorldOnly)`, SceneComponent는 `ROCKS_CLASS()`(WorldOnly 상속)인 리플렉션 클래스다(A0-B B8b, [리플렉션·빌드 보강 설계](../ReflectionFoundations.ko.md) 9.1절). 메타컴파일러가 컨테이너(B5)와 `ROCKS_NATIVE`(B2)를 지원하게 되어 이전의 수동 네이티브 등록 이유가 사라졌다. 멤버 분류는 4.6절이다.

### 4.3 접근

- 객체 포인터는 `Borrow` 안에서만 쓴다. TickScheduler와 World는 매 호출마다 Borrow를 얻는다(잠금 비용은 진단으로 측정하고, 일괄 접근 API는 후속 최적화로 둔다).
- **`Resolve<T>`는 상속을 허용**하도록 바꾼다. 등록된 native 기반 체인에 `T`가 있으면 성공하고, 포인터는 `Object*`에서 `static_cast`로 내려 얻는다(단일 비가상 상속만 지원하므로 안전하다). `SetReference`·`TryPin`·생성 트랜잭션의 타입 검사도 같은 규칙을 쓴다. 정확 타입만 허용하던 기존 의미와 그 테스트는 바꾼다.

### 4.4 네이티브 클래스 등록

World 타입은 리플렉션 클래스이므로 `RegisterWorldTypes(ModuleRegistration&)`(생성 집계 함수 `rocks::generated::World::RegisterModuleTypes` — 의존 모듈 CoreTypeBindings의 struct부터, base 먼저)로 등록한다. 같은 registration에 다시 부르거나 다른 registration이 이미 등록했으면 lease를 붙잡고 성공한다(멱등). 영속 상태가 없는 런타임 전용 파생(.cpp 안의 시험 타입 등)은 네이티브 클래스로 등록한다:

```cpp
class SpinningActor final : public rocks::Actor {
public:
    ROCKS_NATIVE_TYPE(SpinningActor, rocks::Actor, 0x5b1c..., 0x9e02...)
    // ...
};

// 엔진 초기화(EngineConfig::registerTypes) 또는 게임 모듈 초기화에서. 리플렉션 base(Actor)의 생성
// 헤더가 필요하다(모듈 생성 헤더 World.module.generated.h는 등록 함수의 선언만 담는다).
#include <World/Actor.generated.h>
rocks::RegisterNativeClass<SpinningActor>(types);
```

매크로는 `ThisClass`·`Super` 별칭, 정적 `TypeId`, 이름을 정의한다. `NativeClassType` concept은 매크로를 빠뜨려 기반의 ID를 물려받은 클래스를 컴파일 시 거부한다. `RegisterNativeClass<T>`는 `baseType = ObjectTypeIdOf<T::Super>()`와 `MakeRuntimeTypeOps<T>`(`nativeBaseType` 설정)를 등록하고, Super 사슬 위에 리플렉션 조상 R(World 타입)이 있으면 R의 스키마 필드·생성 함수표를 빌리고 R의 WorldOnly를 물려받으며 GC Trace는 native 손 Trace를 부르다가 R의 생성 Trace에서 멈춘다(B8a). native 파생은 자기 C++ 상태를 모르므로 `completeFieldLayout = false`이고 그 객체가 든 그래프의 저장은 Unsupported다. 기반 타입을 먼저 등록해야 한다(TypeRegistry 규칙 — native 등록은 리플렉션 등록 뒤). `nativeBaseType`은 상속 Resolve의 포인터 변환 근거이므로 C++ 상속을 `static_assert`로 보증한 코드(RegisterNativeClass, 메타 생성 코드)만 설정하고, TypeRegistry는 기반 레코드 일치와 크기·정렬 하한을 검사한다. `World::SpawnActor<T>`·`AddComponent<T>`는 `ObjectTypeIdOf<T>()`를 쓴다 — native T는 `T::StaticTypeId`만 쓰고, 리플렉션 World 타입을 템플릿 인자로 쓰는 TU는 생성 헤더를 include한다(World.h는 생성 헤더를 include하지 않는다 — Engine.h 소비자 전체로 컴파일 비용이 번지지 않게).

### 4.5 ObjectRefArray

```cpp
ROCKS_CLASS(WorldOnly)
class Actor : public Object {
    ROCKS_GENERATED_BODY()
public:
    explicit Actor(Allocator& allocator) noexcept : components_(allocator) {}
private:
    // 생성 Trace가 방문한다(손 Trace 없음).
    ROCKS_PROPERTY(ReadOnly, Required, Name = "components") ObjectRefArray<Component> components_;
};

registry.AddReference(access, actor, actorPointer->components_, component);
registry.RemoveReference(access, actor, actorPointer->components_, component);
```

- 원소 버퍼는 소유 객체의 allocator(Registry allocator)에서 할당하고, 확장은 Registry 잠금 밖에서 한다. 원소를 바꾸는 쪽은 소유 스레드뿐이고 GC trace는 slice 안에서만 원소를 읽으므로, 확장과 trace가 겹치지 않는다.
- 배열 객체는 소유자 저장소 안에 직접 있어야 한다(`SetReference`의 필드 위치 검사와 같은 규칙). 추가는 게시된 생존 소유자만 받고, 정리용 제거는 파괴 요청된 소유자에서도 허용한다.
- 끝 원소와 바꿔 떼기 `RemoveReferenceAtSwap(access, owner, array, index)`: index 원소를 지우고 끝 원소를 그 자리로 옮긴다(순서를 지키지 않는다 — O(1), 앞에서 찾거나 뒤를 당기지 않는다). 계약:
  - 지운 원소는 `RemoveReferenceAt`과 같은 규칙이다(장부·관찰자 기록, 삭제 간선은 삽입 장벽이 필요 없다).
  - 옮기는 원소는 같은 소유자·같은 컨테이너에 이미 있는 간선이다. 대상의 생존·cutoff·세대를 다시 묻지 않는다 — 대상이 DestroyRequested(World API 밖 파괴)이거나 cutoff white여도 실패하지 않는다. 간선 다중도가 그대로라 장부·관찰자 기록도 없다.
  - owner 규칙은 `RemoveReferenceAt`과 같다(Alive 또는 정리 중인 DestroyRequested, Borrow 보유, 컨테이너가 owner 저장소 안). index ≥ Size()는 InvalidArgument다. 할당하지 않는다(관찰자 Registry는 지운 원소 하나의 예약 — 공통 규칙).
  - 배경: 처음 구현(1913624)은 `SetReferenceAt(index, 끝 원소)` + 끝 `RemoveReferenceAt`이었는데, `SetReferenceAt`은 대상 검증을 거쳐 끝 원소가 DestroyRequested면 StaleHandle로 실패했고 실패를 버려 지울 Actor가 Level 배열에 좀비로 남았다(리뷰 지적). 같은 컨테이너 안 이동은 대상 검증 대상이 아니다.
- World가 만든 목록에는 null이 없지만 로드된 목록에는 null 원소가 올 수 있다(`Required`는 로드에서 진단만 한다). 그래서 World의 Component 목록 순회(FindComponent·FinishSpawning·활성화·시작·DestroyActor 등)는 null을 끝 표지로 쓰지 않고 원소 수 기준으로 돌며 null을 건너뛴다.

### 4.6 리플렉션과 레벨 편입(A0-B B8b)

멤버 분류([리플렉션·빌드 보강 설계](../ReflectionFoundations.ko.md) 9.1절, 결정 21):

| 타입 | PROPERTY(영속, 모두 ReadOnly — 도구 이름) | NATIVE(런타임) |
|---|---|---|
| Level | `actors`(ObjectRefArray<Actor>, Required) · `name` · `levelScript`(ObjectRef<LevelScript>, null 허용 — `actors` 안의 레벨 스크립트 Actor, [LevelScript](../LevelScript.ko.md) LD4) | World 포인터 · 영구 여부 · 파괴 대기 |
| Actor | `components`(ObjectRefArray<Component>, Required) · `rootComponent`(ObjectRef<SceneComponent>) · `enabled` | World 포인터 · 소속 Level(actors 포함 관계에서 유도) · Tick 설정·Tick 켜짐 |
| Component | `enabled` | World 포인터 · 소유 Actor · Tick 설정 · 수명 기록 |
| SceneComponent | `relativeTransform`(Transform — CoreTypeBindings 미러) | 월드 행렬 캐시 · dirty · parent · children(유도값) |

- **prefab이 더할 것(자리 — [Prefab](../Prefab/README.ko.md), 2026-11-28 PF0)**: Component의 Outer는 소유 Actor이고, 선언 subobject(`ROCKS_PROPERTY(Subobject)`)는 subobject 표를 ObjectRuntime instancing이 만들며 SpawnActor는 Actor와 subobject를 한 트랜잭션에서 Commit한다(PF1 — Prefab 3.2절. 지금은 Actor·Component 모두 Outer·이름 없이 만들고 AddComponent가 별도 트랜잭션이다). Level에 Transient `spawnedActors`(펼친 Actor 소유 목록, PF3)와 영속 `PrefabPlacement` 배열·`placementNodeReferences`(PF5), Actor에 Unpack용 영속 Entity Guid(PF7)를 더한다. 객체 생성 출처(`ClassSubobject`·`PrefabNode`·`InstanceAdded`·`Runtime`)는 Registry slot 표지다.
- **ReadOnly**: 경로·리플렉션 쓰기가 World 장부(ActorRecord·hook·MarkDirty·Tick 등록)를 우회하지 않게 World 필드는 모두 ReadOnly다(로드의 staged 쓰기는 허용). 값은 World API(`SetEnabled`·`SetRootComponent`·`SetRelativeTransform`·`SetLevelScript`(편집 World 전용) 등)로 바꾼다. 그래서 CU5 setter hook 전에는 에디터에서 `relativeTransform`을 경로로 편집할 수 없다(UE `EditAnywhere`와 다른 제약).
- **Tick 설정은 NATIVE**다(UE `PrimaryActorTick`은 archetype 차분 직렬화 — Rocks에는 아직 클래스 기본값 레코드와 차이 저장이 없어 인스턴스 저장값이 클래스 기본값 변경을 가린다). 레코드와 struct 멤버 단위 비교가 생기는 A5에서 리플렉션 struct·enum으로 옮기며, 그래서 `TickSettings`를 단순 값 구조로 둔다([RuntimeClassMetadata](../RuntimeClassMetadata.ko.md) '클래스 기본값 결정').
- **부착 계층**: 실행 중 `parent_`·`children_`은 NATIVE이고, 부모는 영속 `attachParent`(`WeakObjectRef<SceneComponent>` — 같은 그래프 안 대상만 보존)로 저장한다(parent는 다른 Actor의 Component에 붙을 수 있어 강한 참조로 두면 좀비와 그래프 끌어오기가 생긴다 — UE는 Outer·레벨 패키지·garbage elimination으로 푼다). 편입이 `attachParent`에서 `parent_`·`children_`을 다시 만든다(29절). 리플렉션 B9가 `WeakObjectRef<T>` PROPERTY(같은 그래프 안 대상만 보존)를 가능하게 하며, 부착 계층을 리플렉션으로 옮길지는 World 단계가 정한다([리플렉션 B9](../ReflectionExtensions.ko.md) 5.1절).
- **Level 그래프 저장은 구조 시험용이다**: graph v2 편의 API `SaveObjectGraph(root)`로 Level 루트를 저장할 수 있다(A0-Od1부터 — [ObjectFoundations](../ObjectFoundations.ko.md) 5.2절, 그 전에는 graph v1)(그래프의 모든 타입이 생성 타입일 때 — native 파생 Actor·Component가 하나라도 있으면 Unsupported, Actor별 저장 제외는 레벨 패키지 단계). 부착 부모는 `attachParent`로 저장되고 편입(29절)이 자식 목록을 다시 만든다. 저장 안전 지점은 Tick 밖·파괴 대기 정리(FlushDestroys) 뒤다 — 정리 전에는 파괴 대기 Actor가 `actors`에 남아 함께 저장된다.
- **레벨 저장 검증(레벨 패키지 단계 — 자리)**: 레벨 저장·쿡은 (a) `levelScript`가 null이 아니면 `actors`의 원소이고 그 밖에 LevelScript 파생 Actor가 없는지(`LevelScriptNotInLevel`·`DuplicateLevelScript` — cooked 로드도 다시 검사해 `CorruptPackage`, 출시 빌드도 끄지 않는다 — LevelScript LD5), (b) 레벨에 저장하는 weak 참조가 같은 Level의 객체만 가리키는지(다른 Level은 `CrossLevelReference`로 거부, 셀 사이는 영속 Entity 참조가 생기기 전까지 거부 — LD6), (c) prefab 배치가 펼친 노드를 가리키는 참조를 Record에서 null로 두고 (배치 `instanceGuid` + 노드 주소)로 따로 저장하는지([Prefab](../Prefab/README.ko.md) D22, LD17)를 검사한다. 진단 이름은 [에셋 시스템](../AssetSystem/04-IdentityPaths.ko.md) 4.6절이다.
- **장부 밖 객체**: World API(SpawnActor·AddComponent·CreateLevel) 밖에서 만든 Actor·Component(그래프 로드, World registry에 직접 `NewObject`)는 `World*`가 없고 World에 수명 기록이 없다. 그 객체의 계층·World API는 Fatal 대신 `InvalidArgument`다(`SetRootComponent`·`Destroy`·`AttachTo`(그 객체를 부모로 쓰는 경우 포함)·`Detach`, `World::DestroyComponent`). 수명 기록이 필요한 World API(`DestroyActor`·`AddComponent`·`GetComponents`)는 `StaleHandle`이다. 상대 Transform 설정과 월드 행렬 계산은 부모가 없으므로 된다. `GetWorld()`는 참조를 돌려주므로 여전히 Fatal이며 `HasWorld()`로 먼저 확인한다.
- **레벨 편입 계약(레벨 패키지 단계 — 자리)**: 로드한 Level 그래프를 World에 넣는 단계는 (1) Actor마다 `ActorRecord`를 만들고 Level 레코드의 Actor 수를 맞추며 Actor의 `actors` 배열 위치를 레코드(`levelPosition` — 7.4절 바꿔 떼기의 근거)에 기록하고(아래 (4)의 null 정리·압축 뒤 다시 매긴다), Component마다 World의 Component 색인(소유 Actor·Tick handle — 7.4절 우회 파괴 복구)에 넣으며, (2) Level·Actor·Component의 `World*`·Actor의 소속 Level·Component의 소유 Actor를 `actors`·`components` 포함 관계에서 연결하고, (3) 부착 계층이 영속된 뒤에는 parent에서 children을 다시 만들며, (4) `actors`·`components`의 null 원소를 정리하고 root가 `components` 밖이면 비우며, (5) `relativeTransform`을 `SetRelativeTransform`과 같은 규칙(유한·단위 quaternion)으로 검증한 뒤, (6) World 수명 단계(FinishSpawning과 같은 hook 순서)로 진행한다. Actor마다 FinishSpawning하되 `levelScript`는 그 Level의 다른 모든 Actor(prefab 배치에서 펼친 Actor 포함)의 FinishSpawning 뒤에 한다 — 저장 때 꺼져 있던 Actor는 "이미 활성" 보장에서 빠지고, Start는 시작 큐 순서대로 같은 프레임에 준비된 활성 Actor 뒤에 시작하며 꺼진 채 기다리거나 준비가 안 된 Actor는 기다리지 않는다([LevelScript](../LevelScript.ko.md) LD8). 로드는 호출자가 World registry에 직접 하고(편입 전 객체는 장부 밖), 편입은 `World::AdoptLevel`이다 — 상세는 [29절](29-LevelAdoption.ko.md)(별도 registry 문장을 대신한다).
- **World registry의 외부 생성 거부(정책 자리)**: World registry에 World API 밖에서 World 타입을 만드는 것(직접 `NewObject`·그래프 로드)은 계약 위반이다. 지금은 위 장부 밖 규칙으로 Fatal 없이 거부만 하고, A0-O의 에셋 Registry 정책(WorldOnly 타입의 에셋 Registry 생성 거부)의 거울로 World registry가 WorldOnly 타입의 외부 생성 경로를 막는 검사를 둘 자리를 남긴다. 그 검사는 `RegistryPolicy::worldOnlyCreationKey`(World API만 가진 키를 함께 줄 때만 WorldOnly 타입 생성 허용)로 넣는다([ObjectFoundations](../ObjectFoundations.ko.md) 1.3절 후속 자리).
