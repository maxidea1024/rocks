# prefab 설계 — PF1 클래스 원형(선언형 subobject) 구현 명세

[목차](README.ko.md)

## 10. PF1 구현 명세

기준일 2026-11-28(개정 6, 교차 검증 1차 반영 — 10.10절). 3.2절의 결정을 구현 단위로 내린다. 3.2절과 다르면 이 장이 정본이다(10.9절). 근거 코드는 2026-11-28 `feature/engine-runtime-s7`이다.

### 10.1 범위

- 소유자는 **Actor 파생 클래스**, subobject는 **Component 파생**뿐이다. Component 안 subobject, Actor가 아닌 소유자, 에셋 클래스의 subobject는 MetaCompiler 오류다. 그래서 A1 패키지 Exports(에셋 객체)는 이번에 바꾸지 않는다 — 에셋 클래스에 subobject가 생길 때 graph와 같은 칸을 더한다.
- EditorOnly component 타입은 subobject가 될 수 없다(MetaCompiler 오류 — Game·Tryout World가 EditorOnly component를 `Unsupported`로 거부하므로 그 Actor는 spawn이 늘 실패한다, `WorldComponents.cpp:449-452`). UE `CreateEditorOnlyDefaultSubobject` 대응은 후속이다.
- C++ 선언만 한다. Sparks `let` component(SL 4.5절)는 Sparks L2(EI)에서 같은 표로 컴파일한다.
- 타입 교체(파생이 상속 subobject 타입을 바꿈)는 후속이다(10.8절). 파생 클래스의 기본값 덮기는 넣는다.

### 10.2 선언과 스키마

**클래스 지정자**(소유 구조를 일반 규칙으로 서술한다 — ObjectRuntime·ObjectSerialization이 World 타입을 몰라도 배선할 수 있게):

| 지정자 | 붙는 곳 | 뜻 |
|---|---|---|
| `ROCKS_CLASS(SubobjectList = "components")` | `Actor` | 선언 subobject를 담는 소유 목록 필드(`ObjectRefArray<T>`, 강한 간선) |
| `ROCKS_CLASS(SubobjectRoot = "rootComponent")` | `Actor` | `Root` subobject를 넣는 필드(`ObjectRef<T>`) |
| `ROCKS_CLASS(SubobjectAttach = "attachParent")` | `SceneComponent` | subobject 타입의 부착 부모 필드(`WeakObjectRef<T>`) |

- 값은 **그 클래스가 선언한** 필드의 실효 도구 이름(`Name=` 또는 C++ 이름 — base 필드는 private일 수 있어 받지 않는다)이고, MetaCompiler가 FieldId로 풀어 스키마에 Guid로 싣는다. base에서 상속되고 파생이 다시 지정할 수 없다. 이름이 그 클래스 필드가 아니거나 종류가 다르면 오류다.

**필드 지정자**:

```cpp
protected:
    ROCKS_PROPERTY(Subobject, Root, Name = "lightComponent")
    ObjectRef<PointLightComponent> lightComponent_;
    ROCKS_PROPERTY(Subobject, AttachParent = "sceneRoot", Name = "mesh")
    ObjectRef<StaticMeshComponent> mesh_;
```

- `Subobject`는 클래스 계층에 `SubobjectList`가 있는 클래스의 `ObjectRef<T>` 필드에만 붙는다. `T`는 목록 원소 타입의 파생이고 EditorOnly 타입이 아니다. 필드는 암묵 ReadOnly다.
- 접근 지정은 protected 이상이다(private이면 오류 — 파생 클래스가 `InitSubobjectDefaults`에서 `&Base::field_`로 상속 subobject를 덮을 수 있어야 한다).
- **객체 이름 = 필드의 실효 도구 이름**(`Name=` 또는 C++ 이름). MetaCompiler가 `ValidateNameComponent` 규칙(첫 글자·64 byte·예약어)으로 검사한다.
- `Root`: `SubobjectRoot`가 있어야 하고 `T`는 그 필드 타입의 파생이다. 클래스 계층 전체에서 하나뿐이다. `Root`와 `AttachParent`를 함께 쓰면 오류다.
- `AttachParent = "<필드 도구 이름>"`: 같은 클래스나 base의 다른 `Subobject` 필드다. `T`와 대상 타입에 `SubobjectAttach`가 있어야 한다. 순환은 오류다. 부착 부모 타입은 subobject 타입 부착 필드(`WeakObjectRef<T>`)의 `T` 또는 그 파생이어야 한다(MetaCompiler RMC3002, 런타임도 `TypeMismatch`로 다시 본다). 생략하면 `SubobjectAttach`가 있는 subobject는 `Root`에 붙고(자신이 `Root`이거나 Root 타입이 부착 필드 타입의 파생이 아니면 붙지 않음), `Root`가 없으면 붙지 않는다. 생략 규칙은 항목을 만들 때 한 번 정한다 — base에 `Root`가 없고 파생이 `Root`를 더하면 base 항목은 붙지 않은 채로 남는다(펼친 표의 앞부분이 바뀌지 않아야 한다).
- `Transient`·`EditorOnly`·`Required`·`Notify`·Sparks 지정자와 함께 쓰면 오류다.
- 진단 번호는 MetaCompiler 다음 빈 RMC 번호를 쓴다(구현 때 정한다).

**모듈 경계**: StaticMeshActor(WorldIntegration)처럼 다른 모듈의 base에서 `SubobjectList`를 상속하므로, 모듈 descriptor(`ModuleDescriptor.cs` — Within과 같은 방식)에 클래스 지정자 세 칸과 subobject 항목(id·도구 이름·타입·Root·부착 부모)을 싣는다. Root 유일성·AttachParent 해석·이름 충돌은 모듈 경계 너머까지 검사한다.

**ID**: SubobjectGuid = 그 필드의 FieldId다. `.rocksids.json` C++ 이름 키에 kind `subobject`를 더한다(`StableIdManifest.cs:491`, 그리고 `ClangAstParser.cs:1029`·`CompilerDriver.cs:457`·`IdentityCommands.cs:50`). kind가 바뀌면(field ↔ subobject) 지금 `Resolve`는 "missing"으로만 보고하므로(`StableIdManifest.cs:211-217`) 다른 kind 항목을 찾아 전용 진단을 낸다 — 새 ID를 조용히 발급하지 않는다.

**스키마**(`TypeSchema` — MetaSchema):

```cpp
enum class SubobjectFlags : uint32 { None = 0, Root = 1u << 0 };

struct SubobjectSchema {
    Guid id;                  // = 소유 필드의 FieldId(필드 index는 등록이 id로 찾는다 — EditorOnly 필드가 빌드마다 빠져 index가 어긋나므로)
    TypeId type;              // 생성 타입(필드의 T)
    int32 attachParent = -1;  // 펼친 subobject 표 index(-1 = 붙이지 않음 — 생략 규칙은 MetaCompiler가 풀어 둔다)
    uint32 flags = 0;         // SubobjectFlags
};
// TypeSchema에 더한다:
Span<const SubobjectSchema> subobjects{};   // 펼친 표
Guid subobjectListField{};      // 실효 값(상속 포함), 0 = 지정 없음
Guid subobjectRootField{};
Guid subobjectAttachField{};
```

- 객체 이름은 필드 도구 이름이라 따로 싣지 않는다. **스키마는 펼친 표를 싣는다**(상속 필드처럼 — base 표 뒤에 자기 항목, 2026-11-28 PF1a 구현 결정). 등록은 base 표가 앞부분과 같은지(`InheritsSubobjects` — 아니면 `TypeMismatch`)와 클래스 지정자를 base와 같은 값으로 이어받는지를 본다. 등록 검사: id 유일·0 아님(겹치면 `AlreadyExists`), id가 이 타입의 ReadOnly ObjectRef 필드(Transient·EditorOnly·Required·Notify 아님, 목록·root 필드 아님), type이 0 아님, attachParent가 앞쪽 index이거나 −1, Root 하나(Root면 부착 부모 없음), 연산표 수·ID 일치, 클래스 지정자 필드의 종류, 표가 있으면 목록 지정자 필수(그 밖 `InvalidArgument`). struct 스키마는 이 칸을 모두 비워야 한다.
- **표 순서**(MetaCompiler): base 표 뒤에 자기 항목을 둔다. 자기 `Root`가 자기 항목의 맨 앞이고 나머지는 선언 순서다. `AttachParent`는 base 항목이나 **위에 선언한** 자기 항목만 가리킨다(위상 정렬 대신 선언 순서 규칙 — 생략한 부착 부모가 Root를 가리키도록 Root를 앞에 둔다).
- 한도 `MaxClassSubobjects` = 256(펼친 표 크기 — MetaSchema 상수, 넘으면 MetaCompiler 오류와 등록 `CapacityExceeded`).

**타입을 지운 접근**(ObjectRuntime이 소유 타입을 모른 채 쓰도록): 지금 `RuntimeFieldOps`에는 필드 주소 thunk가 없고 배열 필드는 `writeStaged`가 없다(`RuntimeTypeOps.h:362-`). 생성 `Ops()`에 클래스마다 자기 것만 더한다.

```cpp
struct RuntimeSubobjectOps {
    Guid id;
    void* (*field)(Object& owner) noexcept;   // 소유 필드(ObjectRef<T>) 주소
};
// RuntimeTypeOps에 더한다:
Span<const RuntimeSubobjectOps> subobjects{};
Result<void> (*initSubobjectDefaults)(SubobjectDefaultsContext&) noexcept = nullptr;  // 자기 선언만
// 소유 구조 지정자 필드 주소(PF1b 구현 결정): 그 지정자를 선언한 클래스만 싣는다(Actor components_처럼
// private일 수 있어 파생 GeneratedType이 접근하지 못한다). ObjectRuntime이 base 사슬을 걸어 찾는다.
void* (*subobjectListAccess)(Object&) noexcept = nullptr;
void* (*subobjectRootAccess)(Object&) noexcept = nullptr;
void* (*subobjectAttachAccess)(Object&) noexcept = nullptr;
```

ObjectRuntime 안에 타입을 지운 `ObjectRefArray` staged 대입(목록 앞에 넣기·다시 짜기)을 더한다.

**기본값 덮기**:

```cpp
static Result<void> InitSubobjectDefaults(SubobjectDefaultsContext& context) noexcept;

Result<void> PointLight::InitSubobjectDefaults(SubobjectDefaultsContext& context) noexcept {
    return context.With(&PointLight::lightComponent_, [](PointLightComponent& light) noexcept {
        light.SetMobility(ComponentMobility::Stationary);
        return Result<void>::Success();
    });
}
```

- MetaCompiler가 클래스 본문에서 이 이름의 static 멤버 함수를 찾는다(`CheckValidateHook` RMC2309와 같은 방식 — `ClangAstParser.cs:928-931`, 서명이 다르면 오류, 생성 `Ops()` 본문에 서명 static_assert). 생성 코드는 자기 함수 포인터만 싣고 base를 부르지 않는다. **런타임이 base 체인(가장 먼 base 먼저)을 걸으며 부른다** — 모듈 경계를 넘는 생성 호출을 피한다.
- `SubobjectDefaultsContext::With(ObjectRef<T> Owner::* field, fn)`: 소유 객체가 `Owner`의 파생인지(`IsNativeA(NativeTypeKey<Owner>())`) 확인한 뒤 `&(owner.*field)`를 펼친 표 `subobjects[k].field(owner)`와 비교해 항목을 고른다. 없는 필드·subobject 아닌 필드·타입 불일치는 `InvalidArgument`다.
- 두 모드가 같은 함수를 쓴다: **기록 모드**(레코드 생성 — Registry 밖 임시 소유 인스턴스와 임시 subobject 인스턴스, 소유 필드는 비어 있어도 주소 비교는 된다)와 **instancing 모드**(staged 소유 객체와 staged subobject).
- **World 없는 hook**: 두 모드 모두 World 장부 밖에서 돈다(`HasWorld()` 거짓). 값 설정만 하고 World·Registry·다른 객체를 건드리지 않는다 — 생성자 결정성 계약(RCM 59행)과 같다. 두 번 실행해 인코딩을 비교한다. 등록 때 해석이 필요한 값(예: 충돌 profile 이름)은 `OnRegister`가 다시 해석하므로 그대로 쓴다(`PrimitiveComponent.cpp:1238-1257`).

### 10.3 생성(ObjectRuntime)

`Result<usize> ConstructionContext::CreateDeclaredSubobjects(ObjectHandle owner, Span<ObjectHandle> out)`가 모든 경로(SpawnActor·graph 복원·prefab spawn)의 생성 규칙이다. `out`은 펼친 표 크기이고 만든 handle을 표 순서로 받는다(호출자가 클러스터 가입·장부에 쓴다).

1. owner 타입의 펼친 표를 얻는다(비면 성공·무동작).
2. 표 순서로 subobject를 staged로 만든다 — `Create(type, outer = owner, name = 필드 도구 이름)`.
3. 슬롯 생성 출처를 `ClassSubobject`로 표시한다(10.5절 — 생성 인자로 넘겨 별도 잠금이 없다).
4. 배선(staged 쓰기): 소유 필드 ← subobject, `SubobjectList` 목록 ← 표 순서로 앞에(타입을 지운 대입), `Root`면 `SubobjectRoot` 필드 ← subobject, `SubobjectAttach` 필드 ← 표의 부착 부모.
5. `InitSubobjectDefaults` 체인을 instancing 모드로 부른다.

실패는 트랜잭션 실패다(Commit 전이라 전부 rollback). 소유 목록은 소유 객체를 막 만든 직후라 비어 있어야 한다(아니면 `InvalidArgument`). 필드 주소는 연산표 접근자로 얻고, `ObjectRef`·`WeakObjectRef`·`ObjectRefArray`가 standard-layout이라 첫 멤버(handle·배열 저장소)와 주소가 같다는 점(포인터 상호 변환 — 정적 검사)으로 handle 칸에 쓴다. weak 부착 부모는 GC 간선이 아니라 staged 객체에 바로 쓴다. 구현은 `ObjectRuntime/Private/DeclaredSubobjects.cpp`, 시험은 `DeclaredSubobjectTests`다.

### 10.4 graph 형식 3(ObjectSerialization)

**NodeTable 항목**에 종류 칸(u8)을 더한다.

| 종류 | 항목 | 복원 |
|---|---|---|
| 0 Object | `{Guid type, u32 outer, String name}`(지금과 같다) | 새 객체 |
| 1 DeclaredSubobject | `{u32 owner, Guid subobjectId}` | 소유 노드가 만든 subobject를 이 번호에 묶는다 |

- 종류 1의 owner는 **그보다 앞의** 종류 0 노드이고(바로 뒤일 필요 없음), (owner, subobjectId)는 그래프 안에서 유일하다(중복이면 `CorruptPackage`). owner 타입의 펼친 표에 subobjectId가 없으면(클래스에서 사라짐) 복원은 `InvalidArgument`와 진단 `DeclaredSubobjectMissing`으로 실패한다(하위호환 없음 — 다시 저장한다).
- `ResolveNodeTable`은 종류 1의 Outer 판정("집합 밖 Outer는 모두 같다")을 건너뛴다(Outer는 owner로 정해진다). `NodeEntryBytes` 예산에 종류 칸과 종류 1 항목 크기를 넣는다.
- `writeNodeTable = false`인데 집합에 선언 subobject를 가진 객체가 있으면 `InvalidArgument`다(종류 1을 표현할 수 없다).
- `ObjectGraphFormatVersion` = 3. 클래스 기본값 지문과 그래프 byte가 모두 바뀐다 — 모든 에셋 패키지(TestContent 포함)를 다시 쿠킹하고 golden(`PackageGoldenTests`·`ClassDefaultsTests`·너겟 시험)을 다시 고정한다.

**저장**:

- **Include**: `AddNode(owner)` 직후 그 선언 subobject를 표 순서로 넣는다. 호출자가 준 집합의 owner에 딸린 subobject는 집합 끝에 덧붙인다. `IncludeNode`가 subobject를 대상으로 받으면 조상 사슬(owner)을 더한 뒤 `locals_`를 다시 확인해 중복을 막는다(`GraphSaveSet.cpp:9-34·117-201`). subobject의 SubobjectGuid는 owner의 `subobjects[k].field(owner)`가 그 handle을 담은 항목에서 얻는다.
- 소유 필드(`Subobject`)는 값으로 쓰지 않는다.
- `SubobjectList` 목록은 선언 subobject를 뺀 나머지만 쓴다. `FieldEncodeContext`에 원소 생략 질의를 두고 배열 codec이 개수를 먼저 센 뒤 쓴다(원소 수가 앞에 오므로 — `ArrayFieldCodec.h:127`).
- `SubobjectRoot` 필드(종류 0 owner)와 `SubobjectAttach` 필드(종류 1 노드)는 **encode 전에 표 대상과 비교**한다. 같으면 쓰지 않고, 다르면 null이어도 기준 비교(`OmitCurrent` — `GraphSave.cpp:478-485`)를 거치지 않고 **강제로 쓴다**(레코드의 이 필드는 null이라, 그대로 두면 사용자가 지운 null이 기준과 같아 사라지고 복원이 표 대상을 다시 써 넣는다).
- 종류 1 노드의 Record 기준은 subobject 기본값 레코드다(10.6절).

**복원**:

- **도달 검사**(`CheckReachable` — `GraphStage.cpp:273-279`, `GraphReachability.cpp:7-11`)에 "종류 0 owner → 그 종류 1 노드" 암묵 강한 간선을 더한다(소유 필드·목록·root가 저장되지 않아 그 노드로 들어오는 참조가 없다).
- **1단계 검증**에서 거부한다: Record에 `Subobject` 필드 값이 있음, `SubobjectList` 원소가 종류 1 노드이거나 중복, 종류 1 노드 Record의 타입이 표 항목 타입과 다름, owner 아래 표 이름과 같은 이름의 종류 0 노드(`CheckCreatePreconditions` — `GraphIdentity.cpp:148`, 진단 `DeclaredSubobjectNameConflict`). 생성 예산에는 표에 있으나 그래프에 없는 subobject 수도 넣는다.
- **2a**: 종류 0 노드를 만든 직후 `CreateDeclaredSubobjects`(10.3절)를 부르고 종류 1 항목을 (owner, id) 키로 그 subobject에 묶는다. 표에 있는데 그래프에 없는 subobject(클래스에 새로 생김)도 만들어지고 기본값 그대로다. 2a는 이제 할당·상한 밖에도 `InitSubobjectDefaults` 실패로 실패할 수 있다(`GraphApply.cpp:303-305` 주석과 OF 5.4절 실패 목록을 고친다).
- **2b**: Record를 적용한다(누락 = 기준). `ApplyFields` 끝, `CheckRequired` 앞(`GraphStage.cpp:156·186`)에서 `SubobjectList` 목록을 "선언 subobject(표 순서) + Record의 원소"로 다시 짠다(Record decode가 목록을 통째로 바꾸므로 — `ArrayFieldCodec.h:652`). `Required`는 원소 non-null 규칙이라 빈 목록은 문제없다.

### 10.5 생성 출처

- `ObjectRegistry` 슬롯 표지 `ObjectOrigin` — `Default`(0)·`ClassSubobject`. 표지는 Create 때 정하고 바뀌지 않는다.
- `PrefabNode`·`InstanceAdded`·`Runtime`은 각 단계(PF3·PF4·PF5)에서 더한다. 특히 `Runtime`(hook이 만든 component를 저장하지 않음)은 편집 World 모드(PF5, D18)와 함께 켠다 — 그 전에는 `AddComponent`로 만든 component가 지금처럼 저장된다(UE도 인스턴스에서 추가한 component는 `InstanceComponents`로 저장한다 — `Actor.h:4339-4340`).

### 10.6 subobject 기본값 레코드(ObjectSerialization ClassDefaults)

- 키 (소유 TypeId, SubobjectGuid). 소유 타입 하나의 레코드를 한꺼번에 만든다: 임시 소유 인스턴스와 표의 subobject 타입 임시 인스턴스를 생성자로 만들고, 체인을 기록 모드로 부른 뒤 subobject마다 인코딩한다(이벤트 필드는 클래스 레코드처럼 건너뛴다). 두 번 만들어 비교한다(결정성). 소유 타입 레코드와 같은 캐시·실패 기억 규칙(RCM A5a)을 쓴다.
- 지문 = SHA-256(레코드 byte, 구성, codec wire 버전) — 클래스 레코드 지문과 같은 구성 규칙. 지금 쓰는 곳은 없다(에셋 패키지에는 subobject가 없고, 레벨·prefab 패키지가 생길 때 Summary `ClassDefaults`에 싣는다 — 3.6절).
- **노드별 기준**: `GraphBaseline`에 기준 제공자를 더해 종류 1 노드는 subobject 기본값 레코드, 그 밖은 클래스 레코드를 기준으로 한다. 차이 쓰기·되돌리기(`PlanRevert`)·기본값 조회가 노드 종류에 따라 기준을 고른다(되돌리기의 원형 기준 일반화는 PF4).

### 10.7 World

- **SpawnActorDeferred**: 한 `ConstructionContext`에서 Actor `Create` → `CreateDeclaredSubobjects` → 클러스터 가입(Actor와 subobject) → Commit 한 번. Commit 뒤:
  - Actor 장부·Level 간선(지금과 같다).
  - subobject마다 Component 장부(`world_`·`owner_`·큐 여유) — `AddComponent`의 Commit 뒤 처리를 내부 함수로 꺼낸다.
  - **모두 장부에 넣은 뒤** `CheckComponentRules`를 부른다(편입과 같다 — `WorldLevelAdoption.cpp:337-342`; 하나씩 부르면 필수 타입 규칙이 거짓 실패한다).
  - 부착 실행 상태(`parent_`·`children_`)와 Mobility 규칙(ERD59)을 `attachParent`에서 만든다. 지금 편입 본문 안 코드(`WorldLevelAdoption.cpp:400-445`)를 함수로 꺼내 둘이 같이 쓴다.
  - Commit 뒤 실패는 지금 rollback(RequestDestroyOwned)으로 Actor와 subobject를 함께 정리한다.
- **hook 순서**: 선언 root는 PreAwake 전 등록 1단계에 이미 있다(지금은 PreAwake 안에서 만들었다 — 3단계). Spawn Transform 적용(17.3절, `WorldActors.cpp:505`)은 root에 그대로 걸린다.
- **component 파괴와 Subobject 필드**: World가 subobject를 파괴하면(등록 실패 정리 — `WorldActors.cpp:438-441`, `DestroyComponent`) owner의 그 Subobject 필드도 비운다(`ClearRootComponent`와 같은 규칙 — `WorldDestroys.cpp:13-19`). 남겨 두면 강한 참조가 파괴 대기 객체를 붙잡는다.
- **AddComponent**: Outer = 소유 Actor. 이름은 `<타입 도구 이름>_<n>`이고 n은 그 Actor 아래 `IsNameTaken`이 거짓인 가장 작은 0 이상의 수다(파괴 요청된 객체가 쥔 이름도 점유 — OF 1.1절, 3.3절 4항의 "가장 작은 빈 번호", UE `MakeUniqueObjectName`의 "클래스 이름 + 번호"와 같다 — `UObjectGlobals.cpp:2621-2633`). 이름 없는 생성은 대표 자식 규칙 때문에 쓸 수 없다(`ObjectRegistryNames.cpp:333-340`). 이름이 64 byte를 넘지 않게 base를 자르고, Sparks 타입 이름(`A.B`)은 마지막 조각만 쓴다. 생성 출처는 `Default`. Component마다 이름 색인 항목이 하나 늘어난다(메모리 비용 — 측정 대상).
- **레벨 편입(29절)**: graph 복원이 subobject를 만들고 배선하므로 편입은 지금처럼 `components`·`attachParent`에서 장부와 실행 상태를 만든다. component의 Outer가 그 목록을 가진 Actor인지 검사를 더한다(아니면 `InvalidArgument`).
- **ERD37 이전**: `actor_root_setup::PrepareRoot`를 쓰는 Actor(광원 5·환경 6·Trigger 3·StaticMeshActor 1 — 2026-11-28 목록)를 `ROCKS_PROPERTY(Subobject, Root)` + `InitSubobjectDefaults`로 옮기고 `OnFrameworkPreAwake`의 root 생성을 지운다. 다른 타입 root 거부(`InvalidArgument`)는 표가 타입을 정하므로 사라진다. `PrepareRoot`·`ActorRootSetup.h`는 쓰는 곳이 없어지면 지운다. `Light::LightComponentHandle`은 root가 아니라 선언 필드를 돌려준다.
- `WD/Actor.h` 머리 예시를 선언 subobject로 바꾼다(`OnAwake`의 `AddComponent`는 실행 중 추가 예로 남긴다).

### 10.8 단계

| 단계 | 내용 | 시험 |
|---|---|---|
| PF1a(끝 — 2026-11-28) | MetaSchema `SubobjectSchema`·클래스 지정자 칸·등록 검사·펼친 표, `RuntimeSubobjectOps`, MetaCompiler 지정자·진단·descriptor·`subobject` ID kind·`InitSubobjectDefaults` 탐지·생성 코드 | MetaCompiler 양성·음성(지정자 조합·Root 둘·Root+AttachParent·순환·타입 불일치·private·EditorOnly 타입·이름 규칙·kind 변경), 등록 검사(`SubobjectSchemaTests`), `DefaultSubobject_TableGeneratedWithStableGuid`. 모듈 경계 상속은 PF1e의 StaticMeshActor(WorldIntegration)가 실제 빌드로 덮는다 |
| PF1b(끝 — 2026-11-28) | ObjectRuntime `ObjectOrigin` 슬롯 표지, 타입을 지운 목록 staged 대입, `CreateDeclaredSubobjects`, `SubobjectDefaultsContext` 두 모드 | `DefaultSubobject_CreatedByInstancingNotConstructor`, `DefaultSubobject_TableReferencesResolve`, `DefaultSubobject_DerivedClassOverridesInheritedDefaults`, `DefaultSubobject_NoSharedMutableComponent`, 실패 rollback |
| PF1c | ObjectSerialization graph 형식 3(종류 1 노드·Include·도달 간선·목록 걸러내기·root/attach 비교·1단계 거부·2b 재짜기), subobject 기본값 레코드·노드별 기준 | `GraphV3_DeclaredSubobjectNodeRoundTrip`, `DefaultSubobject_RecordIsDeterministic`, `DefaultSubobject_TableReferencesEqualBaseline`, `DefaultSubobject_ClearedRootIsSaved`(null 강제 쓰기), `DefaultSubobject_ClassDefaultChangePropagatesToNonOverridden`, `DefaultSubobject_NewClassSubobjectAppearsOnLoad`, `DefaultSubobject_MissingSubobjectRejected`, `DefaultSubobject_NameConflictRejected`, `DefaultSubobject_RenameKeepsGuidAndOverride`, golden 재고정 |
| PF1d | World SpawnActor 한 트랜잭션·Component Outer·이름 규칙·subobject 장부·부착 함수 추출·Subobject 필드 비우기·편입 Outer 검사 | `DefaultSubobject_SpawnActorSingleTransaction`, `DefaultSubobject_ComponentOuterIsActor`, `AddComponent_NamesAreSmallestFreeNumber`, 기존 World 시험 |
| PF1e | ERD37 이전(15종), `PrepareRoot` 삭제, Actor 예시, 기존 시험 갱신(`WorldLightComponentTests.cpp:840-865` root 재사용·거부, `StaticMeshComponentTests.cpp:559-608`, `WorldEnvironmentComponentTests.cpp:1068` 부근, `WorldPrimitiveComponentTests.cpp:478-500`, 리플렉션 필드 수), 에셋 재쿠킹 | `DefaultSubobject_EngineActorRootSavedAsSubobject`, 광원·Trigger·환경 기존 시험 |

후속(PF1 밖): 타입 교체(`SetDefaultSubobjectClass` 대응), Component 안 subobject, EditorOnly subobject(`CreateEditorOnlyDefaultSubobject`), 에셋 클래스 subobject와 A1 Exports 칸, Sparks `let` component, `Runtime` 출처(PF5).

### 10.9 3.2절과 달라진 점

- 생성 출처: PF1은 `ClassSubobject`와 `Default`만 쓴다. `Runtime` 미저장은 PF5다. 그래서 ERD37 이전은 "저장에서 사라지지 않게"가 아니라 UE 구조(생성자 default subobject)와 기본값 전파를 위해서다.
- 표 참조의 같음 판정은 노드별 기준 제공자가 아니라 graph 저장의 일반 규칙이다(10.4절 — 목록 걸러내기, root·attach 사전 비교). 클래스 기본값 레코드에 참조 칸을 더하지 않는다.
- 객체 이름은 필드 도구 이름이고 스키마는 index 대신 FieldId Guid로 필드를 가리킨다.
- A1 Exports 칸은 이번에 바꾸지 않는다(10.1절).
- subobject 선택은 소유 필드 멤버 포인터다(`ROCKS_GENERATED_BODY`가 클래스마다 내용을 넣지 않는 고정 매크로라 생성 상수를 클래스 안에 둘 수 없다).

UE 대조: `AActor::OwnedComponents`는 UPROPERTY가 아니고 인스턴스에서 더한 component만 `InstanceComponents`에 저장한다(`Actor.h:4327-4331·4339-4340`) — 목록 걸러내기와 같은 구조다. `RootComponent`는 UPROPERTY이고 패키지 저장은 `PPF_DeepCompareInstances|PPF_DeepCompareDSOsOnly`로 비교해 같은 이름 DSO라도 내용이 다르면 쓴다(`PackageHarvester.cpp:225·231-234`, `PropertyBaseObject.cpp:112-153`). Rocks는 subobject 내용을 그 노드 Record로 따로 저장하므로 "표 대상이면 같음"으로 충분하다. UE는 DSO를 이름으로, Rocks는 Guid로 대응한다(의도된 차이 — 3.4절).

### 10.10 개정 기록

- 2026-11-28 초안 → 교차 검증 1차(치명 2·중요 9·사소 7) 반영: 도달 검사 암묵 간선(C1), root·attach 사전 비교와 null 강제 쓰기(C2), AddComponent 이름 규칙(I1), FieldId 기반 스키마·도구 이름(I2), `RuntimeSubobjectOps`·타입을 지운 목록 대입·런타임 체인(I3), 원소 생략 질의·재짜기 위치·1단계 거부(I4), 종류 1 순서 완화·Include(I5), 2a 계약·이름 충돌(I6), World 묶음 처리·EditorOnly 거부·Subobject 필드 비우기·World 없는 hook(I7), MetaCompiler descriptor·탐지·ID 경로(I8), 깨지는 시험·전체 재쿠킹(I9), 사소 7.
