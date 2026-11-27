# prefab 설계 — 원형·인스턴스 생성·ID·override

[목차](README.ko.md)

## 3. Rocks 설계

### 3.1 원형 참조

**원형은 데이터다.** 원형은 Registry 객체가 아니라 노드 표와 노드별 Record로 된 불변 값 블록이다. 이유는 셋이다.

- Actor·Component·Level은 `ROCKS_CLASS(WorldOnly)`이고(ER 4.2·4.6절) 에셋 객체 Registry는 WorldOnly 타입 생성을 거부한다(AS 6.1절, OF 1.3절). UE처럼 원형 객체를 에셋 패키지에 살려 둘 수 없다.
- RCM은 살아 있는 CDO를 거부했다(가변 기준이 결정성을 깬다, WorldOnly 객체의 장부 밖 상주, `IsDefaultObject` 분기 — RCM 76행). 같은 이유가 원형 객체에도 그대로 적용된다.
- 원형을 GC가 보지 않으므로 원형 수와 무관하게 GC 비용이 없다(3.9절).

구성:

| 이름 | 무엇 | 수명 |
|---|---|---|
| `PrefabAsset` | 에셋 객체 Registry의 대표 에셋(WorldOnly 아님). 원형 본문·기준 prefab·revision을 리플렉션 필드로 담는다(3.6절) | 패키지(A1) |
| `PrefabNodeInfo` | 노드 하나의 부가 정보: NodeGuid, 노드 종류(객체·클래스 subobject·중첩 인스턴스), 클래스 subobject면 SubobjectGuid, 중첩이면 대상 prefab. 타입·Outer·이름은 graph v2 NodeTable이 정본이다(3.6절) | 값 |
| `PrefabArchetype` | **해석된 원형**: 기준 사슬과 중첩을 모두 펼친 부분 그래프 목록 + 노드 주소 색인 + 그래프 사이 참조 표 + 미리 해석한 override 경로. 불변, 공유 | 캐시(3.9절) |
| `PrefabContentRevision`·`PrefabResolvedRevision` | 자기 내용 지문과 해석 지문(3.6절) | 값 |

- 원형 참조는 `PrefabAsset`의 AssetId다. World 객체는 에셋을 `ObjectRef`로 가리킬 수 없으므로(AS 6.3절) 영속 참조는 `SoftObjectRef<PrefabAsset>`, 런타임 보유는 `AssetLease`다. lease는 인스턴스 장부가 보유한다(3.9절 — 원형 캐시는 lease를 쥐지 않는다).
- **클래스 원형**(선언형 default subobject, 3.2절)은 아래 층이다. prefab 원형은 그 위에 얹힌다. 적용 순서는 EIP 5.3절 그대로다: native 생성자 → Sparks 필드 초기값(= 클래스 레코드) → subobject 기본값 레코드 → prefab·variant → instance override → spawn 인자.

### 3.2 클래스 원형: 선언형 subobject

**문제.** UE는 생성자 안의 `CreateDefaultSubobject`로 하위 객체를 만든다(2.1절). Rocks 생성자는 Registry·World·다른 객체를 건드리지 못하고 C++ 레코드의 객체 참조는 구조적으로 null이다(RCM 59·60행). 생성자는 `Actor(Allocator&)` 하나뿐이다(`WD/Actor.h:60-61`). 지금은 component를 `OnAwake`에서 `AddComponent`로 만든다(`WD/Actor.h:26-31`). graph v2에는 "이미 만들어진 객체를 노드로 쓰는" 개념이 없다(OF 5.1절).

**설계: subobject를 선언으로 옮긴다**(UE SCS가 선언 트리인 것과 같은 꼴 — 2.4절, 4절 D13).

- **선언**: C++는 소유 필드 지정자로 선언한다 — `ROCKS_PROPERTY(Subobject, Parent = "root") ObjectRef<StaticMeshComponent> mesh;`(지정자 이름은 PF1에서 확정). Sparks는 `let` component 필드가 같은 선언으로 컴파일된다(SL 4.5절).
- **subobject 표**: MetaCompiler가 클래스마다 생성한다. 항목은 `{SubobjectGuid, 타입(TypeId), 이름, 부착 부모(SubobjectGuid 또는 소유 Actor의 root), 소유 필드(FieldId)}`이고 상속 클래스의 표는 base 표 + 자기 항목이다. SubobjectGuid는 `.rocksids.json`에 발급한다(TypeId·FieldId와 같다 — SL 12.2절). 이름은 표 안에서 유일하다(MetaCompiler 오류).
- **생성**: 생성자가 아니라 World instancing이 만든다. `ConstructionContext`에서 소유 객체를 만든 직후 표 순서대로 subobject를 staged로 만들고(Outer = 소유 객체, 이름 = 표 이름), 소유 필드에 staged 쓰기로 넣고, 부착 부모를 정한다. `SpawnActor`·`AddComponent`·그래프 복원·prefab spawn이 모두 이 한 경로를 쓴다.
- **subobject 기본값 레코드**: (소유 클래스 TypeId, SubobjectGuid)마다 하나다. 내용 = subobject 타입의 클래스 레코드 + 소유 클래스가 그 subobject에 준 기본값 덮기. C++는 소유 클래스의 정적 함수가 Registry 밖 임시 subobject 값에 쓰는 방식으로 덮기를 준다(RCM 58·59행의 레코드 생성과 같은 결정성 계약 — 두 번 실행해 인코딩 비교). Sparks는 `let` 초기값의 필드 지정이 덮기다.
- **subobject 지역 번호 참조**: 클래스 레코드·subobject 기본값 레코드 안에서 "이 소유 객체의 k번째 subobject"를 가리키는 참조 wire를 둔다(예: `Actor.rootComponent` 기본값 = subobject 1, 부착 부모 = subobject 2). 값은 소유 객체 기준 지역 번호이고 instancing이 실제 handle로 바꾼다. 이 참조만 레코드의 non-null 객체 참조로 허용한다 — **RCM 59·60·68행 개정이 필요하다**(7절, D1 (a)의 선행 조건).
- **파생 클래스의 덮기**: 파생 클래스는 상속 subobject의 기본값을 SubobjectGuid 키로 덮는다(UE ICH가 `AssociatedGuid`로 대응하는 것과 같다 — 2.4절). 타입 교체(UE `SetDefaultSubobjectClass`)는 같은 SubobjectGuid에 base 타입의 파생만 허용하고, 제거(UE `DoNotCreateDefaultSubobject`)는 1차에 두지 않는다(비활성화는 `enabled` 덮기).
- **graph v2 노드 종류 "기존 default subobject"**: NodeTable 항목이 새 객체를 만들지 않고 "노드 k(소유 객체)의 SubobjectGuid g" 를 가리킨다. 복원 2a는 소유 노드를 만들 때 생긴 subobject를 그 노드 번호에 묶고, 2b는 그 위에 Record를 적용한다. 레벨 저장·prefab 본문·복제가 모두 이 종류로 subobject를 저장한다(**OF 5.1절 개정 필요** — 7절).
- **실행 중 추가 component**: `OnAwake`·게임 코드가 `AddComponent`로 만든 component는 subobject가 아니다. 생성 출처가 Runtime으로 표시되고 원형·레벨 저장에 들어가지 않는다(3.3절 10항).

### 3.3 인스턴스 생성

공통 instancing 순서(EIP 5.3절: AllocateGraph → ApplyDefaults → Deserialize/Remap → Validate → pre-commit Initialize → Commit → Register → BeginPlay)를 prefab에 이렇게 대응한다.

1. **원형 확보**: `PrefabArchetype`을 캐시에서 얻는다(없으면 3.9절의 해석). 이 단계는 Registry를 건드리지 않는다.
2. **인스턴스 계획**: 원형 노드 − 삭제 노드 + 추가 노드로 노드 집합을 정하고 override를 노드 주소별로 묶는다. 한도(3.8절)를 여기서 검사한다.
3. **AllocateGraph**: World의 `ConstructionContext` 하나에서 부분 그래프마다 graph v2 단계형 복원(OF 5.4절)으로 객체를 staged로 만든다. 클래스 subobject는 3.2절 경로로 소유 객체와 함께 생기고 노드는 "기존 default subobject" 종류로 묶인다.
4. **Outer·이름**(4절 D14):
   - Component의 Outer는 소유 Actor, 이름은 노드 이름(subobject는 표 이름)이다. 한 Actor 안의 이름 유일성은 prefab 저장과 override 추가 때 검사하므로 spawn에서 충돌하지 않는다. 인스턴스가 여러 개여도 Outer(Actor)가 다르므로 충돌하지 않는다.
   - Actor는 지금처럼 Outer·이름 없이 만든다(`World.cpp:425`, ER 4.2절). 그래서 같은 원형을 두 번 spawn해도 이름 충돌이 없다. 레벨 패키지 단계가 Actor의 Outer를 Level로 정하면 spawn은 노드 이름에서 빈 이름을 결정적으로 찾는다(가장 작은 빈 번호 접미 — UE `FindFirstFreeName`과 같은 목적, 2.3절). 이름은 정체성이 아니므로 재생성 전파(3.7절)는 새 이름을 받아도 된다. 파괴 요청된 객체가 이름을 계속 차지하는 규칙(OF 1.1절)은 그대로이며, 재생성은 옛 이름 해제를 기다리지 않는다.
5. **값 적용**: 노드마다 원형 Record를 staged 객체에 적용한 뒤(두 단계 decode), override 값을 적용하고(3.5절 — 통째 단위는 부분 Record 디코드), 마지막으로 spawn 인자를 적용한다.
6. **내부 참조 재연결**:
   - 같은 부분 그래프 안: graph v2 노드 번호라서 복원이 새 handle로 바꾼다(OF 5.1절 — UE `SourceToDestinationMap` 역할).
   - 다른 부분 그래프의 노드(중첩 원형 사이, cooked 분할 그래프 사이 — 3.6절): 강한 참조는 graph v2 외부 참조로 쓰고 2b의 `imports`에 **같은 트랜잭션 객체**를 넘긴다(OF 5.4절이 허용한다). weak 참조는 graph v2에 weak import가 없으므로(RX 5.1절) Record에는 null로 쓰고 원형의 **그래프 사이 참조 표** `{부분 그래프, 노드, 필드 경로, 대상 노드 주소}`에 둔다. 모든 부분 그래프의 2b가 끝난 뒤 이 표를 staged 경로 쓰기로 적용한다(4절 D17).
   - override 값 안의 노드 참조: 노드 주소(3.5절)이며 인스턴스 대응표 `PrefabInstanceMap`(노드 주소 → handle, spawn 동안만 존재 — 3.9절)으로 바꾼다. 삭제된 노드를 가리키는 참조는 null이 되고 진단을 낸다.
   - 원형 밖 World 객체 참조는 둘 수 없다(3.5절, 4절 D12).
7. **제외 플래그**: 원형 본문은 편집 World 객체의 저장 가능한 값을 **모두** 보존한다(DuplicateTransient·NonTryoutDuplicateTransient 포함 — 다시 편집할 때 값이 사라지지 않게. UE 템플릿과 같다, 2.3절). spawn 복원은 decode 정책 `excludedFlags = DuplicateTransient | NonTryoutDuplicateTransient`로 그 필드를 건너뛰어 생성자 값으로 둔다(에셋 복제 정책과 같다 — RX 9.1절, 4절 D20). 지금 decode 정책이 Record에 있는 제외 필드를 건너뛰는지 거부하는지는 PF2에서 확인하고, 거부라면 "건너뛰기" 갈래를 더한다(5.1절). Transient 필드는 원래 저장되지 않는다. 클래스가 Transient·Deprecated인 객체(저장 불가 객체 — RX 9.2절)는 원형 노드가 될 수 없다(prefab 저장이 거부·진단). UE의 "CDO·archetype은 Transient 예외"(RX 694행)는 원형이 객체가 아닌 Rocks에서 생기지 않는다.
8. **Validate**: 실효 값 Required(OF 4절), 부착 계층 순환, Transform 유효성(유한·단위 quaternion — ER 4.6절 레벨 편입 5항), 소유 컨테이너 일관성.
9. **소유 목록**(4절 D15): prefab에서 펼친 Actor는 Level의 영속 목록 `actors`가 아니라 Transient 소유 목록 `spawnedActors`(`ROCKS_PROPERTY(Transient) ObjectRefArray<Actor>`)에 넣는다. `actors`는 Required 영속 필드라(ER 4.6절) 펼친 Actor가 들어 있으면 레벨 저장에서 그 Actor를 빼는 순간 집합 밖 강한 참조가 되어 graph v2 Reject로 실패한다(OF 5.3절). Transient 목록은 저장되지 않고 GC 소유 간선 역할만 한다(생성 Trace가 Transient 참조 필드를 방문해야 한다 — 5.1절에서 확인). 레벨에 직접 배치한 Actor만 `actors`에 남는다.
10. **생성 출처 표시**: 객체마다 NATIVE 생성 출처를 둔다 — `ClassSubobject`·`PrefabNode`·`InstanceAdded`(인스턴스 override로 추가한 노드)·`Runtime`(hook·게임 코드의 `SpawnActor`·`AddComponent`). UE `CreationMethod`와 같은 목적이다(2.1절). 원형 본문과 레벨 저장은 Runtime 출처를 넣지 않는다. 편집 World에서는 게임 hook을 실행하지 않으므로(3.13절, 4절 D18) Runtime 출처가 생기지 않는 것이 정상이고, 이 표시는 그 계약의 방어선이다.
11. **pre-commit Initialize → Commit**: 최상위 Actor들을 root로 다중 root Commit 한 번(OF 1.6절). Commit 전 실패는 staged 그래프 전체를 rollback한다.
12. **Register → hook**: World가 Actor마다 `ActorRecord`를 만들고 클러스터에 가입시킨다(3.9절). hook은 spawn 묶음 단위다 — 묶음의 모든 Actor·Component `OnAwake`가 성공하면 노드 순서로 `OnEnable`, `OnStart`는 기존 규칙(ER 7.2절)을 따른다(SL 5.1절). Commit 뒤 `OnAwake` 실패는 묶음 전체를 RequestDestroy 경로로 정리한다(EIP 5.3절 — 공개된 객체를 rollback 경로로 지우지 않는다).

API(이름은 PF3에서 확정 — EIP 20.3절의 `InstantiatePrefab`·`PrefabInstanceDesc`와 맞출 점은 7절):

```cpp
struct SpawnPrefabParams {
    ObjectHandle level;                 // null이면 영구 Level(SpawnParams와 같다)
    Transform transform;                // 인스턴스 원점 Transform(3.10절 — 부착 부모가 있으면 그 기준)
    ObjectHandle attachParent;          // 선택. 최상위 root를 붙일 다른 Actor의 SceneComponent
    Guid instanceGuid;                  // 빈 값이면 새로 발급(3.4절)
    bool tracked = false;               // 인스턴스 장부·노드 대응표·앵커를 둔다(편집기·레벨 배치는 늘 true)
    const PrefabOverrideSet* overrides = nullptr;  // 레벨 배치 인스턴스 로드·편집기 생성용
};
// World API다(World registry의 WorldOnly 생성 키를 가진 경로 — ER 4.6절 정책 자리).
Result<PrefabSpawnResult> World::SpawnPrefab(const AssetLease& prefab, const SpawnPrefabParams&) noexcept;
```

`PrefabSpawnResult`는 인스턴스 Guid와 최상위 Actor handle 목록을 담는 **복사 가능한 값**이다. lease를 쥐지 않고 handle은 대상 파괴 뒤 stale이 된다. 추적 인스턴스(`tracked`)는 `PrefabInstanceId`(World 인스턴스 장부의 slot + generation)를 함께 돌려주며, `World::FindPrefabNode(PrefabInstanceId, PrefabNodeAddress)`로 노드 객체를 찾는다. 추적하지 않는 인스턴스는 노드 대응표를 남기지 않으므로 노드 조회가 `Unsupported`다(3.9절).

### 3.4 하위 객체 안정 ID와 이름

| ID | 발급 | 쓰는 곳 |
|---|---|---|
| `SubobjectGuid` | MetaCompiler가 subobject 선언마다 `.rocksids.json`에 발급 | 클래스 subobject의 정체성, 파생 클래스 덮기 키(UE ICH `AssociatedGuid` 대응) |
| `NodeGuid` | prefab 편집기가 노드를 만들 때 무작위 발급, 원형 수명 동안 불변(이름 변경·이동과 무관) | 원형 노드의 정체성, override 대상 |
| `PrefabInstanceGuid` | 인스턴스 생성 때 발급, 저장 시 보존, 복제(편집기 복사·Tryout이 아닌 복제) 때 재발급 | 인스턴스 정체성 |
| `PrefabNodeAddress` | `{중첩 인스턴스 경로: NodeGuid 목록, 노드: NodeGuid}` | override 대상, 바인딩(EIP 2111행) |
| 파생 Entity ID | `DeriveGuid(PrefabInstanceGuid, 주소)` — 결정적 | 세이브게임·외부 참조·네트워크 |

- **이름은 정체성이 아니다.** UE는 원형과 인스턴스를 같은 FName으로 대응하지만(2.1절) Rocks의 Outer 이름은 생성 뒤 불변이고(OF 1.1절) 편집 중 이름 변경을 허용해야 하므로 GUID로 대응한다. 노드 이름은 표시와 객체 경로용이다(이름 규칙은 3.3절 4항).
- 클래스 subobject 노드의 NodeGuid는 `DeriveGuid(소유 노드 NodeGuid, SubobjectGuid)`로 정한다. 그래서 클래스에 subobject가 추가돼도 원형 노드 표를 고치지 않고 주소가 안정하다.
- 중첩: 원형 B를 품은 원형 A에서, B 안 노드의 A 기준 주소는 `{[B 인스턴스 노드의 NodeGuid], B 안 NodeGuid}`다. 같은 원형을 두 번 중첩해도 경로가 달라 충돌하지 않는다(EIP 20.5절, UE `FLevelInstanceID`가 조상 GUID를 결합하는 것과 같은 목적 — 2.5절).
- **파생 Entity ID는 저장하지 않는다.** UE `FActorInstanceGuid`가 컨테이너 ID와 ActorGuid를 결합하듯(2.5절) 인스턴스 Guid와 주소에서 매번 같은 값을 만든다. 인스턴스 Guid가 보존되면 모든 파생 ID가 보존되고, 복제로 재발급되면 함께 바뀐다(EIP 20.5절의 "저장 시 보존, 새 복제 시 재발급"을 노드 단위로 만족). 인스턴스 안에서 추가한 노드는 그 인스턴스가 발급한 NodeGuid를 쓰므로 같은 식이 그대로 적용된다.
- **`DeriveGuid` 형식**: Core 공용 SHA-256 위에 둔 결합 함수이며 기능 모듈에 따로 만들지 않는다. 입력은 도메인 구분 문자열(`"Rocks.Prefab.NodeId.v1"`, `"Rocks.Prefab.SubobjectNode.v1"`처럼 용도마다 다르다)과 각 입력의 길이 접두(u32) + byte를 이어 붙인 것이다. 주소는 경로 원소 수 + 각 NodeGuid 16 byte로 쓴다. 출력은 앞 16 byte다. 도메인과 길이 접두가 있어 서로 다른 입력 조합이 같은 byte열을 만들 수 없다.
- 배열 원소에는 안정 ID를 두지 않는다(4절 D4). 원소 단위 override는 key 컨테이너(`OrderedMap`·`OrderedSet`)의 key가 안정 ID 역할을 한다(RX 1.5·7절).

### 3.5 override 기록

`PrefabOverrideSet`은 인스턴스(또는 variant)가 원형과 다른 점의 목록이다.

| 항목 | 내용 | UE·Unity 대응 |
|---|---|---|
| 값 override | `{PrefabNodeAddress, 소유형 PropertyPath(필드 ID Guid 단계 + PathKey), 단위 값 wire}` | `FOverriddenPropertySet` Modified·Replace, Unity PropertyModification |
| key 추가·제거 | Map·Set의 `{주소, 필드 경로, key, 값}` 추가·변경과 `{주소, 필드 경로, key}` 제거 | TMap key 단위 차이(2.2절), OS 맵 Add·Remove(2.7절) |
| 노드 추가 | `{부모 주소, 새 NodeGuid, 타입, 이름, 전체 Record}` — 추가한 Component·Actor·중첩 인스턴스 | OS ArrayAdd, Unity AddedComponents·AddedGameObjects |
| 노드 삭제 | `{주소}` — 부분 트리째 | OS ArrayRemove(원형 ID), Unity RemovedComponents |

- **단위**는 RX 10.2절 `OverridePaths`의 정규화를 따른다: Atomic struct는 struct 경로, 비Atomic struct는 멤버 끝 경로(`Transform`은 멤버 단위 — 이동만 덮고 회전·크기는 원형을 따른다, RX 301행), 단일 값은 끝 경로, Array·StaticArray·Optional·InstancedStruct는 필드 통째. **Map·Set만 다르다**: prefab override는 key 단위다(4절 D5 — UE 표준 TMap 차이도 key 단위다, 2.2절). 같은 필드에 통째 override와 key override가 함께 있으면 통째가 이긴다(저장이 key 항목을 지운다).
- **Map·Set key 단위 추출**(4절 D16): RX 10.2절 `Publish`의 override 경로는 Map·Set을 필드 통째 경로로 정규화하므로(RX 812-814행) 그대로 쓰면 key 단위가 사라진다. prefab 기록기는 `Publish`가 돌려주는 옛 값과 게시 뒤 값을 key로 비교해 추가·변경·제거 key를 직접 뽑는다. RX 10.2절은 바꾸지 않는다(다른 소비자 — A5 패키지 차이 — 는 통째가 맞다). 편집기의 경로 쓰기(대입 묶음 밖)도 같은 옛 값/새 값 비교를 쓴다.
- **통째 단위 값의 적용**: 지금 경로 쓰기는 struct·배열을 통째로 쓰면 `Unsupported(AggregateValue)`다(`OR/PropertyPath.h:362-363`). 그래서 Atomic struct·Array·StaticArray·Optional·InstancedStruct 단위 override를 적용할 수단이 없다. **"경로 위치에 그 단위의 부분 Record(wire)를 staged 디코드"하는 API**를 PF4 선행으로 정의한다: `ApplyPathWire(ConstructionContext&, handle, PropertyPath unit, Span<const uint8> wire)` — 그 위치의 필드·멤버 codec으로 두 단계 decode(검증 → 할당 외 실패 없는 적용)를 하고, 참조 원소는 staged 쓰기 규칙(삽입 장벽·대상 검사)을 지난다. 단일 값 단위도 같은 API로 적용해 경로 하나로 통일한다.
- **무엇을 기록하나**: RCM 62행 그대로 **명시 override 집합 ∪ 원형과 인코딩이 다른 값**이다. 편집기와 경로 쓰기가 집합에 기록하고, 저장 때 모든 노드를 원형 Record와 비교해 다른 값을 더한다. 원형과 같은 값을 일부러 넣은 경우도 집합에 남아 원형이 바뀐 뒤 유지된다(2.7절). "원형으로 되돌리기"는 원형 값을 쓰고(기록하지 않는 쓰기) 집합에서 지운다.
- **같음**은 RCM 63행 정의(wire byte, non-null 참조는 대상 정체성)다. 노드 참조의 정체성은 노드 주소이고, 죽은 weak handle은 null로 정규화한 뒤 비교한다(RX 5.1절). byte 비교가 의미를 갖도록 원형 값을 변환해 쓰는 경로(예: 최상위 root의 월드 Transform 구움)를 추적 인스턴스에 두지 않는다(3.10절).
- **소유 컨테이너는 override 대상이 아니다.** `Actor.components`·`Level.actors`·`Level.spawnedActors`처럼 소유 관계를 나타내는 필드는 노드 구조(추가·삭제)에서 다시 만든다. `Actor.rootComponent`처럼 노드를 가리키는 참조 필드는 일반 값 override다.
- **참조 값 표기**: override 값 안의 강한·weak 참조는 노드 주소 표의 번호로 쓴다(graph v2 노드 번호와 같은 고정 폭 U32 — 0 = null). 노드 주소 표는 override 집합 머리에 둔다. 에셋 참조(Soft·AssetId)는 값 그대로다. **원형 밖 World 객체를 가리키는 참조는 1차에 저장할 수 없다**(4절 D12). EIP 20.3절(1771행)은 이를 `SoftEntityId`로 유지하라고 하므로, World 단계의 영속 Entity 참조가 생기면 그 값으로 저장한다(7절).
- **노드 추가·삭제 규칙**:
  - 삭제는 원형 노드만 대상이다(추가한 노드는 그냥 목록에서 뺀다 — UE ArrayRemove가 앞의 Add를 지우면 no-op인 것과 같다, 2.7절). 삭제한 노드의 하위 노드도 삭제된다. 하위 노드를 대상으로 한 override는 고아가 된다(3.7절).
  - 클래스 subobject 노드는 삭제할 수 없다(클래스 선언이 만든다). 비활성화는 `enabled` 값 override로 한다.
  - 루트가 하나뿐인 원형의 루트는 삭제할 수 없다.
  - 추가 노드의 부모는 원형 노드나 다른 추가 노드다. 추가 노드의 Record는 기준이 클래스 레코드인 전부 저장 형식(RCM 66행 누락 = 기준)이다. 추가 노드의 이름은 부모 Actor 안에서 유일해야 한다(추가 때 검사).
- **reparent**: 부착 계층이 리플렉션 필드(WeakObjectRef 부모 — ER 4.6절, RF 결정 21)로 영속된 뒤에는 reparent가 부모 필드의 값 override다. Outer(소유 Actor)를 바꾸는 이동은 없다 — Outer는 불변이다(OF 1.1절).
- **타입 버전 기록**: override 집합은 대상 노드 타입마다 저장 당시 스키마 버전을 기록한다(마이그레이션 판정용 — 3.7절).
