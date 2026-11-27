# prefab 설계 — 결정·선행 조건·단계·시험

[목차](README.ko.md)

## 4. 결정 항목

2026-11-17 사용자 확정: 모든 항목을 아래 "추천" 열대로 정한다. D1은 클래스 원형 + prefab 에셋((a)+(b), Level Instance식 별도 Level 로드는 후속 — (a)는 RCM 생성자 계약 개정이 선행), D9는 1차에 배치 기록 유지(UE식 쿡 때 굽기는 후속 최적화)다. 7절 "다른 문서와 맞출 점"은 PF0 착수 때 함께 고친다. D22는 2026-11-20 [레벨 스크립트](../LevelScript.ko.md) 결정(LD17, 권장안)과 함께 확정했다.

| ID | 질문 | 선택지 | 추천 |
|---|---|---|---|
| D1 | 1차로 무엇을 하나 | (a) 클래스 원형(선언형 default subobject — UE SCS 대응)만 (b) prefab 에셋(데이터 원형, Actor 여러 개 가능 — Unity prefab·UE BP spawn·Level Instance 배치 대응) (c) Level Instance식 별도 Level 로드 (d) 전부 | **(a)+(b)를 1차로 함께**. (a)는 (b)의 선행이고 Sparks `let` component가 기다린다(SL 4.5절). (a)는 RCM 59·60·68행 개정(3.2절)이 선행이다. (c)는 레벨 패키지·스트리밍 단계(EIP 2064행 P2·M7)와 함께, Packed 대응은 렌더 instancing 뒤 |
| D2 | 원형을 무엇으로 두나 | (a) 불변 데이터(노드 표 + Record) (b) 별도 템플릿 Registry의 살아 있는 객체(UE archetype 방식) | **(a)**. WorldOnly 제약·RCM의 CDO 거부 이유가 그대로 적용되고 GC 비용이 0이다(3.1절). 편집은 편집 World로 한다 |
| D3 | 레벨 배치 인스턴스를 어디에 기록하나 | (a) Level의 배치 기록 struct 배열 `PrefabPlacement` (b) UE `ALevelInstance`처럼 인스턴스마다 전용 Actor | **(a)**. 인스턴스마다 Actor 하나를 더 만들지 않는다. (b)의 장점(Actor 검색·선택·기즈모)은 편집기 Outliner가 배치 기록을 묶음 항목으로 보여 주고 앵커(3.10절)를 기즈모로 움직이는 것으로 대신한다 |
| D4 | 배열 원소 override | (a) 배열은 통째, 원소 단위는 key 컨테이너로 (b) 배열 원소에 ElementGuid 도입 | **(a)**. index override는 금지한다(Unity 약점 — 2.10절). ElementGuid는 리플렉션 값 형식·텍스트·경로를 모두 바꾸므로 실제 요구가 생기면 따로 설계한다 |
| D5 | Map·Set override 단위 | (a) 필드 통째(RX 지금 규칙) (b) key 단위 추가·변경·제거 | **(b)**. UE 표준 TMap 차이 저장이 key 단위다(PropertyMap.cpp:355-368 — 2.2절). 형식은 PathKey로 이미 표현된다(RX 1.5절) |
| D6 | 펼친 객체의 ID | (a) 인스턴스 Guid + 주소에서 결정적 파생 (b) 노드마다 Guid 저장 | **(a)**. UE `FActorInstanceGuid`와 같은 방식(2.5절). 저장량이 없고 복제 시 재발급이 자동이다 |
| D7 | 편집기 전파 방식 | (a) 구조 같으면 제자리 갱신, 다르면 재생성 (b) 늘 재생성 | **(a)**. 편집 중 선택·undo 기록·hook 상태를 지킨다. CU5(setter hook) 전에는 (b)만 가능하므로 PF7 착수 때 다시 본다 |
| D8 | cooked 원형 | (a) 쿠커가 평탄화, 부분 그래프 여러 개(각 4096 이하) + 노드 주소 표 + 그래프 사이 참조 표 (b) 평탄화하되 4096을 넘으면 쿡 오류 (c) 런타임에 사슬 해석 | **(a)**. UE cooked 빠른 경로와 같은 목적(2.4절). 그래프 사이 참조 규칙은 편집기 해석과 같아(3.3절 6항) 추가 코드가 적다. (b)는 큰 중첩 prefab을 막는다 |
| D9 | 레벨 배치 인스턴스의 cooked 형태 | (a) 배치 기록 유지, 로드 때 spawn (b) 쿠커가 펼친 Actor를 레벨 패키지에 굽기 | **1차 (a)**. **UE는 (b)다** — Partitioned Level Instance는 WorldPartition 생성·쿡 때 셀에 편입되고 LI Actor는 런타임에 쓰이지 않으며 override도 EditorOnly다(2.5절). Rocks는 레벨 패키지 export 규칙이 정해진 뒤 (b)를 측정해서 고른다(로드 시간 대 패키지 크기) |
| D10 | 최상위 root의 인스턴스 Transform | (a) 추적 인스턴스는 앵커 행렬, 추적하지 않는 실행 중 spawn은 구움(shear면 실패) (b) 늘 앵커 (c) 늘 구움(shear면 실패) | **(a)**. 구움은 byte 비교 override 판정을 깨므로 저장되는 인스턴스에 쓸 수 없다(3.10절). 앵커는 shear 제약도 없앤다. 실행 중 spawn은 게임 코드의 "부모 없는 Actor의 상대 = 월드" 규약을 지키려고 굽는다 — 그 대가로 회전된 최상위 root를 가진 prefab을 비균일 scale로 실행 중 spawn하면 실패한다(필요하면 `tracked`). (b)는 게임 코드가 앵커를 의식해야 하고, (c)는 비균일 scale 배치를 막고 가짜 override를 만든다 |
| D11 | 최상위 Actor 수 | (a) 여러 개 허용 (b) 하나(Unity) | **(a)**. UE Level Instance가 여러 Actor 묶음이다. 단일 Actor 원형은 (a)의 특수한 경우다 |
| D12 | 원형 밖 World 객체 참조 | (a) 영속 Entity 참조(`SoftEntityId`)가 생길 때까지 저장 거부, 생기면 그 값으로 저장 (b) 지금 weak로 저장하고 로드 때 null | **(a)**. 조용히 잃지 않는다. EIP 1771행의 SoftEntityId 방향과 최종적으로 같다 |
| D13 | default subobject 만드는 법 | (a) 선언형: MetaCompiler subobject 표 + World instancing이 생성, subobject 기본값 레코드, 지역 번호 참조, graph v2 "기존 subobject" 노드 (b) UE처럼 생성자가 만든다(생성자에 ConstructionContext 접근 허용) | **(a)**. (b)는 RCM 59행 생성자 결정성 계약과 레코드 생성 방식(Registry 밖 임시 인스턴스)을 깬다. (a)는 UE SCS와 같은 꼴이다 |
| D14 | Actor의 Outer·이름 | (a) 지금처럼 Actor는 Outer·이름 없음, Component Outer = 소유 Actor (b) UE처럼 Actor Outer = Level, spawn이 빈 이름을 결정적으로 찾음 | **1차 (a)**. 이름 충돌이 원천적으로 없고 1M Actor에 이름 색인 비용이 없다. 레벨 패키지 단계가 export를 Outer 사슬로 정해야 하면 (b)로 바꾸며 이름 규칙은 3.3절 4항에 이미 정했다 |
| D15 | 펼친 Actor의 소유 목록 | (a) Level에 Transient 소유 목록 `spawnedActors`를 따로 둔다 (b) `actors`에 두고 레벨 저장이 prefab 소속 Actor를 원소째 빼는 저장 정책을 더한다 | **(a)**. 영속 목록과 저장 집합이 어긋나지 않는다. (b)는 graph v2에 "집합 밖 소유 원소를 지우는" 새 정책이 필요하다 |
| D16 | Map·Set key 단위 추출 위치 | (a) prefab 기록기가 옛 값/새 값을 key로 비교 (b) RX 10.2절 `OverridePaths`를 key 단위로 바꿈 | **(a)**. RX를 쓰는 A5 패키지 차이는 통째가 맞으므로 RX를 바꾸지 않는다 |
| D17 | 부분 그래프 사이 참조 | (a) 강한 = 같은 트랜잭션 import, weak = 그래프 사이 참조 표 + 사후 경로 쓰기 (b) graph v2에 같은 트랜잭션 weak import를 더한다 | **(a)**. graph v2 형식을 바꾸지 않는다. weak 사후 쓰기는 Commit 전 staged 쓰기라 원자성이 유지된다 |
| D18 | 편집 World의 hook | (a) 등록과 `Validate`만, 게임 hook 없음 (b) Tryout처럼 모든 hook 실행 | **(a)**. UE 편집기 World도 BeginPlay를 하지 않는다. hook이 만든 객체가 원형·레벨에 섞이지 않는다 |
| D19 | override 마이그레이션 | (a) 노드별 부분 Record로 묶어 migrate 후 경로로 다시 추출, key 항목은 그 필드가 바뀌면 고아 (b) 버전이 다르면 그 노드 override를 곧바로 고아로 | **(a)**. 부분 Record가 형식상 유효하고 migrate가 Record 단위라 그대로 쓸 수 있다. (b)는 버전 하나 올릴 때마다 모든 레벨의 override를 사람이 다시 손봐야 한다 |
| D20 | DuplicateTransient 계열 값 | (a) 원형 본문은 보존, spawn 복원이 제외 (b) 원형 저장 때 제외 | **(a)**. UE 템플릿도 값을 보존한다(2.3절). (b)는 다시 편집할 때 값이 사라진다 |
| D21 | 인스턴스 장부 범위 | (a) 원형별 `PrefabUsage` + Actor별 칸은 늘, 인스턴스 항목·노드 대응표는 추적 인스턴스만(지연 생성) (b) 모든 인스턴스에 인스턴스 항목 | **(a)**. 1M 규모에서 실행 중 spawn의 장부 비용을 Actor당 고정 byte로 묶는다 |
| D22 | 레벨에 저장하는 참조가 배치 인스턴스의 펼친 노드를 가리킬 때 | (a) Record는 null, Level 배치 노드 참조 표에 (instanceGuid + 노드 주소)로 저장하고 펼친 뒤 D6 규칙으로 해석 (b) 펼친 노드 참조 금지(Unpack 뒤에만 참조) | **(a)**. 펼친 Actor는 Transient 목록(D15)이라 직접 저장할 수 없다. 레벨 스크립트·직접 배치 Actor가 prefab으로 만든 문·트리거를 가리키는 흔한 경우를 Unpack 없이 지원하고, 파생 ID가 결정적이라(D6) 저장량이 주소뿐이다. LevelScript만이 아니라 레벨 저장 참조 일반 규칙이다(3.6절). 영속 Entity Guid가 생기면 그 표현으로 바꾼다 |

## 5. 선행 조건과 단계 계획

### 5.1 선행 조건

| 선행 | 내용 | 상태(2026-11-28) |
|---|---|---|
| 리플렉션 B9 | WeakObjectRef PROPERTY(B9d1), Map·Set·PathKey(B9f), 필드 플래그 DuplicateTransient 계열(B9h1), 저장 불가 객체(B9h2), 대입 묶음·`OverridePaths`(B9i), InstancedStruct(B9j), struct 상속(B9k) | 끝(B9a–B9m) |
| A1 | 패키지 포맷 v1·`Package`·AssetManager·AssetLease·Dependencies(Hard)·패키지 Discard 통지 | 구현. **Discard 통지 구독 API는 없다**(AssetManager에 `OnReady`만) — PF2에서 더한다 |
| A2 | 저장·복제(`asset-duplicate` 제외 정책)·인덱스 | 구현 |
| A5 | 클래스 기본값 레코드·차이 쓰기·override 집합·지문, 차이 형식의 "기준(base)" 명시 API(RCM 72행) | A5a 레코드·A5b 차이 쓰기와 `OverrideSet`·A5c Summary 지문 구현. 기준 일반화는 남음 — 노드별 기준 제공자(PF1), `PlanRevert` 기준 인자(PF4) |
| RCM 개정 | 58·59·68행: 선언형 subobject, 표가 정한 subobject 참조, subobject 기본값 레코드(3.2절) | PF0에서 반영(7절) |
| ObjectRuntime·Serialization | graph v2 "선언 subobject" 노드 종류·형식 3(PF1), `ApplyPathWire`(PF4), decode 정책 `skippedFlags`(PF2), Transient 참조 필드의 GC Trace 방문 | Trace 방문 확인(`CppEmitter.cs:487-491`). decode 정책은 지금 제외 필드를 거부(3.3절 7항). 나머지 미구현 |
| MetaCompiler | `Subobject` 지정자·subobject 표·C++ `subobject` ID kind·`InitSubobjectDefaults` 탐지 | 미구현(Sparks `subobject` kind만 있음) |
| World 단계 | 부착 계층 영속, 레벨 편입 계약, SpawnActor 한 트랜잭션·Component Outer(PF1), staged 묶음 등록 키 `worldOnlyCreationKey`, `spawnedActors`, 생성 출처, 앵커 부모 행렬, 편집 World 모드, Actor 영속 Entity Guid(Unpack), 영속 Entity 참조(셀 사이·D12) | 부착 계층 영속 끝(`attachParent`, ER 29절). GC-S7 Level 클러스터 병합. 나머지 미구현 |
| 레벨 패키지 | WorldOnly 그래프의 패키지 저장·export 집합 규칙(OF 5.6절 "레벨 패키지(WorldOnly, 후속)"), 배치 기록 필드 | 후속 |
| CU4·CU5 | 소유형 경로 `OwnedPropertyPath`와 Record 형식(RX 20절), Undo transaction, setter·통지 hook(World 필드 ReadOnly — ER 4.6절) | CU4 미구현. CU5 진행 중(변경 통지 CN1a만, World setter hook 없음) |
| Sparks L2 | component `let` 필드 초기값(SL 4.5절) — PF1과 함께 열린다 | 언어 코어 LC 끝, 엔진 통합 EI 대기 |
| GC-S7 | Level 클러스터(GCS 13절) | 병합 |

### 5.2 단계

| 단계 | 내용 | 규모 | 선행 |
|---|---|---|---|
| PF0 | (설계 확정) 교차검증 2차 반영, 4절 결정, 한도 값 확정, 7절 다른 문서 개정 — **끝(2026-11-28, 개정 6)** | S | — |
| PF1 | (클래스 원형 — 3.2절) MetaCompiler `Subobject` 지정자·subobject 표·C++ `subobject` ID kind, ObjectRuntime `ConstructionContext`의 subobject 생성, SpawnActor 한 트랜잭션·Component Outer = Actor, subobject 기본값 레코드(`InitSubobjectDefaults`, 결정성 대조), 표 참조와 같음 판정, graph v2 형식 3 "선언 subobject" 노드·A1 Exports 칸, 노드별 차이 기준, 파생 클래스 덮기, 생성 출처 slot 표지, ERD37 엔진 Actor root 이전 | L | A5(끝), RCM 개정(PF0 반영), Sparks L2(Sparks 부분만) |
| PF2 | (원형 데이터 형식) `PrefabAsset`·`PrefabNodeInfo`·NodeGuid, 본문 인코딩(편집 World → graph v2 + 레코드 대비 차이), 두 revision, A1 저장·로드, Summary 본문 타입 지문, 해석된 원형 캐시(사슬 없는 경우), 제외 필드 건너뛰기 decode(`skippedFlags`), Summary `ClassDefaults` 본문 타입 행·Hard 의존을 내주는 저장 hook, AssetManager Discard 통지 | M | A1, A5, PF1, 부착 계층 영속 |
| PF3 | (실행 중 생성) `Prefab` 모듈(World 위)·`SpawnPrefab`·`SpawnPrefabBatch`, World staged 묶음 등록 키, Level 소속 유도·`levelPosition`의 `spawnedActors` 처리, 부분 그래프 복원·그래프 사이 참조, `spawnedActors`, 구움 Transform, 다중 root Commit, hook 묶음, `PrefabUsage` 장부·lease, Level 클러스터 가입, 벤치 | M | PF2, `worldOnlyCreationKey` |
| PF4 | (override) `PrefabOverrideSet` 형식(노드 주소 표·참조 번호·타입 버전), `ApplyPathWire`, 값·Map/Set key·노드 추가·삭제, 명시 집합 ∪ 차이 수집, key 추출, Revert(`PlanRevert` 기준 인자 일반화) | L | PF3, CU4 소유형 경로 |
| PF5 | (레벨 배치) `PrefabPlacement`, 추적 인스턴스 장부·앵커 행렬, 파생 ID, 로드 시 spawn, 편집 World 모드 | M | PF4, 레벨 패키지 |
| PF6 | (중첩·variant) 기준 사슬·중첩 해석, 주소 경로, 순환·점진 한도 검사, 우선순위, 해석 입력 복사(Game)·해석(worker) | L | PF4 |
| PF7 | (전파) resolvedRevision 비교, 제자리 갱신·재생성, override 마이그레이션, 충돌 분류·고아 보존, Apply to source·Make variant·Unpack API | L | PF6, CU5(제자리 갱신), Actor 영속 Entity Guid(Unpack) |
| PF8 | (쿠킹) 평탄화(부분 그래프 분할), EditorOnly 제외, 고아 거부, 하드 의존 예산, cooked resolvedRevision 검증, cookKey 전이 | M | PF6, A4 |
| PF9 | (후속, D1·D9에 따라) 별도 Level 로드 묶음(UE LevelStreaming), 쿡 때 레벨에 굽기(UE Partitioned), 렌더 instancing으로 굽기(UE Packed) | L | 레벨 스트리밍, 렌더 instancing |

PF1은 A5와 RCM 개정 뒤 언제든 할 수 있고, PF2 이후는 World 단계와 레벨 패키지 단계에 묶인다. 단계마다 Debug·format·policy·바뀐 부분 시험으로 검증한다.

### 5.3 시험 목록

| 단계 | 시험 |
|---|---|
| PF1 | `DefaultSubobject_TableGeneratedWithStableGuid`, `DefaultSubobject_CreatedByInstancingNotConstructor`, `DefaultSubobject_RecordIsDeterministic`(두 번 생성 대조), `DefaultSubobject_TableReferencesResolve`(rootComponent·부착 부모·소유 필드), `DefaultSubobject_TableReferencesEqualBaseline`(차이 쓰기에 안 나옴), `DefaultSubobject_RenameKeepsGuidAndOverride`, `DefaultSubobject_DerivedClassOverridesInheritedDefaults`, `DefaultSubobject_GraphNodeBindsExistingSubobject`, `DefaultSubobject_NoSharedMutableComponent`, `DefaultSubobject_ClassDefaultChangePropagatesToNonOverridden`, `CreationOrigin_RuntimeComponentsNotSaved`, `DefaultSubobject_SpawnActorSingleTransaction`(실패 시 subobject 남지 않음), `DefaultSubobject_ComponentOuterIsActor`, `DefaultSubobject_IdKindChangeRejected`(field ↔ subobject), `DefaultSubobject_EngineActorRootSavedAsSubobject`(ERD37 Actor 레벨 저장·로드), `GraphV3_DeclaredSubobjectNodeRoundTrip` |
| PF2 | `PrefabAsset_RoundTripNodesAndBody`, `PrefabAsset_BodyIsDiffAgainstClassRecord`, `PrefabAsset_DuplicateTransientKeptInBodyExcludedAtSpawn`, `PrefabAsset_NodeInfoMustMatchNodeTable`, `PrefabAsset_RejectsUnsaveableAndNativeDerivedNodes`, `PrefabAsset_RejectsExternalWorldReference`, `PrefabAsset_SummaryCarriesBodyTypeFingerprints`, `PrefabAsset_NodeLimitIsCapacityExceeded`, `PrefabRevision_ResolvedChangesWithBaseNestedAndTypeFingerprint` |
| PF3 | `SpawnPrefab_GraphMatchesArchetype`, `SpawnPrefab_InternalReferencesPointToNewObjects`, `SpawnPrefab_CrossGraphStrongAndWeakReferences`, `SpawnPrefab_TwiceHasNoNameConflict`, `SpawnPrefab_TwoInstancesShareNoObjects`, `SpawnPrefab_ExpandedActorsInTransientList`, `SpawnPrefab_BakedRootTransformChildTimesParent`, `SpawnPrefab_ShearFailsWithoutLeftovers`, `SpawnPrefab_AttachParentComposition`, `SpawnPrefab_AwakeFailureDestroysWholeBatch`, `SpawnPrefab_LeaseReleasedWhenLastActorDies`, `SpawnPrefab_CacheHoldsNoLeaseAndDropsOnDiscard`, `SpawnPrefab_UntrackedLeavesNoInstanceRecord`, `SpawnPrefab_JoinsLevelCluster`, `SpawnPrefabBatch_OneCommitPerBatch`, 벤치 3.9절 |
| PF4 | `PrefabOverride_FieldValueSurvivesArchetypeChange`, `PrefabOverride_SameValueExplicitOverrideIsKept`, `PrefabOverride_TransformTranslationOnlyFollowsRotation`, `PrefabOverride_AtomicStructAppliedByPathWire`, `PrefabOverride_ArrayOptionalInstancedStructWholeUnit`, `PrefabOverride_MapKeyAddChangeRemoveExtractedFromPublish`, `PrefabOverride_ArrayHasNoIndexOverride`, `PrefabOverride_AddAndRemoveNodes`, `PrefabOverride_RemoveClassSubobjectRejected`, `PrefabOverride_ReferenceToRemovedNodeBecomesNull`, `PrefabOverride_RevertRestoresArchetypeValue`, `PrefabOverride_OwnedContainerNotOverridable` |
| PF5 | `PrefabPlacement_LevelSavesPlacementNotExpandedObjects`, `PrefabPlacement_LevelSaveSucceedsWithExpandedActors`, `PrefabPlacement_DerivedIdsStableAcrossSaveLoad`, `PrefabPlacement_DuplicateReissuesInstanceAndDerivedIds`, `PrefabPlacement_PieKeepsIds`, `PrefabAnchor_RootRelativeStaysByteEqual`(배치 이동 뒤 override 0), `PrefabAnchor_NonUniformScaleWithRotatedRoots`, `EditWorld_NoGameHooksRun` |
| PF6 | `PrefabNesting_SamePrefabTwiceHasDistinctAddresses`, `PrefabNesting_OuterOverrideBeatsInner`, `PrefabVariant_ChainAppliesBaseFirst`, `PrefabVariant_AddedNodesOnlyInOverrides`, `PrefabChain_CycleRejectedOnSaveLoadCook`(variant·중첩·override 추가 중첩 섞인 순환 포함), `PrefabChain_LimitsStopIncrementally`, `PrefabResolve_WorkerUsesCopiedBytesOnly`, `DeriveGuid_DomainAndLengthPrefixSeparateInputs` |
| PF7 | `PrefabRebase_NonOverriddenFollowsNewBase`, `PrefabRebase_OverriddenKept`, `PrefabRebase_InPlaceKeepsHandles`, `PrefabRebase_StructuralChangeRecreatesWithSameIds`, `PrefabRebase_OrphanOverrideKeptAndReported`, `PrefabRebase_PartialRecordMigration`, `PrefabRebase_MigratedMapFieldOrphansKeyEntries`, `PrefabRebase_AddedNodeNameConflictRenamed`, `PrefabApply_MovesOverridesToSource`, `PrefabMakeVariant_MovesAddedNodeGuids`, `PrefabUnpack_FreezesDerivedIds` |
| PF8 | `PrefabCook_FlattenedEqualsCookedConfigResolved`(Cooked 구성 해석과 같은 입력 → 같은 byte), `PrefabCook_SplitsOver4096IntoPartialGraphs`, `PrefabCook_EditorOnlyStripped`, `PrefabCook_OrphanOverrideFails`, `PrefabCook_HardDependencyBudget`, `PrefabCook_RevisionMismatchIsUnsupportedVersion`, `PrefabCook_BaseChangeRecooksDependents` |
| 통합 | 전파·GC·Undo·save/load·cell unload를 함께 도는 soak(EIP 20.3절 M5 gate), 결정성(같은 입력 두 번 → 같은 파생 ID·같은 그래프 byte) |
