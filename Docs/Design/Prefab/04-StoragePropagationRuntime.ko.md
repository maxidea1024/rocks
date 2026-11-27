# prefab 설계 — 저장·전파·중첩·실행 비용·Transform·오류·스레드

[목차](README.ko.md)

## 3. Rocks 설계(이어서)

### 3.6 저장 형식

**prefab 패키지.** A1 패키지(AS 5절) 하나에 대표 에셋 `PrefabAsset` 하나다. 원형 객체들은 패키지 export가 아니라 `PrefabAsset`의 필드 값이다(3.1절 — WorldOnly 객체는 에셋 Registry에 둘 수 없다).

| `PrefabAsset` 필드 | 내용 | 정본 |
|---|---|---|
| `base` | `SoftObjectRef<PrefabAsset>` — variant면 기준 prefab, 아니면 null | — |
| `body` | 원형 본문: graph v2 바이트(NodeTable 있음, `BaseOuter` — OF 5.1절). 노드 Record는 **클래스 레코드 대비 차이**(subobject는 subobject 기본값 레코드 대비 — A5 차이 쓰기). variant는 비어 있다 | 노드의 타입·Outer·이름 |
| `nodeInfo` | `Array<PrefabNodeInfo>` — `body` NodeTable과 같은 순서·같은 길이의 부가 정보(NodeGuid·노드 종류·SubobjectGuid·중첩 대상) | NodeGuid·노드 종류 |
| `crossReferences` | 그래프 사이 weak 참조 표(3.3절 6항) — 편집기 본문에서는 중첩 노드 안을 가리키는 참조만 생긴다 | — |
| `overrides` | `PrefabOverrideSet` — variant의 기준 대비 차이(**variant의 추가 노드는 여기에만 있다**), 중첩 인스턴스 노드에 둔 override | variant 추가 노드 |
| `contentRevision` | 자기 내용 지문 | — |

- **정본은 하나씩이다**: 노드 타입·Outer·이름은 graph v2 NodeTable, NodeGuid와 노드 종류는 `nodeInfo`, variant 추가 노드는 `overrides`의 노드 추가 항목이다. 둘이 어긋나면(길이·순서) `CorruptPackage`다.
- `body`의 노드 수는 graph v2 `maxObjects`(4096 — OF 5.6절)로 묶이며 이 값을 `MaxPrefabNodes`로 이름 붙여 쓴다. `body`는 64 MiB(`maxValueBytes`) 이하다. 큰 본문을 Bulk entry로 옮길지는 PF2에서 크기를 재고 정한다.
- **두 가지 revision**(교차검증 I8):
  - `contentRevision` = SHA-256(이 패키지의 `base`·`body`·`nodeInfo`·`crossReferences`·`overrides`의 인코딩). 저장 때 계산한다. 편집기 dirty 판정과 쿡 입력 지문에 쓴다.
  - `resolvedRevision` = SHA-256(도메인 문자열, 자기 `contentRevision`, 기준의 `resolvedRevision`, 중첩 노드마다 대상의 `resolvedRevision`(노드 순서), 해석 결과가 쓰는 타입들의 (TypeId, 스키마 버전, 클래스 레코드·subobject 기본값 레코드 지문) TypeId 정렬 목록). 저장하지 않고 해석 때 계산한다(cooked는 쿠커가 계산해 저장한다).
  - 쓰임: 원형 캐시 키 = (AssetId, `resolvedRevision`). 레벨 배치 기록의 `baseRevision` = 저장 당시 `resolvedRevision`이라서 기준 사슬·중첩·코드 기본값이 바뀌어도 전파가 일어난다(3.7절). cooked 검증 = cooked `PrefabAsset`에 저장한 `resolvedRevision`과 cooked 레벨 배치 기록의 `baseRevision` 대조(다르면 `UnsupportedVersion` — 다시 쿠킹). 쿠커 재계산 = prefab의 cookKey 입력에 기준·중첩 prefab의 cookKey와 타입 지문 목록을 넣어 기준이 바뀌면 그것을 쓰는 prefab과 배치 레벨이 다시 쿠킹된다.
- **의존**: 원형 본문의 에셋 참조(World 객체의 Soft 참조)는 이 패키지의 Dependencies(Hard)로 기록한다 — "하드 의미는 그 World 데이터를 담는 레벨·prefab 패키지의 Dependencies(Hard)로 표현한다"(AS 6.3절). 기준 prefab과 중첩 prefab도 Hard다. override하지 않은 값의 클래스 레코드 참조도 포함한다(RCM 68행). prefab별 하드 의존 크기 예산은 쿡이 검사한다(SL 7.2절). Hard 의존 덕분에 prefab A가 상주하는 동안 그 기준·중첩 prefab도 상주한다.
- **레벨 배치 인스턴스.** 레벨에 놓인 인스턴스는 펼친 객체가 아니라 **배치 기록** `PrefabPlacement{instanceGuid, prefab(SoftObjectRef), baseRevision, transform, overrides}`로 저장한다(UE Level Instance가 World 에셋 + 배치 Actor인 구조 — 2.5절, 4절 D3). 배치 기록은 Level의 영속 struct 배열 필드다. 펼친 Actor·Component는 Transient 소유 목록에 있으므로(3.3절 9항) 레벨 저장에 들어가지 않는다. `baseRevision`이 다르면 로드 때 전파(3.7절)를 거친다.
- **펼친 노드를 가리키는 레벨 참조**(4절 D22): 레벨에 직접 저장하는 Record(직접 배치 Actor·LevelScript와 그 component)의 weak 참조가 같은 Level의 배치 인스턴스가 펼친 노드를 가리키면, 펼친 객체는 Transient 목록에 있어(3.3절 9항) 그 handle을 저장할 수 없다. 레벨 저장은 Record의 그 자리를 null로 쓰고 Level의 영속 struct 배열 `placementNodeReferences`에 `{참조하는 객체(레벨 graph v2 노드 번호), 소유형 PropertyPath, instanceGuid, PrefabNodeAddress}`를 둔다. 로드는 배치를 펼친 뒤(PF5) 주소를 D6 규칙(인스턴스 Guid + 주소 → 추적 인스턴스의 노드 대응표, `World::FindPrefabNode`)으로 해석해 Commit 전 staged 경로 쓰기로 채운다(D17의 사후 쓰기와 같은 방식). 대상 노드가 없으면(배치 override로 삭제, 원형에서 사라짐) null로 두고 `PrefabPlacementReferenceOrphaned`를 낸다. 강한 참조는 지금처럼 graph v2가 집합 밖 참조로 거부한다. 영속 Entity Guid가 생기면 그 표현(D12의 영속 Entity 참조)으로 바꾼다.
- **실행 중 spawn한 인스턴스**는 저장하지 않는다. 세이브게임은 원형 ID·인스턴스 Guid·spawn/despawn 기록과 SaveGame 필드를 저장한다(EIP 951행 — 세이브게임 계층).
- **쿠킹**(4절 D8): 쿠커는 variant 사슬과 중첩을 펼친 **평탄화 원형**을 cooked `PrefabAsset`에 저장한다. 펼친 노드가 `MaxPrefabNodes`(graph v2 `maxObjects`)를 넘을 수 있으므로 평탄화 본문은 **부분 그래프 여러 개**(각각 4096 이하 — 원래의 원형 경계를 따라 나누고, 한 원형이 4096을 넘을 수는 없다) + 노드 주소 표 + 그래프 사이 참조 표다. 그래프 사이 참조는 3.3절 6항 규칙(강한 = 같은 트랜잭션 import, weak = 참조 표)을 그대로 쓴다. `base`·`overrides`는 비운다(UE cooked 빠른 경로가 차이 블록을 미리 만드는 것과 같은 목적 — 2.4절). 편집기 해석 결과와 cooked 평탄화 결과는 같은 입력에서 같은 byte여야 한다(PF8 시험). EditorOnly 필드·노드는 뺀다. 레벨 배치 인스턴스의 cooked 형태는 4절 D9.
- **A1과의 관계**: prefab은 새 섹션을 만들지 않는다. 일반 에셋 패키지이고, 원형 본문은 대표 에셋의 값이다. 패키지 Summary의 타입 표에는 `PrefabAsset`과 함께 **원형 본문이 쓰는 World 타입들**의 (TypeId, 스키마 버전, 클래스 레코드 지문)을 넣는다 — 본문 Record가 그 타입들의 레코드 대비 차이이기 때문이다(RCM 69행 지문 규칙을 본문 타입까지 넓힌다 — 7절). 지문이 다르면 편집기는 진단 뒤 열고 cooked는 `UnsupportedVersion`이다.
- 원점 규약 버전 칸은 두지 않는다(하위호환 없음 — 규약이 바뀌면 원형을 다시 만든다).

### 3.7 원형 변경 전파와 충돌 처리

**원칙**: 인스턴스는 차이만 가지므로 **새 원형 + 같은 override = 전파 결과**다(UE override 추적의 "재생성으로 전파" — 2.7절). 이것이 EIP 20.3절의 3-way(old base / new base / local override)를 그대로 구현한다: override가 없는 필드는 new base 값, 있는 필드는 local 값이다.

**언제**:

- 로드: 배치 기록의 `baseRevision` ≠ 현재 `resolvedRevision`이면 전파 후 spawn하고, 편집기는 레벨을 dirty로 표시한다.
- 편집기에서 원형 저장: 열린 World의 그 원형을 쓰는 추적 인스턴스(중첩·variant 포함 — 인스턴스 장부의 원형별 목록, 3.9절)를 모두 갱신한다.
- `RebasePrefabInstance(PrefabInstanceId, resolvedRevision)`(EIP 20.3절)로 명시 호출.
- 실행 중(Tryout·게임)에는 원형이 바뀌지 않는다. Tryout 중 원본 수정은 명시 live patch 범위에서만이다(EIP 20.3절).

**어떻게**(4절 D7):

- **제자리 갱신**: 노드 집합이 같으면(추가·삭제 노드 없음, 노드 타입 같음) 노드마다 override하지 않았고 현재 값이 **옛 원형 값과 인코딩이 같은** 단위에 새 원형 값을 쓴다(UE `PropagatePropertyChange`의 "이전 값과 같을 때만" — 2.8절. override 집합이 있으므로 비교는 확인용이다). 쓰기는 World API·setter hook 경로를 지난다(CU5). 객체 정체성·handle·hook 상태가 유지된다.
- **재생성**: 노드가 추가·삭제되었거나 노드 타입이 바뀌었으면 인스턴스를 같은 인스턴스 Guid로 다시 만든다 — 옛 객체 RequestDestroy(EndPlay 계열 hook) → 새 spawn. Actor는 Outer·이름이 없고(3.3절 4항) Component는 새 Actor 아래라서 옛 객체가 이름을 차지하고 있어도 충돌하지 않는다. 파생 ID가 같으므로 외부 바인딩·세이브 참조는 유지된다. BeginPlay 중복 방지는 EIP 1738행의 world session event 기록을 쓴다.

**override 마이그레이션**(4절 D19): 노드 타입의 스키마 버전이 override 집합에 기록된 버전과 다르면, 그 노드의 값 override 항목을 **부분 Record**로 묶는다(누락 = 기준이 형식 규칙이므로 일부 필드만 담은 Record가 유효하다 — RCM 66행). 그 Record에 타입 migrate(Record 단위, 값의 존재 여부를 받는다 — RF 2.2절, RCM 67행)를 적용한 뒤 결과 Record에 있는 단위 경로를 다시 뽑아 새 항목으로 바꾼다. Map·Set key 항목은 부분 Record로 표현할 수 없으므로, 그 필드를 migrate가 바꾸면(전후 wire가 다르면) key 항목을 고아로 보존하고 진단한다. FieldId는 이름 변경에도 유지되므로 이름만 바꾼 필드는 마이그레이션 없이 그대로 맞는다.

**충돌 분류**(전파는 충돌이 있어도 멈추지 않는다 — 충돌 항목만 보존하고 진단한다):

| 충돌 | 처리 | 진단 |
|---|---|---|
| override 대상 노드가 새 원형에 없다 | override 항목을 **고아로 보존**(적용하지 않음), 편집기에 표시, 사용자가 지우거나 다른 노드로 옮긴다 | `PrefabOverrideOrphaned` |
| override 경로의 필드가 마이그레이션 뒤에도 없다 | 고아 | `PrefabOverrideOrphaned` |
| override 값의 wire가 필드 종류와 맞지 않는다 | 고아 | `PrefabOverrideTypeMismatch` |
| migrate가 key 단위 override가 있는 Map·Set 필드를 바꿨다 | key 항목 고아 | `PrefabOverrideNeedsReview` |
| 삭제 항목의 노드가 이미 원형에서 없어졌다 | 항목을 지운다(목적 달성) | 없음(정보) |
| 추가 노드의 부모가 원형에서 없어졌다 | 최상위 Actor면 인스턴스 루트로, Component면 고아로 보존 | `PrefabAddedNodeParentMissing` |
| 추가 노드 이름이 새 원형 노드 이름과 겹친다 | 추가 노드 이름에 결정적 번호 접미를 붙이고 진단(정체성은 NodeGuid라 영향 없음) | `PrefabNodeNameConflict` |
| Required 필드가 전파 결과 null | spawn 실패(편집기는 진단과 함께 인스턴스를 비활성 표시) | `RequiredMissing`(OF 4절) |
| 새 원형이 순환·깊이 한도를 만든다 | 원형 저장 자체를 거부 | `PrefabCycle`·`PrefabDepthExceeded` |

- cooked 빌드에는 충돌이 없어야 한다 — 쿡은 고아 override가 남은 레벨·prefab을 오류로 거부한다.
- **Apply to source**(인스턴스 override를 원형에 올리기): 원형 편집 transaction으로 처리하고 영향 인스턴스 목록을 만든다(EIP 20.3절). 올린 항목은 인스턴스 집합에서 지운다. `EditScope = "Instance"` 필드의 override는 올리지 않고 인스턴스 집합에 남긴다(UE `SkipInstanceOnlyProperties` — [UI 툴킷](../UiToolkit/README.ko.md) 7.3절 (a)). 같은 인스턴스를 연쇄로 다시 적용하지 않는다.
- **클래스 기본값 변경**(코드 수정·Sparks reload): 원형 본문이 클래스 레코드 대비 차이이므로 원형이 override하지 않은 값은 새 클래스 기본값을 따른다. 타입 지문이 `resolvedRevision`에 들어 있으므로 편집기 배치 인스턴스는 다음 로드 때 전파를 거치고, 살아 있는 인스턴스는 RCM 71행 reload 규칙을 노드마다 적용하되 "기준"을 해석된 원형 값으로 본다.

### 3.8 중첩과 variant

- **variant**: `base`가 다른 prefab인 prefab이다. 원형 = 기준의 해석된 원형 + 이 prefab의 `overrides`(추가 노드 포함). Unity variant·UE BP 상속(ICH)과 같은 축이다.
- **중첩**: 노드 종류가 "중첩 인스턴스"인 노드는 `{prefab, transform, overrides}`를 갖는다. 해석은 그 prefab의 해석된 원형을 이 노드 아래에 펼치고 주소에 이 노드의 NodeGuid를 앞에 붙인다. override 노드 추가 항목으로 중첩 인스턴스를 더할 수도 있다.
- **우선순위**(바깥이 이긴다): 클래스 레코드 → subobject 기본값 레코드 → 가장 안쪽 원형 본문 → 그 원형의 variant 사슬(기준부터) → 바깥 원형이 그 중첩 노드에 둔 override → … → 레벨 배치 override → spawn 인자.
- **순환 금지**: 해석은 방문 중인 AssetId 집합을 들고 깊이 우선으로 내려가며 다시 만나면 `PrefabCycle`로 실패한다. 기준 사슬, 본문의 중첩 노드, **override가 추가한 중첩 인스턴스 노드**(variant `overrides`와 중첩 노드의 override 안)를 모두 같은 집합으로 본다(A가 B를 품고 B가 A의 variant인 경우도 순환이다). 편집기 저장·로드·쿡이 모두 검사한다(UE는 편집기 로드의 `CheckForLoop`과 쿡 검증 `ValidateCircularReference`로 나눠 검사한다 — 2.5절. EIP 20.3절의 "cooker가 거부"보다 이르게 막는다).
- **한도는 해석 도중 점진적으로 검사한다**: 깊이는 내려갈 때마다, 누적 노드 수·override 수는 노드를 펼칠 때마다 비교해 넘는 순간 멈춘다(큰 입력을 다 펼친 뒤 거부하지 않는다 — 작업량도 한도 안).
- **한도**(이름 붙은 상수 — `Prefab/PrefabLimits.h`, 값은 PF0 확정 때 다시 본다):

| 상수 | 제안 값 | 근거 |
|---|---|---|
| `MaxPrefabBaseDepth` | 16 | variant 사슬 길이. EIP 20.3절 "초기 중첩 depth 상한 16" |
| `MaxPrefabNestingDepth` | 16 | 중첩 깊이 = 주소 경로 길이 상한 |
| `MaxPrefabNodes` | 4096 | 원형 하나의 `body` 노드 수 = graph v2 `maxObjects`(OF 5.6절) |
| `MaxPrefabInstanceNodes` | 65536 | 펼친 인스턴스 하나의 전체 노드 수(중첩 포함). spawn 한 번의 staged 객체 상한 |
| `MaxPrefabOverrides` | 65536 | override 집합 하나의 항목 수 |

  넘으면 `CapacityExceeded`다. `MaxPrefabInstanceNodes`가 graph v2 단일 그래프 한도보다 크므로 spawn은 부분 그래프마다 graph v2 복원을 따로 하고 한 ConstructionContext에 모은다(3.3절 3항).

### 3.9 실행 중 생성 비용·인스턴스 장부·GC

**spawn 경로에서 하지 않는 일**: 원형 사슬 해석, 원형과의 비교, 문자열 경로 해석, 원형 객체 복제. 모두 해석된 원형을 만들 때 한 번 한다.

- **해석된 원형 캐시**(교차검증 I7): 키는 (AssetId, `resolvedRevision`)이고 **lease를 쥐지 않는다.** lease를 쥐면 캐시가 자기 에셋을 영원히 붙잡는 순환이 된다. 캐시 항목은 본문 타입의 TypeLease만 쥔다(타입 등록이 원형보다 먼저 사라지지 않게 — RCM 55행 레코드와 같은 규칙). 항목은 AssetManager의 패키지 Discard 통지(그 AssetId의 패키지가 상주에서 빠질 때)로 버린다. 상주는 인스턴스 장부의 lease와 Hard 의존이 정한다. cooked는 쿠커가 이미 평탄화했으므로(3.6절) 로드가 곧 해석이다.
- **인스턴스 장부**(World 쪽, 4절 D21):

| 장부 | 언제 | 내용 | 1M 규모 비용 |
|---|---|---|---|
| 원형별 항목 `PrefabUsage` | 모든 spawn | `{AssetLease, 살아 있는 Actor 수, 추적 인스턴스 목록 머리}` — 원형 하나에 하나 | 원형 수에 비례(인스턴스 수와 무관) |
| Actor별 칸 | 모든 spawn | `ActorRecord`에 u32 `PrefabUsage` 번호(+ 추적이면 인스턴스 번호 u32) | Actor당 4–8 B |
| 추적 인스턴스 항목 `PrefabInstanceRecord` | `tracked`(편집기·레벨 배치·요청 시) | `{인스턴스 Guid, resolvedRevision, 앵커 행렬(3.10절), override 집합 사본, 노드 대응표(지연 생성)}` | 추적 인스턴스 수에 비례 |

  - lease 반납: `PrefabUsage`의 살아 있는 Actor 수가 0이 되면(마지막 Actor의 레코드 제거 — ER 7.2절) lease를 반납하고 항목을 지운다. Component는 소유 Actor와 함께 죽으므로 따로 세지 않는다.
  - 노드 대응표(`PrefabInstanceMap`)는 spawn 동안 임시로 만들고 끝나면 버린다. 추적 인스턴스는 `FindPrefabNode`를 처음 부를 때 다시 만든다(지연 생성 — 최상위 Actor부터 노드 순서로 소유 목록을 따라가며 맞춘다). 추적하지 않는 실행 중 spawn은 노드 대응표·인스턴스 항목을 전혀 남기지 않는다.
- **spawn 비용**: O(노드 수 + 실효 Record 크기 + 인스턴스 override 수). 실행 중 spawn은 보통 인스턴스 override가 없으므로 노드 수와 Record 크기에 비례한다. 할당은 staged 객체와 그 값뿐이다(장부는 `PrefabUsage` 계수 증가와 Actor 칸 하나).
- **대량 spawn**: `World::SpawnPrefabBatch(prefab, Span<const SpawnPrefabParams>)`로 여러 인스턴스를 한 ConstructionContext·한 Commit으로 만든다. Commit·클러스터 가입·관찰자 보고가 묶음마다 한 번이다. 묶음 크기는 `MaxPrefabInstanceNodes`로 나눈다.
- **GC**:
  - 원형은 Registry 객체가 아니므로 GC 표식·sweep 비용이 0이다. 원형 수가 많아도 GC와 무관하다.
  - 펼친 객체는 일반 World 객체다. Rocks World 클러스터는 **Level 하나**이고 World API 생성의 Commit에서 가입한다(GCS 13.3절, 개정 2). prefab spawn도 같은 경로로 대상 Level 클러스터에 가입한다. **UE와 다르다**: UE Level 클러스터는 로드 때 한 번 만들어지고 실행 중 spawn한 Actor는 들어가지 않는다(2.9절). Rocks는 간선 쓰기마다 장부를 고치므로 spawn한 객체도 가입한다. prefab 단위 클러스터는 두지 않는다(UE에도 없다).
  - 클러스터 소속 조건은 `completeFieldLayout` 타입이다(GCS 13.3절). native 파생 Actor·Component는 원형 본문에 저장할 수 없으므로(ER 4.6절 "native 파생이 든 그래프는 저장할 수 없다") prefab 노드는 모두 리플렉션 타입이고 클러스터 조건을 만족한다.
  - spawn은 열린 ConstructionContext 동안 GC를 막는다(OF 6절). 큰 묶음은 `MaxPrefabInstanceNodes` 단위로 나눠 GC 정지 구간을 묶는다.
- **비용 목표**(PF3 벤치에서 확정): 노드 16개 원형 1만 회 spawn, 노드 4096개 원형 1회 spawn, 1M 객체 누적 spawn 후 증분 GC 한 cycle의 표식 시간이 같은 수의 직접 `SpawnActor`·`AddComponent`와 같은 차수일 것. 추적하지 않는 spawn의 장부 비용은 Actor당 고정 byte(위 표)로 측정한다.

### 3.10 Transform 규약

좌표·단위·행렬은 UE 5.8과 같다: 왼손 +X 전방·+Y 오른쪽·+Z 위, 위치 cm, 편집기 회전 입력은 도 단위 Rotator(`ToQuaternion`으로 변환), `Transform`은 double 회전(quaternion)·이동·scale(R·T·S), 합성은 `Transform::TryMultiply(child, parent)` = UE `Child * Parent`(Core/Math/Transform.h 12-14행), SceneComponent 월드 행렬 = 상대 TRS 행렬 × 부모 월드 행렬(행 벡터 — ER 7.3절).

- **원형 원점**: 원형은 자기 원점(prefab 공간)을 갖는다. 최상위 Actor의 root component `relativeTransform`은 prefab 공간 기준이다. Actor에 붙은 하위 SceneComponent는 지금처럼 부모 기준이다.
- **추적 인스턴스 — 앵커**(4절 D10): 최상위 root의 `relativeTransform`에 월드 값을 구워 넣지 않는다. 구우면 byte 비교 override 판정(3.5절)이 늘 "다름"이 되고, 편집 뒤 역변환으로 되돌려도 비트가 달라 가짜 override가 생기며 제자리 전파(3.7절)도 깨진다. 대신 인스턴스 장부의 **앵커 행렬**(`Mat4d` — 인스턴스 Transform의 TRS 행렬, 부착 부모가 있으면 × 부모 월드 행렬)을 부착 부모 없는 최상위 root의 "부모 행렬"로 쓴다. 최상위 root의 월드 행렬 = 상대 TRS 행렬 × 앵커 행렬이다. root의 `relativeTransform`은 prefab 공간 값 그대로이므로 원형과 byte로 같고, 사용자가 그것을 바꾸면 정확히 그만큼 override가 된다. 인스턴스를 옮기면 앵커 행렬을 바꾸고 최상위 root 이하를 dirty로 표시한다.
  - 앵커는 행렬 합성이라 비균일 scale 인스턴스 아래 회전한 최상위 root도 shear 그대로 정확하다 — 추적 인스턴스에는 shear 제약이 없다.
  - 중첩: 중첩 인스턴스 노드의 `transform`은 바깥 prefab 공간 기준이다. 안쪽 최상위 root의 부모 행렬 = 중첩 노드 TRS 행렬 × 바깥 앵커 행렬이다(중첩 앵커도 행렬이다).
  - 앵커는 NATIVE 상태다(저장은 배치 기록 `transform`). SceneComponent의 부모 행렬 출처가 "부모 SceneComponent 또는 앵커"로 늘어나는 것은 World 단계 변경이다(7절).
- **추적하지 않는 실행 중 spawn — 구움**: 게임 코드는 부착 부모 없는 Actor의 상대 Transform을 월드 Transform으로 다룬다(ER 7.3절). 그래서 실행 중 spawn은 최상위 root `relativeTransform`에 `TryMultiply(원형 상대 Transform, 인스턴스 Transform)`을 써 넣는다. 저장하지 않는 인스턴스라 override 판정 문제가 없다. 합성이 shear로 실패하면(Transform.h 47-49행) spawn은 `Unsupported`·`PrefabTransformShear`로 실패하고 아무것도 남기지 않는다(EIP 147행 CF04 "불허 경로는 변경 없는 실패"). 비용: 원형 안에 회전된 최상위 root가 있으면 그 prefab을 비균일 scale로 실행 중 spawn할 수 없다. 이런 사용이 필요하면 `tracked = true`로 앵커를 쓴다.
- **부착 부모가 있는 spawn**(`attachParent`): 최상위 root를 그 SceneComponent에 붙인다. 추적이면 앵커 행렬 = 인스턴스 Transform TRS 행렬 × 부모 월드 행렬이고 부모가 움직이면 앵커를 다시 계산한다(부모의 dirty 전파가 앵커를 거친다). 추적하지 않으면 root `relativeTransform` = `TryMultiply(원형 상대, 인스턴스 Transform)`이 부모 기준 상대 값이 되며 shear 규칙은 같다. 다른 Actor의 Component를 부모로 저장하는 일(배치 기록의 부착)은 원형 밖 World 참조라 1차에 저장하지 않는다(4절 D12).
- **override**: `Transform`은 비Atomic이라 멤버 단위 override다(RCM 64행, RX 3.1절). 인스턴스에서 이동만 바꾸면 회전·scale은 원형 변경을 따른다(RX 301행이 말한 UE 동작).
- **저장**: 배치 기록의 `transform`은 인스턴스 원점 Transform(부착 부모 없으면 월드)이다. 최상위 root의 상대 Transform 편집은 prefab 공간 값의 override로 그대로 저장한다(역변환 없음).
- **큰 좌표**: 인스턴스 Transform과 앵커 행렬은 double이다. 렌더용 float 좁히기(Transform3f)는 렌더 계약을 따른다.

### 3.11 오류·진단

`ErrorCode`는 기존 것을 쓰고 원인은 진단 코드로 구분한다(AS 4.6절 진단 형식, OF 진단과 같은 꼴).

| 진단 | ErrorCode | 때 |
|---|---|---|
| `PrefabCycle` | InvalidArgument(편집)·CorruptPackage(로드) | 기준·중첩·추가 중첩 순환 |
| `PrefabDepthExceeded`·`PrefabNodeLimit`·`PrefabOverrideLimit` | CapacityExceeded | 3.8절 한도 |
| `PrefabMissingBase`·`PrefabMissingNested` | MissingDependency | 기준·중첩 prefab 로드 실패 |
| `PrefabDuplicateNodeGuid` | CorruptPackage | 노드 정보·추가 노드 GUID 중복 |
| `PrefabNodeTableMismatch` | CorruptPackage | `nodeInfo`와 NodeTable의 길이·순서 불일치 |
| `PrefabNodeNameConflict` | InvalidArgument(추가)·진단(전파) | 같은 Actor 안 노드 이름 중복 |
| `PrefabUnsaveableNode` | Unsupported | Transient·Deprecated 클래스, native 파생, Runtime 출처 노드, LevelScript 파생 클래스(1.3절) |
| `PrefabExternalReference` | InvalidArgument | 원형 밖 World 객체 참조 저장(3.5절) |
| `PrefabOverrideOrphaned`·`PrefabOverrideTypeMismatch`·`PrefabOverrideNeedsReview`·`PrefabAddedNodeParentMissing` | 편집기: 진단만, cooked: InvalidArgument | 3.7절 충돌 |
| `PrefabOverridePathInvalid` | InvalidArgument | 경로 해석 실패, ReadOnly 위반, 소유 컨테이너 대상, 편집 위치 위반(편집기 쓰기 경로 — [UI 툴킷](../UiToolkit/README.ko.md) 7.3절 (a)) |
| `PrefabEditScopeViolation` | 편집기: 진단만(값은 적용), cooked: InvalidArgument | 저장물의 편집 위치 위반: 원형 저장물(`PrefabAsset`의 본문·variant `overrides`·중첩 노드 override)에 `EditScope = "Instance"` 필드가 클래스 레코드와 다른 값, 배치 저장물(`PrefabPlacement.overrides`·레벨 직접 배치 Record)에 `"Archetype"` 필드가 원형과 다른 값(UI 툴킷 7.3절 (a)) |
| `PrefabRemoveNotAllowed` | InvalidArgument | 클래스 subobject·유일 루트 삭제 |
| `PrefabTransformShear` | Unsupported | 3.10절 구움 경로 |
| `PrefabRevisionMismatch` | 편집기: 진단 후 전파, cooked: UnsupportedVersion | 배치 기록의 `baseRevision` 불일치 |
| `PrefabNodeLookupUntracked` | Unsupported | 추적하지 않는 인스턴스의 노드 조회 |
| `PrefabPlacementReferenceOrphaned` | 편집기: 진단만(null로 둠), cooked: InvalidArgument | 레벨의 배치 노드 참조(D22)가 가리키는 노드가 펼친 인스턴스에 없다 |

- 진단은 노드 주소·필드 경로를 함께 담는다(경로 표기는 RX 1.6절). 로그 문구는 영어다.
- 실패 원자성: 원형 해석 실패는 캐시를 만들지 않는다(할당 실패가 아닌 실패는 (AssetId, resolvedRevision)별로 기억한다 — RCM 56행과 같은 방식). spawn 실패는 3.3절 11·12항.

### 3.12 스레드·수명

| 작업 | 스레드 | 근거 |
|---|---|---|
| prefab 패키지 로드 | A1 로더(IO·압축 해제 worker, 객체 생성 Game) | AS 11.2절 |
| 해석 입력 모으기: 대상·기준·중첩 `PrefabAsset`의 필드 byte 복사, 본문 타입 TypeLease 획득 | Game(`AssetManager::With` — 에셋 객체는 소유 스레드에서만 읽는다, AS 6.1절) | 교차검증 I12 |
| 원형 해석(사슬·중첩 펼침, override 경로 해석, `resolvedRevision` 계산) | 어느 스레드나 — **입력은 Game에서 복사한 불변 byte와 TypeLease뿐**이고 Registry를 건드리지 않는다(TypeRegistry 조회는 잠금) | OF 6절 `ObjectGraphStage::Validate`와 같은 조건 |
| graph v2 1단계(파싱·검증) | 어느 스레드나 | OF 6절 |
| staged 생성·값 적용·Commit·World 등록·hook | World 소유 스레드(Game) | OF 6절, ER 7절 |
| 전파(편집기) | Game, 편집 transaction 안 | 3.7절 |

- **수명**: `PrefabArchetype`은 불변 공유 값이고 TypeLease만 쥔다(3.9절). 에셋 lease는 `PrefabUsage`가 쥐고 원형의 마지막 Actor가 사라질 때 반납한다. `PrefabSpawnResult`는 lease를 쥐지 않는 값이다.
- **World 종료·Tryout**: World 종료 시 `PrefabUsage`의 lease를 모두 반납한다. Tryout 시작은 편집 World를 복제하며 인스턴스 Guid·배치 기록·추적 인스턴스 장부를 유지한다(Tryout 복제 — DuplicateTransient만 제외, RX 9.1절). 늦게 끝난 비동기 로드는 종료된 World에 spawn하지 않는다(RCM 45행).
- **모듈 reload**: 본문 타입의 revision이 바뀌면 `resolvedRevision`이 바뀌므로 옛 캐시 항목은 더 쓰이지 않고 Discard 통지나 다음 정리에서 버린다.

### 3.13 편집기·네트워크와의 접점(자리)

- **편집 World**(4절 D18): prefab 편집은 원형을 **편집 전용 World**에 spawn해서 World API로 고치고 저장 때 다시 원형 본문으로 인코딩한다(UE Level Instance 편집 모드·Unity prefab mode와 같은 흐름). 편집 World와 레벨 편집 World는 게임 hook(`OnAwake`·`OnEnable`·`OnStart`·`OnTick`)을 실행하지 않는다. 등록(`OnRegister` — 표시·backend 등록)과 편집기 재구성 hook(`Validate` — EIP 5.3절·1740행)만 실행한다. 그래서 hook이 만드는 Runtime 출처 component가 원형에 섞이지 않는다(3.3절 10항). 지금 `FinishSpawning`은 등록과 `OnAwake`를 함께 하므로(ER 7.3절) 편집 World 모드가 World 단계 변경으로 필요하다(7절).
- **편집기 명령**(EIP 20.3절)은 이 문서의 API로 구현한다.
  - Revert = override 집합에서 지우고 원형 값 쓰기.
  - Unpack = 펼친 객체를 레벨에 직접 배치한 객체로 바꾸고 배치 기록을 지운다. 파생 ID를 그 시점 값으로 굳혀 각 Actor에 저장해야 하므로 **Actor의 영속 Entity Guid 필드가 선행 조건**이다(지금 ER 4.6절 Actor 필드에 없다 — 7절).
  - Make variant = 인스턴스의 override 집합으로 새 prefab의 `overrides`를 만들고 인스턴스는 새 variant를 가리킨다. `EditScope = "Instance"` 필드의 override는 옮기지 않고 인스턴스 집합에 남긴다(UI 툴킷 7.3절 (a)). 인스턴스가 추가했던 노드는 **NodeGuid를 그대로 새 variant로 옮긴다**(Apply와 같은 이동 의미 — 인스턴스 쪽 항목은 지워져 같은 NodeGuid가 두 곳에 남지 않는다). 그래서 파생 ID가 바뀌지 않는다.
  - Apply to source는 3.7절.
- 네트워크 spawn: 원형 AssetId·`resolvedRevision`·인스턴스 Guid·spawn 인자·권한 override를 보내고 받는 쪽이 같은 revision을 검증한 뒤 spawn한다(EIP 20.3절). 파생 ID가 결정적이므로 하위 객체 ID를 따로 보내지 않는다.
