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
| `CreateDefaultSubobject`(생성자, 이름) | 선언형 subobject 표(`ROCKS_PROPERTY(Subobject)`, SubobjectGuid = 소유 필드 FieldId), ObjectRuntime instancing이 생성 | 생성자는 객체를 만들지 않는다 |
| `SetDefaultSubobjectClass` | 파생 클래스의 subobject 타입 교체(base 타입의 파생만) | |
| `DoNotCreateDefaultSubobject` | (1차 없음 — `enabled` 덮기) | |
| `USimpleConstructionScript`·`USCS_Node` | subobject 표(C++ 지정자·Sparks `let`) + prefab 노드 표 | Blueprint는 도입하지 않음(EIP 20.4절) |
| `USCS_Node::VariableGuid` | SubobjectGuid·NodeGuid | |
| `UInheritableComponentHandler`·`FComponentKey` | 파생 클래스 subobject 기본값 덮기(SubobjectGuid 키), prefab variant | |
| `EComponentCreationMethod` | 객체 생성 출처(ClassSubobject·PrefabNode·InstanceAdded·Runtime) | |
| `FindFirstFreeName` | (D14 (b)일 때) 노드 이름 기반 빈 이름 찾기 | 1차는 Actor 이름 없음 |
| `FBlueprintCookedComponentInstancingData` | cooked 평탄화 원형(3.6절, D8) | |
| BP 클래스 실행 중 spawn | `SpawnPrefab`(Prefab 모듈, 추적하지 않음) | |
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

PF0(2026-11-28)에서 아래 행을 각 문서에 반영했다. 행 번호는 반영 당시다. "PF 단계"는 문서에 자리만 적고 해당 단계 구현 때 본문을 고칠 곳이다.

| 문서·위치 | 이 문서 | 반영 |
|---|---|---|
| RCM 43·73·274행 | default subobject는 선언형 표를 ObjectRuntime instancing이 만든다(3.2절, PF1) | 반영 — "prefab과 함께"를 PF1로 |
| RCM 58행(만드는 법 C++) | 선언 subobject 사이 참조는 레코드에서 null, 표가 정한 대상을 instancing이 써 넣는다 | 반영 — 레코드 밖 규칙, 같음 판정은 노드별 기준 제공자 |
| RCM 59행(생성자 결정성 계약) | 생성자는 하위 객체를 만들지 않는다, `InitSubobjectDefaults`도 같은 계약 | 반영 |
| RCM 68행(참조 기본값) | 표가 정한 자기 subobject 참조는 World 객체 참조가 아님 | 반영 |
| RCM 72행(기준의 일반화) | 첫 몫은 PF1 노드별 기준, 원형 기준·`PlanRevert` 인자는 PF4 | 반영 |
| RCM A5c 명세(Summary `ClassDefaults`) | prefab 패키지는 본문 타입·subobject 기본값 레코드 지문 행을 더하고 저장 hook이 내준다 | 반영(PF2에서 구현) |
| OF 5.1절 | graph v2 형식 3: NodeTable 노드 종류 칸 "선언 subobject", A1 Exports 같은 칸, 노드별 기준 | 반영 — 5.1절 머리에 예정 개정(PF1에서 본문 개정) |
| OF 5.6절 | `MaxPrefabNodes` = 4096, 인스턴스·cooked는 부분 그래프 여러 개 | 레벨 패키지 단계에서 함께 재검토(변경 없음) |
| `OR/PropertyPath.h:375` | 통째 쓰기는 지금 `Unsupported(AggregateValue)` → `ApplyPathWire`(PF4) | PF4 |
| RX 9.1절 | decode 정책 `skippedFlags`(PF2) | 반영 |
| RX 9.2절·20절 | Transient·Within 템플릿 예외는 생기지 않는다 | 반영 — 열린 문제 닫음 |
| RX 20절(key·원소 단위 차이) | prefab에서 Map·Set은 key 단위, 정적 배열은 통째(D5) | 반영 — prefab 몫 닫음, A5 몫은 남음 |
| RX 19절 소비자 표 | prefab 행(정규화·`ApplyPathWire`·`skippedFlags`) | 반영 |
| ER 4.2절(04-ObjectSystem 22행) | 부착 계층은 `attachParent`로 영속 | 반영(낡은 문구 정정) |
| ER 4.6절(Level·Actor 행) | Component Outer = Actor·선언 subobject·SpawnActor 한 트랜잭션(PF1), `spawnedActors`(PF3), `PrefabPlacement`·`placementNodeReferences`(PF5), Actor Entity Guid(PF7), 생성 출처 slot 표지 | 반영 — "prefab이 더할 것" 항목(PF 단계마다 본문 개정) |
| ER 7.3절 | 영속 상태 = 상대 Transform + `attachParent`, 앵커 부모 행렬(D10) | 반영(낡은 문구 정정, 앵커는 PF5) |
| ER 17.8·24.2.5절, ERD37 | 엔진 Actor root를 선언 subobject `Root`로 이전(PF1) | 반영 |
| ER 7.2·7.3절 | 편집 World는 게임 hook 없음(D18) | PF5 |
| ER 4.6절(ReadOnly) | override 적용은 staged 쓰기, 편집기 기록·제자리 전파는 CU5 | 모순 없음 — PF7 선행으로 기록 |
| `WD/Actor.h:26-46` | 예시의 `OnAwake` 생성은 Runtime 출처 | PF1에서 예시 갱신 |
| EIP 5.3절(525행) | 원형 본문은 보존, spawn이 복제 제외 정책, EditorOnly는 쿡에서 제외 | 반영 |
| EIP 20.3절(1820-1835행) | `SpawnPrefab(World&, AssetLease, SpawnPrefabParams)` 하나, SoftEntityId 시점(D12), ElementGuid 없음(D4), 순환 검사 넓힘, 파생 ID | 반영 — 20.3절 끝 정합 문단 |
| EIP 20.5절(1855행) | 주소 마지막 칸 = PropertyPath(필드 ID + key), 노드 ID 파생 | 반영 |
| EIP 2175행(바인딩) | 완전한 `PrefabNodeAddress` | 이미 일치 |
| SL 15절 | ElementGuid 없음(D4) | 이미 반영(SL 15-SaveAndData 37행) |
| AS 6.3절 | prefab 패키지는 일반 에셋 패키지, 본문 Soft·기준·중첩을 Hard로 | 반영 |
| AS 11절 | AssetManager Discard 완료 통지(지금 `OnReady`만) | 반영 — 자리(PF2에서 구현) |
| GCS 13.3절 | World 클러스터는 Level, spawn Commit 전 가입 | 이미 일치(GC-S7 병합) |
| CoreObjectInputDetailedPlan 606·735행 | SubobjectGuid = PF1 | 반영 |
| `StableIdManifest.cs`(MetaCompiler) | C++ 이름 키에 `subobject` kind | PF1 |

## 8. 확인하지 못한 UE 사실

2026-11-28에 1–3을 UE 5.8 원본으로 확정해 2절에 옮겼다(1 → 2.7절 `Enable` 진입점 없음, 2 → 2.5절 쿡 때 적용 경로, 3 → 2.9절 `bCanBeInCluster` 기본값 false).

1. Unity 내부 동작 — 소스가 없어 공개 직렬화 필드 이름만 근거로 썼다.

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
- 개정 6(2026-11-28): PF0 — 교차검증 2차 반영(치명 2·중요 8·사소 9).
  - 치명 C1: subobject 생성 규칙을 ObjectRuntime `ConstructionContext`로, prefab을 World 위 `Prefab` 모듈로, World는 staged 묶음 등록 키만(3.2·3.3절). C2: ERD37 엔진 Actor root 이전을 PF1 범위로(3.2절).
  - 중요 I1: PF1 결정 — `Subobject`·`Root`·`AttachParent` 지정자, Actor 소유 Component만, `SubobjectSchema` 표, SubobjectGuid = 소유 필드 FieldId(`subobject` kind), 표가 정하는 subobject 참조와 같음 판정, graph v2 형식 3, 노드별 차이 기준, 생성 출처 slot 표지, `InitSubobjectDefaults`. I2: SpawnActor 한 트랜잭션·Component Outer를 PF1로, `spawnedActors`의 Level 소속·`levelPosition`을 PF3로. I3: decode 정책은 거부 — `skippedFlags`(PF2). I4: `resolvedRevision`에 구성, PF8 시험 정정, `cookedGraphs`. I5: Summary `ClassDefaults`와 저장 hook. I6: `ApplyPathWire` 기반·`OverrideSet` 재사용·`PlanRevert` 일반화. I7: Event 필드 단위. I8: 한도 7개 추가·상수별 진단·`PrefabReferenceToRemovedNode`.
  - 사소: 행 번호 갱신, 클러스터 가입 시점, owner·instigator, 절대 성분, U32 참조 비트, README 착수 문구·GCS 인용.
  - 5.1절 상태 갱신, 7절 다른 문서 반영, 8절 UE 사실 1–3 확정(2.5·2.7·2.9절).
