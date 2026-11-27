# prefab 설계 — UE 대응표·다른 문서와 맞출 점·미확인·개정 기록

[목차](README.ko.md)

## 6. UE 대응표

| UE 5.8 | Rocks | 비고 |
|---|---|---|
| CDO(`RF_ClassDefaultObject`) | 클래스 기본값 레코드(ClassDefaults) | 객체 아님(RCM) |
| archetype(`GetArchetype`) | 해석된 원형 `PrefabArchetype`의 노드 실효 Record | 데이터(3.1절) |
| `FStaticConstructObjectParameters::Template` | spawn의 원형 Record 적용(3.3절 5항) | |
| `FObjectInstancingGraph` | graph v2 노드 번호 재연결 + 그래프 사이 참조 표 + `PrefabInstanceMap` | |
| `StaticDuplicateObject`·`FDuplicateDataWriter` | graph v2 전부 저장 + 제외 정책(RX 9.1절) | spawn 복원이 같은 제외 정책을 쓴다 |
| `CreateDefaultSubobject`(생성자, 이름) | 선언형 subobject 표(SubobjectGuid), World instancing이 생성 | 생성자는 객체를 만들지 않는다 |
| `SetDefaultSubobjectClass` | 파생 클래스의 subobject 타입 교체(base 타입의 파생만) | |
| `DoNotCreateDefaultSubobject` | (1차 없음 — `enabled` 덮기) | |
| `USimpleConstructionScript`·`USCS_Node` | subobject 표(C++ 지정자·Sparks `let`) + prefab 노드 표 | Blueprint는 도입하지 않음(EIP 20.4절) |
| `USCS_Node::VariableGuid` | SubobjectGuid·NodeGuid | |
| `UInheritableComponentHandler`·`FComponentKey` | 파생 클래스 subobject 기본값 덮기(SubobjectGuid 키), prefab variant | |
| `EComponentCreationMethod` | 객체 생성 출처(ClassSubobject·PrefabNode·InstanceAdded·Runtime) | |
| `FindFirstFreeName` | (D14 (b)일 때) 노드 이름 기반 빈 이름 찾기 | 1차는 Actor 이름 없음 |
| `FBlueprintCookedComponentInstancingData` | cooked 평탄화 원형(3.6절, D8) | |
| BP 클래스 실행 중 spawn | `World::SpawnPrefab`(추적하지 않음) | |
| `SerializeTaggedProperties`의 archetype 대비 차이 | 원형 대비 차이(A5 writer, 기준 = 원형 사슬) | |
| TMap key 단위 차이("Removed") | Map·Set key 단위 override | |
| `FOverriddenPropertySet`·`EOverriddenPropertyOperation` | `PrefabOverrideSet`(값·key 추가·제거·노드 추가·삭제) | Rocks는 런타임에도 쓴다 |
| `FOverridableManager` | 명시 override 집합(RCM 62행) | |
| `FPropertyNode::PropagatePropertyChange` | 제자리 갱신 전파(3.7절) | |
| `FBlueprintCompileReinstancer` 차이 재적용 | 재생성 전파(3.7절) | |
| `ALevelInstance`·`ILevelInstanceInterface` | `PrefabPlacement` + 추적 인스턴스 장부·앵커(D3) | Actor가 아니라 기록 |
| `ELevelInstanceRuntimeBehavior::Partitioned` | (후속 PF9 — 쿡 때 레벨에 굽기, D9 (b)) | 1차는 로드 때 spawn(D9 (a)) |
| `ELevelInstanceRuntimeBehavior::LevelStreaming` | (후속 PF9) | |
| `FLevelInstanceID`·`FActorContainerID` | `PrefabInstanceGuid` + 중첩 경로 | |
| `FActorInstanceGuid`(Combine) | 파생 Entity ID(`DeriveGuid`) | |
| `ULevelInstancePropertyOverrideAsset` | `PrefabPlacement.overrides` | Rocks는 런타임 데이터 |
| `ULevelInstanceSubsystem::CheckForLoop`·`FLevelInstanceActorDesc::ValidateCircularReference` | `PrefabCycle` 검사(3.8절) | 저장·로드·쿡 모두 |
| `APackedLevelActor`·`FPackedLevelActorBuilder` | (후속 PF9 — 렌더 instancing으로 굽기) | |
| `ULevel::CreateCluster`·`ULevelActorContainer` | Level 클러스터(GCS 13.3절) | Rocks는 spawn Commit에서도 가입 |
| Unity PropertyModification | 값 override 항목 | 경로는 문자열이 아니라 필드 ID Guid |
| Unity Added/Removed Components·GameObjects | 노드 추가·삭제 | |
| Unity Prefab Variant | variant(`base`) | |

## 7. 다른 문서와 맞출 점

이 문서는 다른 문서를 고치지 않았다. 아래는 PF0 확정 뒤 반영할 곳이다.

| 문서·위치 | 지금 문구 | 이 문서 | 맞출 방향 |
|---|---|---|---|
| RCM 59행 | 생성자는 Registry·World·다른 객체를 건드리지 않는다 | 그대로 지키고 subobject는 선언형(3.2절) | 문구 유지, "subobject는 선언으로 instancing이 만든다"를 더함 |
| RCM 60행 | C++ 레코드의 객체 참조는 구조적으로 null | subobject 지역 번호 참조는 허용 | 예외 하나 추가, subobject 기본값 레코드 정의 추가 (D1 (a) 선행) |
| RCM 68행 | 레코드의 참조 기본값은 null만(Sparks Asset 예외), World 객체 참조 거부 | 자기 subobject 지역 번호 참조는 World 객체 참조가 아님 | 허용 범위에 추가 (D1 (a) 선행) |
| RCM 43·73행 | "default subobject가 prefab과 함께 들어올 때 검증" | PF1(A5 뒤, prefab 에셋보다 먼저) | 단계 코드로 구체화 |
| RCM 69행 | Summary 지문은 "패키지 export 구체 클래스별" | prefab은 값으로 담긴 원형 본문 타입의 지문도 필요 | 지문 대상 넓힘 |
| OF 5.1절 | NodeTable 항목은 늘 새 객체를 만든다 | "기존 default subobject" 노드 종류 | graph v2 형식 개정(PF1) |
| OF 5.6절 | `maxObjects` 4096(레벨 패키지에서 다시 본다) | `MaxPrefabNodes` = 4096, 인스턴스·cooked는 부분 그래프 여러 개 | 레벨 패키지 단계에서 함께 재검토 |
| `OR/PropertyPath.h:362-363` | 통째 쓰기는 Unsupported(AggregateValue) | `ApplyPathWire`(경로 위치 부분 Record staged 디코드) | 새 API(PF4 선행) |
| RX 10.2절(812-814행) | `OverridePaths`는 Map·Set을 필드 경로로 정규화 | prefab 기록기가 key 단위를 따로 뽑는다(D16) | RX는 유지, "prefab은 key 단위를 직접 추출"을 소비자 표(19절)에 더함 |
| RX 9.1절 | 복제 저장이 DuplicateTransient 계열을 뺀다, 복제본 로드는 정책 없이 읽는다 | 원형 본문은 보존, spawn 복원이 decode 정책으로 뺀다(D20) | decode 정책의 제외 필드 건너뛰기 동작을 명시 |
| RX 9.2절 694행·20절 | "A5 클래스 기본값·prefab의 Transient·Within 템플릿 예외를 그 단계에서 다시 정한다" | 원형이 객체가 아니므로 예외가 생기지 않는다. Transient·Deprecated 클래스는 원형 노드 불가. Within은 노드 Outer 검사로 그대로 | 열린 문제를 닫음 |
| RX 20절 | "Map·Set·정적 배열의 key·원소 단위 차이 저장(A5·prefab)" | prefab override에서 Map·Set은 key 단위, 정적 배열은 통째(D5) | prefab 몫을 닫고 A5 패키지 차이 단위는 A5에 남김 |
| ER 4.2절·`World.cpp:425` | Actor·Component를 Outer·이름 없이 만든다 | Component Outer = 소유 Actor(subobject·노드 이름), Actor는 1차에 이름 없음(D14) | Component Outer 규칙 추가 |
| ER 4.6절(Level 행) | `Level.actors`는 Required 영속 필드(2026-11-20에 `levelScript` 추가 — [레벨 스크립트](../LevelScript.ko.md) LD4) | 펼친 Actor는 Transient `spawnedActors`(D15), 배치 기록 필드, 배치 노드 참조 표 `placementNodeReferences`(D22) 추가 | Level 필드 표 개정(레벨 저장 검증의 펼친 노드 참조 규칙은 ER 4.6절에 이미 반영) |
| ER 4.6절(Actor 행) | 영속 필드는 `components`·`rootComponent`·`enabled` | Unpack에 Actor 영속 Entity Guid 필드 필요 | World 단계에서 필드 추가 |
| ER 4.6절·7.3절 | "영속 상태는 상대 Transform뿐이고 계층은 저장하지 않는다" | 부착 계층 영속이 PF2 선행 | World 단계 결정 21에서 영속으로 정할 때 prefab을 소비자로 명시 |
| ER 7.3절 | 월드 행렬 = 상대 TRS × 부모 월드 행렬 | 부모 행렬 출처에 인스턴스 앵커 추가(D10) | SceneComponent 계약 개정 |
| ER 7.2·7.3절 | `FinishSpawning`이 등록과 `OnAwake`를 함께 | 편집 World는 게임 hook 없음(D18) | 편집 World 모드 추가 |
| ER 4.6절 | World 필드는 모두 ReadOnly, 경로 편집은 CU5 뒤 | override 적용은 staged 쓰기(허용), 편집기 override 기록·제자리 전파는 CU5 필요 | 모순 없음, PF7 선행으로 기록 |
| `WD/Actor.h:26-31` | 예시가 `OnAwake`에서 root component를 만든다 | 원형 component는 subobject 선언, `OnAwake` 생성은 Runtime 출처 | PF1 뒤 예시 갱신 |
| EIP 20.3절(1771-1781행) | `PrefabInstanceDesc{AssetId prefab; Transform root; PrefabOverrideSet}`, `InstantiatePrefab(World&, …)` | `World::SpawnPrefab(AssetLease, SpawnPrefabParams)` | World API로 두는 이유(WorldOnly 생성 키)·lease 인자·`tracked`를 EIP에 반영, 이름 하나로 통일 |
| EIP 20.3절(1771행) | "외부 world object는 SoftEntityId로 유지" | 1차는 저장 거부, 영속 Entity 참조가 생기면 SoftEntityId(D12) | 시점 차이 기록(최종 방향 같음) |
| EIP 20.3절(1784행) | "editor 노출 collection에는 stable ElementGuid를 부여하거나 keyed collection을 요구" | 배열은 통째, 원소 단위는 key 컨테이너(D4) | ElementGuid 선택지를 "필요 시 별도 설계"로 낮춤 |
| SL 15절 | (초안 1.2) "`@edit` 배열 원소에는 ElementGuid를 자동 부여한다" | ElementGuid 없음(D4) | SL 초안 1.3에서 정정 — 원소 식별은 key 컨테이너(Map·Set) 또는 struct 안 ID 필드 |
| EIP 20.5절(1804행)·2111행 | 주소 = PrefabInstanceGuid + 중첩 경로 + NodeGuid + FieldId + **ElementGuid** | 마지막 칸은 PathKey(key 컨테이너) | 주소 마지막 칸을 "PropertyPath(필드 ID + key)"로 |
| EIP 20.3절(1786행) | "reference cycle을 cooker가 거부" | 편집기 저장·로드·쿡 모두 거부 | 검사 시점을 넓힘(강화) |
| EIP 20.5절(1804행) | "instance EntityGuid는 저장 시 보존하고 새 복제 시 재발급" | 인스턴스 Guid만 보존, 노드 ID는 결정적 파생 | 노드 ID 파생 규칙(3.4절) 추가 |
| EIP 5.3절 509행 | "GUID/Outer·editor-only·transient field의 복사 정책을 명시" | 원형 본문은 보존, spawn이 복제 제외 정책, EditorOnly는 쿡에서 제외 | 3.3절 7항을 EIP 정책 표로 옮김 |
| AS 6.3절 | "레벨·prefab 패키지(후속)의 Dependencies(Hard)" | prefab 패키지는 새 섹션 없는 일반 에셋 패키지, 본문 Soft 참조를 Hard로 기록 | 일치. "prefab 패키지"가 별도 포맷이 아님을 명시 |
| AS 6.1절·11절 | 패키지 Discard·상주 단위 | 원형 캐시가 패키지 Discard 통지를 받는다 | A1 AssetManager에 통지 지점 추가 |
| GCS 13.3절(개정 2, `experiment/gc-scale`) | World 클러스터는 Level, spawn Commit에서 가입 | prefab spawn도 같은 경로 | engine-runtime에 개정 2가 병합되면 3.9절 인용을 그 판으로 고정 |
| CoreObjectInputDetailedPlan 12.2절(597·606행)·735행 C23 | "SubobjectGuid … prefab과 함께 후속" | PF1 | 단계 코드 연결 |

## 8. 확인하지 못한 UE 사실

1. `FOverridableManager::Enable()`을 처음 부르는 진입점(Engine/Source와 Plugins 범위에서 찾지 못함).
2. Level Instance 속성 override가 쿡 때 셀 데이터에 적용되는 최종 경로.
3. `AActor::bCanBeInCluster`의 기본값(명시 설정 코드를 찾지 못함).
4. Unity 내부 동작 — 소스가 없어 공개 직렬화 필드 이름만 근거로 썼다.

## 9. 개정 기록

- 개정 1(2026-11-17): 첫 초안.
- 개정 2(2026-11-17): 교차검증 1차 반영.
  - 치명 C1: 선언형 subobject(3.2절, D13), RCM 개정을 D1 (a) 선행으로.
  - 중요 I1: Outer·이름 규칙(3.3절 4항, D14). I2: `spawnedActors`(D15). I3: key 추출(D16). I4: `ApplyPathWire`. I5: 그래프 사이 참조(D17). I6: cooked 부분 그래프(D8). I7: 캐시는 lease 없음, 장부(D21). I8: 두 revision. I9: 앵커(D10). I10: 생성 출처·편집 World hook(D18). I11: 부분 Record 마이그레이션(D19). I12: 해석 입력 복사. I13: UE Partitioned 대응 정정(2.5절, D9). I14: SoftEntityId 차이(7절).
  - 사소 S1–S13: 2.2·2.4·2.5·2.9절 정정, DuplicateTransient 보존(D20), D10 사용성 비용, 점진 한도·override 추가 중첩의 순환 검사, 정본 하나, `DeriveGuid` 형식, Unpack 선행·Make variant 이동 의미, `transformSpace` 삭제.
  - 추가: 부착 부모 있는 spawn 합성, 장부 지연 생성.
  - UE 이름 짓기 근거는 리뷰가 든 `MakeUniqueObjectName`이 아니라 `FindFirstFreeName`(ActorConstruction.cpp:1126)으로 확인해 적었다.
- 개정 3(2026-11-17): 사용자 결정 — D1–D21 모두 추천안으로 확정(4절 머리).
- 개정 4(2026-11-19): [UI 툴킷](../UiToolkit/README.ko.md) 7.3절 (a) 편집 위치(`EditScope`) 규칙 반영 — 3.7절 Apply to source와 3.13절 Make variant는 `"Instance"` 필드 override를 옮기지 않는다, 3.11절에 편집 위치 위반 원인과 `PrefabEditScopeViolation`을 더했다. 편집 문맥은 저장 위치로 정한다(`PrefabAsset` = 원형, `PrefabPlacement` = 배치 인스턴스).
- 개정 5(2026-11-20): [레벨 스크립트](../LevelScript.ko.md) 확정에 맞춰 D22(레벨에 저장하는 참조가 펼친 노드를 가리킬 때 — LD17)를 더하고 3.6절에 배치 노드 참조 표, 3.11절에 `PrefabPlacementReferenceOrphaned`와 `PrefabUnsaveableNode`의 LevelScript 원인, 1.3절에 LevelScript 비범위(prefab·Level Instance 불가, Level Instance 도입 때 재검토), 7절 Level 행을 고쳤다.
