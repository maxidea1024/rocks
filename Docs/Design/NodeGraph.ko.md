# 노드 그래프(NodeGraph) 설계

기준일: 2026-11-22, 개정 3. 이 문서는 여러 전용 편집기(머티리얼·사운드·애니메이션·VFX·PCG 등)가 함께 쓰는 노드 그래프 체계를 정한다. 범위는 다음과 같다.

- 그래프 데이터 모델과 도메인 schema 접점
- 변경·undo·통지
- 복사·붙여넣기·텍스트 형식
- 노드 등록·버전
- 그래프 편집 화면(`GraphView`)

UE 5.8 대응은 다음과 같다.

- 데이터: EdGraph(`UEdGraph`·`UEdGraphNode`·`UEdGraphPin`·`UEdGraphSchema`)
- 화면: GraphEditor 모듈(`SGraphEditor`·`SGraphPanel`·`SNodePanel`·`SGraphNode`·`SGraphPin`)
- 모델 우선 구조 대조: RigVM·MetaSound Frontend·PCG·Dataflow

UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`, commit `f45729c0b6f0`)다. 경로 약어는 다음과 같다. UE 이름은 대응표와 근거에만 적는다.

- `RE/` = `Engine/Source/Runtime/Engine/`
- `ED/` = `Engine/Source/Editor/`
- `PL/` = `Engine/Plugins/`

상태: **설계 초안**. 결정 GD1–GD25는 모두 권장안이다. 교차 검증 1회를 반영했다(필수 9·권장 14·참고 11, 17절).

구현 단계는 NG0–NG8(14절)이다. 착수는 계획 순서(A0-B → A0-O → 리플렉션 B9 → Curve → A0-G → A1–A6 → 화면 마일스톤)를 따르고, 이 문서 때문에 화면 단계를 앞당기지 않는다.

- 화면이 없는 NG0–NG4는 [머티리얼 시스템](MaterialSystem.ko.md) MS1과 같은 시점이거나 그 직전이다.
- 화면 단계 NG5–NG7은 [UI 툴킷](UiToolkit.ko.md) UT3·UT4 뒤다.

## 요약

**모델 우선(GD1).**
- 권위 데이터는 도메인과 무관한 그래프 모델(`GraphDocument`) 하나다. Object가 아닌 일반 데이터다.
- 화면은 이 모델을 직접 읽는다. UE EdGraph 같은 편집용 사본을 두지 않는다.
- UE 시스템들은 "도메인 모델 ↔ EdGraph" 양방향 동기화 때문에 보정 코드가 쌓였다(2.7절).

**식별과 저장(GD2–GD4·GD23).**
- 노드는 `NodeId`(Guid)로 식별한다.
- 핀은 노드 종류 서술이 선언한 안정 키 `PinKey`(Name)로 식별한다.
- 간선은 `(출력 NodeId, PinKey) → (입력 NodeId, PinKey)`로 한 곳에 저장한다. 핀 객체·양방향 포인터는 두지 않는다.
- 노드 속성은 리플렉션 Record 바이트(struct Guid + 버전 + 내용)로 보관한다. 그래서 모델 모듈이 Object 층에 의존하지 않고, 모르는 노드도 그대로 보존한다.
- 저장·비교는 ID 순 정규 순서로 한다.

**변경 경로는 하나(GD5–GD7).**
- 모든 변경은 `GraphMutator` 명령으로만 한다.
- 명령은 검증 → 되돌릴 수 있는 `GraphPatch` 생성 → EditorBackend Transaction 기록 → 세분 통지(`GraphChange`) 순서로 처리된다.
- undo는 역 patch를 같은 경로로 적용한다. 그래서 화면·컴파일러가 받는 통지가 편집 때와 같다.
- RigVM의 "controller + 명령 기록" 방식이다. 다만 UE 트랜잭션과 따로 도는 별도 스택은 두지 않는다.

**revision과 hash(GD8).**
- 문서 revision은 모든 편집에서 오르고, 충돌 검사에 쓴다.
- 컴파일 판정은 의미 데이터의 semantic hash로 한다.
- 위치·크기·주석·색·접힘 같은 배치 데이터는 별도 struct에 두고 hash에서 뺀다(계획 14.6·14.8절).

**schema는 정책만(GD9).**
- 도메인은 `GraphSchema`로 타입·연결 규칙·변환 노드 삽입·메뉴·검증을 제공한다.
- 색·위젯은 화면 쪽 시각 등록(`GraphVisualRegistry`)이 맡는다.

**복사·붙여넣기(GD12).**
- 모델 조각을 정규 JSON 텍스트로 내보낸다. 쓰기는 엄격 JSON이고, 읽기는 JSON5도 받는다.
- 붙여넣기는 공통 층이 ID를 다시 발급한다.
- UE의 "모델 객체를 EdGraph 노드 밑으로 잠시 옮기는" 소유권 꼼수는 쓰지 않는다.

**화면(GD15–GD17).**
- `GraphView`는 UE GraphEditor의 기능 범위를 따른다.
- 갱신은 통지 단위의 증분 갱신이다. culling·hover에는 공용 2D 격자 색인을 쓴다.
- 특정 도메인(Kismet 같은) 의존은 공통 층에 넣지 않는다.

**범위 밖(GD20).**
- Blueprint와 범용 gameplay 비주얼 스크립팅은 만들지 않는다.
- 공통 층은 실행 VM을 갖지 않는다.
- Sparks 시각 편집은 [레벨 스크립트](LevelScript.ko.md) LD2대로 보류 선택지다.

## 1. 현재 상태

| 항목 | 현재 Rocks | 문제 |
|---|---|---|
| 코드 | 그래프 모델·편집 명령·그래프 화면 코드가 없다. EditorBackend 코드도 없다 | — |
| 머티리얼 | [머티리얼 시스템](MaterialSystem.ko.md)에 그래프 형식이 있다: 5.2절(노드·핀·간선), 5.5절(`NodeId`·semantic hash), 14절(E1–E14 에디터 요구). 계획 14.6절에 `MaterialNodeDescriptor`·`RegisterMaterialNode` 초안이 있다 | 노드·핀·간선·undo 단위·복사 형식이 머티리얼 전용이다. 다른 도메인이 같은 것을 다시 만든다 |
| UI | [UI 툴킷](UiToolkit.ko.md) 6절 표에 한 줄이 있다: "그래프 편집 `GraphView` · node·pin 시각 등록, 그래프 데이터와 schema는 각 도구 소유". UT6에 "그래프 Element"가 있다 | 화면과 데이터의 경계, 갱신 방식, 명령 범위가 정해지지 않았다 |
| 편집 계약 | 계획 22.1절: 화면 → EditorBackend typed 명령(expected revision·stable ID·thread affinity 검증). 22.2절: Transaction이 stable ID·FieldId·old/new·asset revision을 기록하고, 객체 생성·삭제·reparent는 graph patch로 저장하며, undo 기간 snapshot 메모리는 budget으로 제한한다. drag는 release 때 undo 1개다 | 22.2절의 graph patch는 객체 계층용이다. 노드 그래프 편집(노드 추가·연결·붙여넣기·접기)의 patch 형식이 없다 |
| 다른 그래프 도메인 | 오디오: `SoundEvent` 노드 그래프(AU10)·`SoundGraph`(M7, [오디오 시스템](AudioSystem.ko.md) 2절). 애니메이션: blend tree·state machine·graph compiler(계획 15절 1522행). PCG graph(계획 24.3절). VFX(계획 21절). MetaSounds형 DSP graph(계획 24.3절) | 모두 "전용 데이터 편집기"로만 적혀 있고 공통 기반이 없다 |
| 정책 | Blueprint·범용 gameplay node graph 제외: 계획 14.5·22.2·24.3절, [레벨 스크립트](LevelScript.ko.md) LD1·LD2, [게임플레이 작업 안내](../Guides/GameplayWorkflowGuide.ko.md) 512행 | 공통 그래프 층이 실행 흐름·VM으로 커지지 않게 경계를 문서로 막아야 한다 |
| Core 기반 | `Guid`·`TryNewGuid`(`Core/Source/Runtime/Core/Public/Core/Guid.h`). JSON 읽기(Strict·Json5)·쓰기(`JsonWriter`는 늘 엄격한 Canonical, `Core/Source/Runtime/Core/Public/Core/Text/Json.h:136–142`). 리플렉션 Record와 InstancedStruct wire 형식(`Core/Source/Runtime/Serialization/Public/Serialization/InstancedStructWire.h`, struct ID는 Guid). 리플렉션 B9의 `InstancedStruct`·`OrderedMap`은 구현 완료지만 ObjectRuntime에 있다 | `OrderedMap`은 삽입 순서를 저장·비교에 넣는다(`OrderedMap.h:3–14`). `InstancedStruct`는 TypeLease를 쥐고, 등록되지 않은 struct는 decode에서 거부·비움만 할 수 있다([리플렉션 확장](ReflectionExtensions.ko.md) 11절). 2D 공간 색인이 없다([공간 색인](SpatialIndex.ko.md): 해시·2D 격자는 후속) |

## 2. UE 5.8 분석

### 2.1 구성과 규모

| 층 | UE 위치 | 줄 수(wc -l) |
|---|---|---|
| 데이터: `UEdGraph`·`UEdGraphNode`·`UEdGraphPin`·`UEdGraphSchema`·`UEdGraphNode_Documentation` | `RE/Classes/EdGraph/`, `RE/Private/EdGraph/` | 9,105(이 중 `EdGraphPin.cpp` 2,524, `EdGraphSchema.h` 1,502) |
| 통지 플래그·노드 문구 cache | `RE/Public/GraphEditAction.h`, `RE/Public/EdGraph/EdGraphNodeUtils.h` | 282 |
| 편집 도우미: `FEdGraphUtilities`(복사·붙여넣기·복제·factory 등록)·`UEdGraphNode_Comment`·`SGraphEditor` facade | `ED/UnrealEd/` | 약 2,500 |
| 화면: GraphEditor 모듈 | `ED/GraphEditor/` | 40,091(155개 파일). Kismet 전용 pin·node 위젯 약 11,000을 빼면 공통 부분은 약 29,000 |
| 참고: Blueprint schema·노드 기반 | `ED/BlueprintGraph/Private/EdGraphSchema_K2.cpp`·`K2Node.cpp` | 8,256 · 2,231 |

EdGraph 클래스는 런타임 Engine 모듈에 컴파일된다. 그러나 핀 직렬화는 `#if WITH_EDITOR` 안에서만 한다(`RE/Private/EdGraph/EdGraphNode.cpp:232–262`). 그래서 쿠킹된 게임은 핀을 읽지 않는다.

### 2.2 데이터 모델

**그래프.**
- `UEdGraph`는 다음을 가진다(`RE/Classes/EdGraph/EdGraph.h:74–109`).
  - `Schema` 클래스, `Nodes` 배열
  - 편집·삭제·이름 변경 허용 플래그
  - 에디터 전용 `SubGraphs`·`GraphGuid`
- `GetSchema()`는 schema의 CDO를 돌려준다(`RE/Private/EdGraph/EdGraph.cpp:198–205`). 즉 schema는 상태 없는 전략 객체다.
- 노드 생성 도우미 `FGraphNodeCreator<T>`의 `Finalize`는 `CreateNewGuid` → `PostPlacedNewNode` → (핀이 없으면) `AllocateDefaultPins`를 차례로 부른다. `Finalize`를 빠뜨리면 check가 걸린다(`EdGraph.h:273–326`).

**노드.**
- `UEdGraphNode`는 다음을 가진다(`RE/Classes/EdGraph/EdGraphNode.h:293–406`).
  - 핀 배열 `TArray<UEdGraphPin*> Pins`. UPROPERTY가 아니다.
  - `int32` 위치·크기, 고급 핀 표시
  - 활성 상태: `Enabled`·`Disabled`·`DevelopmentOnly`(174행)
  - 주석 말풍선
  - 컴파일 메시지(`bHasCompilerMessage`·`ErrorType`·`ErrorMsg`), `NodeGuid`
- 핀 찾기(`FindPin`·`FindPinById`)는 선형 탐색이다(`EdGraphNode.cpp:412–462`).

**핀.**
- `UEdGraphPin`은 UObject가 아닌 C++ 클래스다(`RE/Classes/EdGraph/EdGraphPin.h:293–635`).
- 필드는 다음과 같다.
  - `PinId`(Guid)·이름·방향·`FEdGraphPinType`
  - 기본값 네 가지: 문자열·자동 생성 기본값·Object·Text
  - `LinkedTo`·`SubPins`·`ParentPin`. 모두 raw 포인터다.
- 생성은 `new`다(`RE/Private/EdGraph/EdGraphPin.cpp:507–512`).
- 삭제는 미룬다. `FPinDeletionQueue`가 다음 tick에 `delete`한다(31–87, 1799–1836). 위젯이 쥔 raw 포인터를 한 프레임 살려 두려는 것이다.
- 헤더 주석은 "TSharedPtr로 관리"라고 적어 실제와 다르다(`EdGraphPin.h:611`).

**연결.**
- `MakeLinkTo`는 양쪽 `LinkedTo`에 서로를 넣고 두 노드를 `Modify`한다. schema 검사는 하지 않는다(`EdGraphPin.cpp:514–542`).
- 대칭이 깨지면 ensure가 걸린다(661–703).

**split pin.**
- K2 schema는 부모 핀을 숨기고, `부모_자식` 이름의 자식 핀을 같은 `Pins` 배열에 만든다(`ED/BlueprintGraph/Private/EdGraphSchema_K2.cpp:7401–7470`).
- 기본 schema의 `SplitPin`·`RecombinePin`은 빈 함수다(`EdGraphSchema.h:1060–1063`).

**재구성.**
- 기본 `ReconstructNode`는 빈 함수다. 실제 알고리즘은 K2에 있다(`ED/BlueprintGraph/Private/K2Node.cpp:703–761`). 옛 핀을 치우고 → 다시 할당하고 → 옛 핀을 새 핀에 다시 잇는다.
- 다시 잇기는 O(N²)이고, 코드 주석도 그렇게 적는다(1399).
- 맞는 새 핀이 없는 옛 핀에 연결이나 사용자 값이 있으면 고아 핀(`bOrphanedPin`, 연결 불가)으로 남긴다(1456–1555).
- `MovePersistentDataFromOldPin`이 `PinId`·사용자 기본값·연결 순서를 옮긴다(`EdGraphPin.cpp:724–822`).

**소유.**
- 소유 사슬은 asset → graph → node다(`EdGraphNode.cpp:541–552`).
- 그래프는 asset의 에디터 전용 필드다. 예: `USoundCue::SoundCueGraph`(`RE/Classes/Sound/SoundCue.h:108–115`).

### 2.3 schema

**연결 응답**(`RE/Classes/EdGraph/EdGraphSchema.h:34–58`).
- 응답 값은 `MAKE`·`DISALLOW`·`BREAK_OTHERS_A/B/AB`·`MAKE_WITH_CONVERSION_NODE`·`MAKE_WITH_PROMOTION`이다.
- `FPinConnectionResponse`는 메시지와 치명 여부를 가진다(416–483).
- `TryCreateConnection`은 응답에 따라 기존 연결을 끊거나 변환 노드를 만든다(`RE/Private/EdGraph/EdGraphSchema.cpp:474–552`).

**노드 메뉴.**
- 기본 `GetGraphContextActions`는 **로드된 모든 `UClass`를 훑으며** 노드 CDO에 메뉴 항목을 묻는다(`EdGraphSchema.cpp:1140–1158`).
- 메뉴 항목 `FEdGraphSchemaAction`은 문구·tooltip·분류·검색 키워드·묶음 순서와 `PerformAction`을 가진다(`EdGraphSchema.h:93–350`).
- 새 노드 항목은 template을 `DuplicateObject`로 복제한 뒤 GUID·핀 할당·자동 연결·격자 맞춤을 한다(`EdGraphSchema.cpp:372–432`).
- 검색 순위는 `GetActionFilteredWeight`가 정한다(855행 이후).

**화면 hook이 schema에 있다.**
- 핀 색(`GetPinTypeColor`, `EdGraphSchema.h:966–978`), 연결선 그리기 정책 생성(1271), 기본값 숨김(948)
- `ShouldAlwaysPurgeOnModification`의 기본값이 true다(1279). 즉 변경마다 화면 전체를 다시 만든다(2.5절).

**기타 hook.**
- 자산 drop: `DroppedAssetsOnGraph/Node/Pin`(1212–1255)
- 연결선 더블클릭 → 재경유 노드 삽입: `OnPinConnectionDoubleCicked`(1068, 철자 오류가 API에 남아 있음)

**Blueprint 결합.**
- 기본 schema API에 `UBlueprint*`·`FBPVariableDescription`·`FProperty` drop 함수가 있다(1345–1469).
- 기본 이름 검사기는 `FKismetNameValidator`다(`EdGraphSchema.cpp:1204–1207`).

**검증.** 그래프 단위 검증 함수는 없다. 노드마다 `ValidateNodeDuringCompilation`으로 검증한다(`EdGraphNode.h:902`).

### 2.4 undo·복사·붙여넣기

**undo.**
- 편집은 `FScopedTransaction` 안에서 대상마다 `Modify()`를 불러, 객체 상태를 통째로 기록한다.
- 핀은 노드 직렬화 안에서 `(소유 노드, PinGuid)`로 기록된다(`EdGraphPin.cpp:1838–1892, 2132–2176`).
- 복원할 때는 `thread_local` 보류 표로 상대 핀 포인터를 다시 잇는다(466, 2245–2268).
- 연결의 한쪽 노드만 트랜잭션에 기록되면 `LinkedTo` 대칭이 깨진다. 코드는 이것을 "transaction buffer is corrupt"로 기록하고 그래프를 `FGraphConnectionSanitizer`에 넘긴다(2212–2261).
- 에디터는 undo·redo 뒤마다 `SanitizePinsPostUndoRedo()`를 부른다. 주석은 이것을 안정성 보완용 임시 코드라고 적는다(`ED/UnrealEd/Private/EditorServer.cpp:5557–5561`).

**복사.**
- `FEdGraphUtilities::ExportNodesToText`는 `UExporter`의 T3D 텍스트로 내보낸다(`ED/UnrealEd/Private/EdGraphUtilities.cpp:458–481`).
- 핀은 손으로 쓴 `CustomProperties Pin (...)` 줄로 나간다(`EdGraphPin.cpp:1077–1255`).

**붙여넣기.**
- `ImportNodesFromText`는 선택 밖으로 나가는 연결을 끊고, 노드마다 `PostPasteNode`·`ReconstructNode`를 부른다. 붙일 수 없는 노드는 schema가 대체 노드로 바꾼다(`EdGraphUtilities.cpp:46–248, 484–497`).
- **NodeGuid 재발급은 각 편집기가 직접 한다.** 예: `ED/Kismet/Private/BlueprintEditor.cpp:7733–7827`.
- `ImportNodesFromText`를 부르는 파일이 약 24개다(직접 호출 20곳).

### 2.5 통지와 화면 갱신

**통지.**
- 종류는 `GRAPHACTION_Default`·`AddNode`·`SelectNode`·`RemoveNode`·`EditNode`뿐이다(`RE/Public/GraphEditAction.h:8–52`).
- 핀·연결 단위 통지가 없다. 연결 변화는 노드 virtual(`PinConnectionListChanged`·`NodeConnectionListChanged`)로만 전달된다.

**화면 갱신.** `SGraphPanel::OnGraphChanged`(`ED/GraphEditor/Private/SGraphPanel.cpp:2639–2820`)의 동작은 다음과 같다.
- 기본 설정이면 모든 노드 위젯을 버리고 다시 만든다.
- 추가·삭제는 한 프레임 미룬다. `AddNode`가 핀 할당보다 먼저 통지하기 때문이다(2693–2697).
- `EditNode`는 노드 위젯 전체를 다시 만든다.
- 연결선 그리기 정책 객체를 paint마다 새로 만든다(527–530). 주석은 "@TODO … mutating every pin state every frame"(648–657)이다.

### 2.6 화면(GraphEditor)

| 구성 | UE | 근거 |
|---|---|---|
| facade | `SGraphEditor`가 모듈의 구현 위젯으로 위임한다(모듈 unload 때 자리 위젯으로 교체). 이벤트 묶음은 `FGraphEditorEvents` | `ED/UnrealEd/Public/GraphEditor.h:104–777`, `ED/UnrealEd/Private/GraphEditor.cpp:26–141` |
| 캔버스 | `SNodePanel`: pan·zoom, 20단계 zoom(0.1–2.0, 기본 1:1)을 LOD 5등급에 대응, 선형 culling·marquee, 격자 맞춤, 가장자리 자동 pan, `TSet<UObject*>` 선택 | `ED/GraphEditor/Private/SNodePanel.cpp:54–123, 494–530, 1297–1436, 1690–1709`, `Public/SNodePanel.h:73–293` |
| 그리기 순서 | 주석 → diff 강조 → 연결선 → 그림자 → 노드, 선택 노드는 마지막. 화면 밖 노드도 연결선용 핀 위치를 합성 | `SGraphPanel.cpp:183–606` |
| 노드·핀 위젯 | `SGraphNode::UpdateGraphNode`가 위젯 트리를 통째로 만든다. 핀 표시 규칙에 K2 범주(`PC_Exec`·`PN_Self`)가 하드코딩되어 있다. 노드가 자기 위젯을 역참조(`DEPRECATED_NodeWidget`)하며, 헤더가 이것을 "안전하지 않다"고 경고 | `SGraphNode.cpp:550, 847, 1198–1224`, `EdGraphNode.h:509–510` |
| factory | node 위젯: 노드 자신 → 등록 factory → K2 cast 사슬 → 기본. pin 위젯: 등록 factory → K2 범주 switch(기본값 편집기 선택) → 기본. 연결선 정책: schema → factory → K2 → 기본. "@TODO: Fold all of this code into registered factories" | `ED/GraphEditor/Private/NodeFactory.cpp:87–384` |
| 연결선 | cubic Hermite, 접선은 설정값. hover는 bounding box 선검사 후 16구간 표본 최소 거리. 자르기 선 교차 검사 | `ConnectionDrawingPolicy.cpp:300–760`, `Public/GraphSplineOverlapResult.h:13–93` |
| drag | `FDragConnection`: hover 때 `CanCreateConnection`, drop 때 transaction 안 `TryCreateConnection`, 빈 곳 drop은 노드 메뉴. 그 밖에 `FDragNode`, palette 항목 drag | `DragConnection.cpp:65–505` |
| 노드 메뉴·palette | `SGraphActionMenu`: tree view, 필터 토큰화, schema 가중치 순위. `SGraphPalette`가 감싼다 | `SGraphActionMenu.cpp:372, 1288–1344` |
| 공통 명령 | 정렬·분배·쌓기·곧게 펴기, 접기·펼치기, 활성 전환, 북마크 이동(QuickJump), 상·하류 선택, 연결 없는 핀 숨기기, 삭제 후 다시 잇기, 핀 연결 끊기, 기본값 복원 등. 공통 위젯은 4개만 결합하고 나머지는 host 편집기가 결합 | `ED/GraphEditor/Public/GraphEditorActions.h:22–165`, `SGraphEditorImpl.cpp:648–673` |
| 기타 | 찾기(`SFindInGraph`, 선형), diff(`FGraphDiffControl`: 클래스 + `NodeGuid` 일치, 없으면 제목, O(N²)), 읽기 전용 미리보기, 설정(핀 간격·곡선·타입 색), 노드 맞붙이기(약 1,054줄, 현재 꺼짐). 미니맵은 없다 | `FindInGraph.cpp:265–346`, `GraphDiffControl.cpp:706–779`, `NodeSnappingManager.cpp:532–535` |
| 디버그 표시 | 공통: 노드 오류 표지·overlay hook. 공통 층에 섞인 Kismet 전용: breakpoint overlay, pin watch(`SGraphPin`이 `FKismetDebugUtilities` 직접 호출), paint 안의 `FKismetNodeInfoContext` | `SNodePanel.h:670–690`, `SGraphPin.cpp:932–944, 1301–1330`, `SGraphPanel.cpp:289` |
| 의존 | GraphEditor.Build.cs가 Kismet·KismetCompiler·BlueprintGraph·Persona를 끌어온다 | `ED/GraphEditor/GraphEditor.Build.cs:23–45` |

### 2.7 도메인 사용례: 권위 데이터와 동기화

| 시스템 | 권위 데이터 | 편집 그래프 | 동기화 | 문제 흔적 |
|---|---|---|---|---|
| 머티리얼 | `UMaterialExpression` + 입력 쪽 `FExpressionInput` | 저장하지 않는다(UPROPERTY 아님, `RE/Public/Materials/Material.h:1246–1251`). 열 때 `RebuildGraph` | 데이터 → 그래프 전체 재구성(`ED/UnrealEd/Private/MaterialGraph.cpp:55–371`). 그래프 → 데이터는 연결·위치 전체 복사(`LinkMaterialExpressionsFromGraph`, 496–635)를 연결·끊기·삭제·Apply마다 | undo 뒤 역참조 복구(정의 `ED/MaterialEditor/Private/MaterialEditor.cpp:252–263`, PostUndo 호출 7388), 붙여넣기 뒤 핀 범주 복구(6526–6540), "Unclear why this is null sometimes"(`MaterialGraph.cpp:453`), 순환은 경고만("TODO: re-enable", `MaterialGraphSchema.cpp:547`) |
| Sound Cue | `USoundNode` 트리 | 에디터 전용으로 저장 | 그래프 → 데이터 `CompileSoundNodesFromGraphNodes`를 연결·핀 추가·삭제마다(`ED/AudioEditor/Private/SoundCueGraph.cpp:76–131`) | 복사 때 모델 객체를 그래프 노드 밑으로 옮기는 꼼수(`SoundCueGraphNode.cpp:171–211`) |
| Behavior Tree | 편집 그래프 | — | 그래프 → 실행 트리 전체 재생성 `UpdateAsset`(`ED/BehaviorTreeEditor/Private/BehaviorTreeGraph.cpp:102–187, 902–959`) | **자식 실행 순서를 노드 X 좌표로 정렬**(551, 700, 1189), 모델 코드가 위젯 크기를 읽음(1236–1247) |
| 애니메이션 그래프 | 편집 노드가 실행 struct(`FAnimNode_*`)를 멤버로 품음 | Blueprint 그래프 | Kismet 컴파일 때 struct를 생성 클래스 CDO로 복사 | 동기화 문제는 적다. 대신 Blueprint 컴파일러 전체에 묶인다 |
| Niagara | `UNiagaraGraph : UEdGraph` 자체 | 같음 | 컴파일은 불변 digest 사본으로(`NiagaraGraphDigest.h`) | 그래프 = 자산이라 schema·화면 결합을 그대로 진다 |
| PCG | `UPCGGraph`·`UPCGNode`·`UPCGPin`·`UPCGEdge`(런타임 UObject) | transient `UPCGEditorGraph` | 모델 → 그래프 `ReconstructGraph`(`PL/PCG/Source/PCGEditor/Private/PCGEditorGraph.cpp:70–151`). 그래프 → 모델은 schema 연결·끊기 override(`Schema/PCGEditorGraphSchema.cpp:287–470`). 주석·재경유는 저장 직전에만 되돌려 복사(`PCGEditorGraph.cpp:189–212`) | 링크를 조용히 건너뜀(`PCGEditorGraph.cpp:345–356`), "crashes where one of these nodes was nullptr"(`PCGEditorGraphSchema.cpp:304`), "TODO: evaluate creating a clipboard object instead of ownership hack"(`PCGEditor.cpp:3123`), undo 중 재구성 억제(`GIsTransacting`) |
| Dataflow | `UDataflow : UEdGraph`가 `Dataflow::FGraph`를 함께 소유, 둘 다 저장 | 같은 객체 | 핀 연결 변화 diff로 모델 갱신. 로드·undo 뒤 전 핀 재동기화(`DataflowEdNode.cpp:677–785, 1100–1124`) | "Modify(); // TODO: How do we modify a DataflowNode"(547, 581) |

**공통 사실.**
- 머티리얼·PCG·Sound Cue의 컴파일·번역은 편집 그래프를 읽지 않고 별도 모델을 읽는다.
- 그런데도 편집 그래프를 따로 두고 양방향으로 맞추느라, 시스템마다 보정 코드가 붙었다.
  - undo 뒤 보정
  - 복사 소유권 꼼수
  - 재구성 억제 플래그
  - 저장 직전 복사

### 2.8 모델 우선 구조

| | RigVM | MetaSound Frontend | Dataflow·PCG |
|---|---|---|---|
| 모델 | UObject 그래프(`URigVMGraph`·`Node`·`Pin`·`Link`). 모듈은 UncookedOnly(`PL/Runtime/RigVM/Source/RigVMDeveloper`) | USTRUCT 문서(`FMetasoundFrontendDocument`, `PL/Runtime/Metasound/Source/MetasoundFrontend/Public/MetasoundFrontendDocument.h:2045`). Runtime 모듈 | 2.7절 |
| 변경 경로 | `URigVMController` 하나(약 200개 함수, `.cpp` 25,645줄). 모두 `UFUNCTION`이라 Python에서 호출 가능. 변경마다 Python 한 줄 출력 가능 | `FMetaSoundFrontendDocumentBuilder`(`MetasoundFrontendDocumentBuilder.h:184`). 쿠킹된 게임에서도 `UMetaSoundBuilderSubsystem`으로 그래프를 만들고 재생 중 연산자에 실시간 반영(`MetasoundDynamicOperatorTransactor.h:183`) | schema override |
| 식별 | 문자열 경로(`Node.Pin.SubPin`, 중첩 그래프는 `\|`). 이름을 바꾸면 경로가 바뀌어 controller가 링크·기록을 고친다 | node·vertex·page·member 모두 Guid, 간선 = Guid 넷(`:706`). 쿠킹용 결정적 Guid 생성 범위(`MetasoundFrontendDocumentIdGenerator.h:48–77`) | Guid |
| undo | 직렬화한 작은 action struct(약 100종)의 자체 스택: 병합·bracket(drag)·취소. UE 트랜잭션에는 `ActionIndex` 하나만 기록하고 `PostTransacted`에서 자체 스택을 재생한다(`RigVMControllerActions.h:20–35, 230–445`, `RigVMControllerActions.cpp:184–224`). 근거 주석: 모든 변경 출처(UI·Python·undo)가 같은 통지를 내게 하려고 | 문서 전체 UE 트랜잭션 후 builder 재적재·화면 강제 갱신 | UE 트랜잭션 + 보정 |
| 통지 | 약 50종 세분 이벤트(`RigVMNotifications.h:16–71`), 억제 guard, 전체 재생(`ResendAllNotifications`), 재컴파일 bracket | 색인 기반 modify delegate + 에디터 dirty 집합을 화면이 **매 Tick 조회**: `FEditor::Tick` → `RefreshEditorContext` → `SynchronizeGraph`(`MetasoundEditor.cpp:5190–5200`, `MetasoundEditorGraphBuilder.cpp:1836`) | 그래프 변경 delegate |
| 화면 | `URigVMEdGraph`가 통지를 따라가는 사본(`RigVMEdGraph.cpp:182–520`). 역방향은 전부 controller 호출. EdGraph 핀 이름 = 모델 핀 경로 | transient EdGraph를 동기화로 생성(`MetasoundSource.h:152–156`) | 2.7절 |
| 복사 | **모델** 객체 텍스트 내보내기(`RigVMController.cpp:4054–4343`) | EdGraph 텍스트 | 소유권 꼼수 |
| 노드 버전 | struct의 `GetUpgradeInfo`(새 struct + 핀 경로 재지정), `UpgradeNodes`, 고아 핀 | minor 자동 갱신, `INodeUpdateTransform`(여러 노드 → 하나, 5.7부터 실험 표지, `MetasoundFrontendNodeUpdateTransform.h:63`), 노드 이전 정보, 템플릿 노드 펼침 | 재구성 |
| 타입 | 타입 색인 registry. template·permutation·wildcard 해석을 controller가 undo와 함께 | 이름 키 data type registry, 변환 노드 | 타입 이름·변환 노드 |

UAF(AnimNext)는 RigVM의 model·controller·view 구조를 그대로 상속한다(`PL/Experimental/UAF/UAF/Source/UAFUncookedOnly/Internal/AnimNextRigVMAssetEditorData.h:182`).

조사한 범위에서 UE 5.8에는 여러 시스템이 함께 쓰는 새 그래프 모델이 없다. GameplayGraph·StateGraph·PostProcessMaterialChainGraph는 노드 편집 체계가 아니다. 공통으로 남은 것은 화면 층(EdGraph·SGraphEditor)이다.

### 2.9 Rocks가 가져갈 것과 버릴 것

| UE 사실 | Rocks 결정 |
|---|---|
| 도메인 모델과 편집 그래프의 양방향 동기화(2.7절)에 보정 코드가 쌓였다. 신규 시스템(RigVM·MetaSound·PCG)은 모델을 권위로 삼는다 | 모델 하나, 화면은 모델을 직접 읽는다(GD1) |
| 핀이 비UObject raw 객체라 직렬화·GC·undo·텍스트·삭제 큐·sanitizer를 손으로 만들었다(2.2·2.4절) | 핀을 인스턴스로 저장하지 않는다(GD3) |
| 양방향 raw 포인터 연결은 대칭이 깨지고, `MakeLinkTo`가 schema를 건너뛴다 | 간선 목록 한 곳, 변경은 `GraphMutator`만(GD4·GD5) |
| 객체 snapshot undo는 연결 한쪽만 기록될 때 깨진다. RigVM은 명령 기록으로 같은 통지를 보장한다 | patch 기반 undo. 편집·undo·redo·스크립트가 같은 경로·같은 통지(GD6) |
| 통지 4종 + 기본 전체 재생성 | 세분 통지(GD7)와 증분 화면 갱신(GD15) |
| 노드가 위젯을 만들고 역참조하며, schema가 색·그리기 정책을 가진다 | 모델에 화면 코드 없음, 시각은 `GraphVisualRegistry`(GD9·GD15) |
| 공통 화면이 Kismet에 의존 | 공통 층의 도메인 의존 금지, 정책으로 검사(GD17) |
| 메뉴 구성 때 모든 UClass를 훑는다 | 노드 종류 등록표 조회(GD11) |
| 붙여넣기 GUID 재발급을 편집기마다 따로 한다 | 공통 층이 항상 재발급(GD12) |
| BT 실행 순서를 X 좌표로 정한다 | 순서는 간선의 명시 순번, 배치 데이터는 의미에 영향 없음(GD4·GD8) |
| 문자열 경로 식별은 이름 변경에 약하다. MetaSound Guid는 순서 변경에 강하다 | Guid + 서술의 안정 키(GD2) |
| RigVM controller는 25,645줄짜리 단일 클래스 | 공통 명령은 작은 고정 집합, 도메인 명령은 그 조합(GD5) |

## 3. 범위와 원칙

**범위.**
- 데이터 모델·schema 접점·변경·undo·통지
- 복사·붙여넣기·텍스트 형식
- 노드 등록·버전
- 진단 형식
- 그래프 화면
- 도메인 적용 규칙(12절)

**범위 밖.**
- 도메인별 노드 의미·타입 체계·컴파일러(각 도메인 문서)
- 실행 VM, gameplay 비주얼 스크립팅(GD20)
- multi-user 동시 편집(계획 22.2절 P3 이후)

**원칙.**
1. 그래프 모델 모듈(`NodeGraph`)은 Object 층(ObjectRuntime)·위젯·장치에 의존하지 않는다. 창·GPU 없이 시험한다. 속성 해석(리플렉션 타입 조회)은 그 위 층이 한다(GD23).
2. 화면은 모델을 바꾸지 않는다. 바꿀 때는 EditorBackend 명령을 낸다(계획 22.1절).
3. 의미 데이터와 배치 데이터를 섞지 않는다.
4. 같은 입력은 같은 결과를 낸다. Guid 발급·저장 순서·텍스트 출력을 결정적으로 만든다. 시험은 시간·경쟁에 기대지 않는다.
5. UE의 기능 범위를 따른다. 2.9절의 구조 결함은 따르지 않는다. 애매한 곳은 UE 방식을 따른다.

## 4. 용어 대응

이름공간은 `rocks::nodegraph`다. 같은 이름이 머티리얼 쪽 예정 파일(계획 14.6절 `MaterialSchema/Public/Material/GraphSchema.h`)과 겹쳐도 이름공간으로 구분한다.

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| 그래프 문서 `GraphDocument` | 그래프를 가진 asset의 에디터 전용 그래프 집합(`UBlueprint::UbergraphPages` 등), MetaSound `FMetasoundFrontendDocument` | 자산 하나의 그래프 묶음(주 그래프 + 하위 그래프)의 의미 데이터 |
| 그래프 배치 `GraphDocumentLayout` | `UEdGraphNode` 위치·크기, `UEdGraphNode_Comment` | 같은 문서의 배치 데이터(별도 struct, GD8) |
| 그래프 `Graph`, `GraphId` | `UEdGraph`, `GraphGuid` | 노드·간선 묶음 하나 |
| 노드 `GraphNode`, `NodeId` | `UEdGraphNode`, `NodeGuid` | 노드 인스턴스 |
| 노드 종류 `NodeTypeId`, 노드 종류 서술 `NodeTypeDescriptor` | `UEdGraphNode` 파생 클래스, RigVM unit struct, MetaSound class(이름 + 버전) | 핀 서술·속성 struct·버전·분류 |
| 핀 키 `PinKey`, 핀 서술 `PinDescriptor` | `UEdGraphPin`(`PinId`·`PinName`), RigVM 핀 경로 | 노드 종류가 선언한 안정 키와 방향·타입 제약 |
| 간선 `GraphEdge` | `UEdGraphPin::LinkedTo`, `URigVMLink`, `FMetasoundFrontendEdge` | 출력 핀 → 입력 핀 |
| 고아 핀 `RetainedPin` | orphaned pin(`bOrphanedPin`) | 서술에서 사라졌지만 연결·값이 남아 보존한 핀 |
| 속성 Record `PropertyRecord` | (UE는 노드 UObject 자체) | 노드 속성의 리플렉션 Record 바이트(struct Guid + 버전 + 내용) |
| 그래프 schema `GraphSchema` | `UEdGraphSchema`, `URigVMSchema` | 도메인 정책(타입·연결·메뉴·검증) |
| 연결 판정 `ConnectVerdict` | `FPinConnectionResponse` | 연결 가능 여부와 처리 방법 |
| 그래프 변경기 `GraphMutator` | `URigVMController`, `FMetaSoundFrontendDocumentBuilder` | 유일한 변경 API |
| 그래프 patch `GraphPatch` | RigVM action, UE transaction 기록 | 되돌릴 수 있는 변경 기록 |
| 그래프 변경 통지 `GraphChange` | `FEdGraphEditAction`, `ERigVMGraphNotifType` | 세분 변경 이벤트 |
| 그래프 조각 `GraphFragment` | T3D 복사 텍스트 | 복사·붙여넣기·template 단위 |
| 노드 메뉴 항목 `NodeMenuEntry` | `FEdGraphSchemaAction` | palette·검색 항목 |
| 그래프 진단 `GraphDiagnostic` | `ErrorType`·`ErrorMsg`·`FCompilerResultsLog` | 위치가 있는 오류·경고 |
| 그래프 화면 `GraphView` | `SGraphEditor`·`SGraphPanel` | UiToolkit 6절의 그래프 Element |
| 그래프 캔버스 `GraphCanvas` | `SNodePanel` | pan·zoom·선택·culling |
| 시각 등록 `GraphVisualRegistry` | `FGraphPanelNodeFactory`·`FGraphPanelPinFactory`·`FGraphPanelPinConnectionFactory` | 노드·핀·연결선 표현 등록 |
| 연결선 그리기 `WirePainter` | `FConnectionDrawingPolicy` | 곡선·hover·자르기 판정 |
| 재경유 노드 `Reroute` | knot(`UK2Node_Knot`·`SGraphNodeKnot`) | 배치용 경유점(의미 비교 때 접힘) |
| 주석 상자 `CommentBox` | `UEdGraphNode_Comment` | 배치 데이터의 묶음 상자 |
| 그래프 입출력 노드 `GraphInput`·`GraphOutput`, 접힌 그래프 노드 `CollapsedGraph` | `UK2Node_Tunnel`·`UK2Node_Composite`, `URigVMCollapseNode` | 하위 그래프 경계(GD25) |

## 5. 결정

| ID | 사안 | 권장안 | 버린 대안 | 근거 |
|---|---|---|---|---|
| GD1 | 권위 데이터 | 도메인과 무관한 `GraphDocument` 하나. 도메인 자산이 이것을 필드로 갖고, 화면은 이것을 직접 읽는다 | (가) UE EdGraph처럼 그래프 = 화면 겸 데이터 객체. (나) 도메인 모델 + 편집 그래프 양방향 동기화(머티리얼·PCG 방식). (다) RigVM처럼 모델 + 통지로 따라가는 화면용 사본 | (가)는 2.2·2.6절의 결합, (나)는 2.7절의 보정 코드를 낳는다. (다)의 사본은 UE `SGraphEditor`가 `UEdGraph`를 요구해서 생긴 것이다. Rocks 화면은 새로 만들므로 사본이 필요 없다 |
| GD2 | 식별 | `NodeId`·`GraphId`·주석 id는 Guid. `PinKey`는 Name이다. 정적 핀의 키는 서술이 선언한 이름이고 표시 이름과 독립이다. 동적 핀의 키는 도메인이 안정 id에서 만든 Name이다(예: 함수 호출 입력 `P_<ParameterId 32자 hex>`) | RigVM 문자열 경로, UE 핀별 `PinId` Guid, 정적·동적 키 타입 분리 | 머티리얼 시스템 5.2·5.5절과 같다. 경로는 이름 변경에 약하고, 핀별 Guid는 핀을 인스턴스로 저장해야 한다. 리플렉션 Map 키 종류는 Name·String·Guid·정수뿐이다([리플렉션 확장](ReflectionExtensions.ko.md) 1.3절). Name 하나면 두 종류를 같은 형식으로 다룬다 |
| GD3 | 핀 저장 | 노드 인스턴스에 핀 배열을 저장하지 않는다. 저장하는 것은 입력 기본값 override(`PinKey` → 값)와 고아 핀뿐이다. 핀 목록은 서술 + 노드 속성에서 유도한다 | UE처럼 노드마다 핀 객체 배열 | 2.2·2.4절의 손 직렬화·삭제 큐·sanitizer가 필요 없다. 저장 크기가 작고, 서술 개정이 자동 반영된다 |
| GD4 | 간선 저장과 순서 | 그래프마다 간선 목록 하나: `(출력 NodeId, PinKey, 입력 NodeId, PinKey, 순번)`. 입력 핀의 최대 간선 수는 서술이 정한다(기본 1). 순번은 **출력 핀 쪽**에만 의미가 있다(`ordered` 출력 핀, 6.2절). 순서가 의미 있는 입력 묶음은 동적 입력 핀으로 표현한다 | 양방향 `LinkedTo`, 입력 쪽 순번 | 대칭 유지 문제가 없다. 순번은 실행 순서가 의미 있는 도메인(BT형 자식 순서·흐름 출력)에 쓴다. 좌표로 순서를 정하지 않는다. 입력 묶음은 Sound Cue처럼 입력 핀 여럿으로 둔다(`USoundNode::ChildNodes` ↔ 입력 핀, `SoundCueGraph.cpp:40–74`) |
| GD5 | 변경 경로 | `GraphMutator`만 문서를 바꾼다. 공통 명령은 고정 집합이다(8.1절). 도메인 명령(변환 노드 삽입·함수 펼치기 등)은 공통 명령의 조합이다 | 모델 setter 공개, RigVM식 거대 controller | 검증·patch·통지를 한 곳에서 보장한다. 도메인 명령이 조합이라 undo·통지를 따로 짜지 않는다 |
| GD6 | undo | 명령마다 `GraphPatch`(정방향 op + 역방향 op)를 만들어 EditorBackend Transaction에 넣는다. drag는 bracket으로 묶어 release 때 하나로 확정한다. undo·redo는 역·정 patch를 `GraphMutator`로 적용한다. patch 크기는 Transaction의 undo 메모리 budget(계획 22.2절)에 들어간다 | 문서 snapshot(MetaSound), UE 트랜잭션 + 자체 스택 다리(RigVM) | 계획 22.2절의 Transaction 계약이 stable ID·old/new·patch 기반이다. snapshot은 큰 그래프에서 메모리가 크고, 세분 통지를 내지 못한다 |
| GD7 | 통지 | 세분 통지 `GraphChange`(8.3절). 한 명령의 통지는 끝에 묶음 하나로 보낸다 | UE 4종 통지, MetaSound의 Tick 조회 | 화면 증분 갱신·컴파일 debounce가 통지 종류로 판단한다. 시간 조회에 기대지 않는다 |
| GD8 | revision·hash·배치 분리 | 문서 revision 하나가 모든 편집(배치 포함)에서 오르며 충돌 검사(expected revision)에 쓴다. 통지에는 "의미 데이터를 건드림" 표지를 단다. 컴파일 여부는 semantic hash(지연 계산)로 판정한다. 노드 위치·크기·접힘·고급 핀 표시·말풍선·주석 상자는 `GraphDocumentLayout`(별도 struct)에 두고 hash·실행 순서에 넣지 않는다 | 의미·배치 revision 둘(개정 1), 노드에 위치를 함께 저장하고 hash에서만 제외 | 계획 22.1절 명령의 `expectedRevision`은 하나다. 계획 14.8절은 "document revision만 바뀐 경우 같은 semantic 결과 사용"을 전제한다. 배치를 별도 struct로 두면 도메인 자산이 그 필드만 EditorOnly로 표시할 수 있다(9.1절) |
| GD9 | schema 책임 | `GraphSchema`는 정책만 맡는다: 타입 표, 연결 판정, 변환 노드 선택, 메뉴 항목, 기본 노드, 검증, 붙여넣기 허용, 의미 정규화. 색·위젯·그리기는 화면의 `GraphVisualRegistry`가 맡는다 | UE `UEdGraphSchema`(화면·Blueprint 함수 포함) | 2.3절. schema는 headless 쿠커에도 링크되어야 한다 |
| GD10 | 타입 | 공통 층은 `PinTypeId`(도메인 타입 표의 색인)와 호환 질의만 안다. wildcard·template 해석은 선택 기능이다. 도메인이 켜면 해석 결과를 노드 속성으로 저장하고 patch에 넣는다 | 공통 타입 체계(UE `FEdGraphPinType`의 범주·하위 범주) | 도메인마다 타입이 다르다(머티리얼 5.3절, 오디오 신호, PCG 데이터). RigVM도 해석을 controller가 undo와 함께 한다 |
| GD11 | 노드 종류 등록 | 등록표(`NodeTypeRegistry`)에 서술을 module lease와 함께 등록한다. 해제는 새 사용 차단 → 진행 작업 drain 뒤다. 메뉴는 등록표를 조회한다 | UClass 전체 순회 | 계획 14.6절 `RegisterMaterialNode` 규칙을 일반화했다 |
| GD12 | 복사·붙여넣기 | `GraphFragment`를 정규 JSON 텍스트로 만든다. 쓰기는 `JsonWriter` Canonical(엄격 JSON = JSON5의 부분집합), 읽기는 Json5 문법도 받는다. 붙여넣기는 공통 층이 NodeId·주석 id를 항상 재발급하고 바깥 간선을 끊는다. 같은 문서이고 도메인이 허용하면 바깥 간선을 유지한다. 다른 도메인 조각은 거부한다 | UE T3D + 소유권 꼼수, 편집기별 Guid 재발급, JSON5 쓰기 | 2.4·2.7절. 텍스트는 사람이 읽고 diff·golden 시험에 쓴다. Core의 JSON 쓰기는 늘 엄격 Canonical이다(`Json.h:136–142`) |
| GD13 | 노드 버전 | 서술의 `schemaVersion`과 migration 함수(옛 키 → 새 키 재지정, 속성 변환, 1:1 노드 교체). 여러 노드 → 하나 교체는 후속(NG8) | UE `ReconstructNode` 재구성 | RigVM `GetUpgradeInfo`·MetaSound 자동 갱신과 같은 방향이다 |
| GD14 | 모르는 노드 | 등록되지 않은 `NodeTypeId`는 속성 Record 바이트를 그대로 보존하는 자리 노드로 연다. 편집·저장 왕복에서 바이트가 바뀌지 않는다. 출력에 닿으면 도메인 컴파일 오류다 | 열기 실패, 삭제 | 머티리얼 시스템 V12·E13, 계획 14.6절. 속성이 Record 바이트(GD23)라서 리플렉션 쪽 보존 기능 없이도 성립한다 |
| GD15 | 화면 갱신 | 통지 단위 증분 갱신. 노드 하나가 바뀌면 그 노드 시각만 다시 만든다. 전체 재생성은 문서 다시 읽기 때만 한다. 노드 시각은 모델을 포인터로 잡지 않고 `NodeId`로 찾는다 | UE의 기본 전체 재생성 | 2.5절 |
| GD16 | 화면 공간 색인 | 노드 hit test는 UiCore hit grid(노드 시각이 Element이므로)를 쓴다. 캔버스 좌표의 culling·연결선 hover·marquee는 공용 2D 균일 격자를 쓴다. 이 격자는 [공간 색인](SpatialIndex.ko.md) 5.3절 공용 2D 균일 격자이고, GraphView에 따로 만들지 않는다 | UE 선형 탐색, GraphView 전용 격자 | 2.6절. UiCore hit grid는 그려진 Element만 담으므로 화면 밖 culling에는 쓸 수 없다. 범용 기능을 기능 모듈에 따로 구현하지 않는다 |
| GD17 | 공통 층 경계 | NodeGraph·NodeGraphTools·GraphView는 어떤 도메인 모듈에도 의존하지 않는다. 도메인 전용 위젯·디버그 표시는 시각 등록과 overlay hook으로 넣는다. policy 검사로 막는다 | UE GraphEditor의 Kismet 의존 | 2.6절 |
| GD18 | 주석 상자 소속 | 주석 상자 안 노드 목록을 저장하지 않는다. 상자 drag 시작 때 기하로 계산해 이동 patch에 넣는다 | UE `NodesUnderComment`를 선택 때 갱신 | 저장 데이터가 기하와 어긋나지 않는다 |
| GD19 | 진단 | `GraphDiagnostic{AssetId, GraphId, NodeId, PinKey(선택), 심각도, 코드, 메시지}`. 도메인 검증·컴파일러가 내고 화면은 그대로 표시한다. 노드에 오류 필드를 저장하지 않는다 | UE 노드 필드 `ErrorType`·`ErrorMsg` | 머티리얼 시스템 5.4절의 `(AssetId, NodeId, PinKey, 코드, 메시지)`에 GraphId를 더했다. 진단은 컴파일 결과이고 문서 상태가 아니다 |
| GD20 | 실행 흐름·비주얼 스크립팅 | 공통 층은 핀 종류(데이터·흐름·구조)만 구분하고 실행하지 않는다. 범용 gameplay 노드(객체 생성·RPC·게임 이벤트 조합)를 등록하지 않는다. Sparks 시각 편집은 LD2대로 보류하고, 만들게 되면 Sparks 코드를 생성한다 | Blueprint형 그래프 VM | 계획 14.5·22.2·24.3절, LevelScript LD1·LD2 |
| GD21 | 모듈 배치 | `NodeGraph`(Engine Runtime, Core·MetaSchema·Serialization만), `NodeGraphTools`(Engine Developer, 텍스트·commandlet), `GraphView`(Engine Editor, 새 모듈). 11.1절 | 모델을 Editor 계층에 둠, 화면을 EditorFramework 안에 둠 | MaterialSchema(Runtime, 의존 Core·MetaSchema·Serialization — 머티리얼 시스템 3.2절)가 NodeGraph에 의존하므로 Developer 이상에 둘 수 없다. 의존을 MaterialSchema와 같게 맞췄다. 화면은 UE 공통 부분만 약 29,000줄이라 EditorFramework와 나눈다 |
| GD22 | 자동화 | 공통 명령을 EditorBackend 명령 schema로 노출한다(계획 22.1절 외부 RPC). commandlet `graph-dump`·`graph-validate`·`graph-diff`를 둔다 | RigVM식 Python 한 줄 출력 | 계획 22.1절에 typed 명령·JSON RPC 계약이 이미 있다. 텍스트 dump는 golden 시험과 source control diff에 쓴다 |
| GD23 | 속성 저장 | 노드 속성은 `PropertyRecord`, 즉 리플렉션 Record 바이트(InstancedStruct wire 형식: struct Guid + schema version + 내용)로 보관하고 patch에도 바이트로 기록한다. 해석(타입 조회·decode·텍스트화)은 도메인과 NodeGraphTools가 필요할 때 lease를 잠깐 잡고 한다 | 노드에 `InstancedStruct` 값으로 보관 | `InstancedStruct`는 ObjectRuntime에 있고 TypeLease를 쥔다(`ObjectRuntime/Public/Object/InstancedStruct.h:1–7`). 그러면 원칙 1이 깨지고, 노드 종류 해제가 열린 문서·undo 이력의 lease 때문에 막힌다. 미등록 struct decode는 거부·비움만 된다(리플렉션 확장 11절). 바이트 보관이면 세 문제가 함께 풀린다 |
| GD24 | 스레드 소유 | 문서는 소유 lane 하나(EditorBackend가 정한 에디터 lane)에서만 읽고 바꾼다. 다른 스레드의 RPC 명령은 그 lane에 게시한다. 컴파일러·검색 색인에는 불변 snapshot(문서 복사)을 넘긴다 | 잠금으로 여러 스레드 접근 | 계획 22.1절 thread affinity 검증, 머티리얼 시스템의 compiler snapshot(계획 14.6절)과 같다 |
| GD25 | 하위 그래프 경계 | 공통 노드 종류 `GraphInput`·`GraphOutput`(하위 그래프 안, 속성에 경계 핀 목록)과 `CollapsedGraph`(부모 그래프, 속성에 GraphId, 핀은 하위 그래프 경계에서 유도) | 도메인마다 경계 노드 | 접기·펼치기(8.1절)를 공통 명령으로 하려면 경계가 공통이어야 한다. 도메인 함수 노드(머티리얼 함수 호출)는 다른 자산을 참조하므로 도메인 노드로 둔다 |

## 6. 데이터 모델

### 6.1 구조

```cpp
// 이름은 초안이다. 리플렉션 struct로 저장한다. Object 포인터·TypeLease 없음.
namespace rocks::nodegraph {

struct GraphDocument {                  // 의미 데이터
    DomainId domain;                    // 도메인 schema 식별(예: "Material")
    uint32 documentFormatVersion;       // 공통 형식 버전
    GraphId mainGraph;
    Array<Graph> graphs;                // GraphId 순 정렬(6.3절)
};

struct Graph {
    GraphId id;
    GraphRole role;                     // Main, Collapsed, DomainDefined(도메인 정의 역할 번호)
    Array<GraphNode> nodes;             // NodeId 순 정렬
    Array<GraphEdge> edges;             // 정규 순서(GI5)
};

struct GraphNode {
    NodeId id;
    NodeTypeId type;
    uint32 typeSchemaVersion;           // 저장 당시 서술 버전(GD13)
    PropertyRecord properties;          // 리플렉션 Record 바이트(GD23)
    Array<PinValueEntry> inputDefaults; // 기본값과 다른 입력 값만, PinKey 순
    Array<RetainedPin> retainedPins;    // 고아 핀(6.4절), PinKey 순
    NodeEnableState enableState;        // Enabled·Disabled(6.5절)
};

struct GraphEdge {
    NodeId fromNode; PinKey fromPin;
    NodeId toNode;   PinKey toPin;
    uint32 order;                       // ordered 출력 핀의 간선 순번, 아니면 0
};

struct GraphDocumentLayout {            // 배치 데이터(GD8), 도메인 자산이 별도 필드로 가진다
    Array<GraphLayout> graphs;          // GraphId 순
};

struct GraphLayout {
    GraphId id;
    Array<NodeLayout> nodes;            // NodeId 순: 위치(float, 캔버스 단위)·크기·접힘·고급 핀 표시·말풍선
    Array<CommentBox> comments;         // id 순: 사각형·문구·색·글자 크기
    Array<Bookmark> bookmarks;          // 북마크 이동(QuickJump) 번호 → 보기 위치
};

}
```

- `PinValueEntry`는 `{PinKey, PropertyRecord}`다. 값 struct는 도메인 타입 표가 정한다. 자산 참조는 `AssetId`로 저장한다(그래서 [에셋 시스템](AssetSystem.ko.md) 6.2절 의존 종류로 보고된다).
- 하위 그래프는 같은 문서 안에서만 참조한다. 다른 자산의 그래프(머티리얼 함수 등)는 도메인 노드 속성의 `AssetId`로 참조한다.
- **재경유**는 공통 노드 종류 `Reroute`다. 입력 하나·출력 여럿이고, 다른 노드처럼 `nodes`와 배치 항목을 가진다.
  - 추가·삭제는 문서 revision을 올린다.
  - 의미 바이트(8.4절)와 도메인 컴파일 입력에서는 재경유를 접는다. 재경유 노드를 빼고 간선을 이어 붙인다.
  - 그래서 재경유 편집은 semantic hash를 바꾸지 않고, 컴파일도 일어나지 않는다(머티리얼 시스템 5.2절과 결과가 같다).
- **메모리 표현.** 메모리에서는 `GraphMutator`가 파생 색인을 따로 유지한다. 저장하지 않는다.
  - NodeId → 위치
  - 노드별 입력·출력 간선 목록
  - 노드별 핀 목록 cache

  정렬 배열은 저장·비교·hash의 정규 형식이다.

### 6.2 노드 종류 서술

```cpp
struct PinDescriptor {
    PinKey key;                 // 안정 키(Name)
    PinDirection direction;     // Input·Output
    PinKind kind;               // Data·Flow·Structural(GD20)
    PinTypeId type;             // 도메인 타입 표 색인. wildcard면 Unresolved
    uint16 maxEdges;            // 입력 기본 1, 출력 기본 무제한(표지 상수)
    bool ordered;               // 출력 핀만: 간선 순번이 의미 있음(GD4)
    bool advanced;              // 고급 표시 묶음
    PinValuePolicy valuePolicy; // 기본값 편집 가능 여부·숨김
    Text displayName;           // 표시 이름(키와 독립)
};

struct NodeTypeDescriptor {
    NodeTypeId type;
    uint32 schemaVersion;
    Guid propertiesStruct;                           // 속성 struct ID(Record schema)
    Span<const PinDescriptor> staticPins;
    DynamicPinsFn dynamicPins;                       // (속성 Record, 문서 읽기 view) → 추가 핀(선택)
    MigrateFn migrate;                               // 옛 버전 → 현재(선택, GD13)
    PassThroughPins passThrough;                     // 비활성 때 통과할 입력 → 출력 쌍(선택, 6.5절)
    NodeMenuInfo menu;                               // 분류·키워드·도움말(UiToolkit UD14·UD16 규칙)
    NodeCapabilities capabilities;                   // 복제·삭제 가능, 문서당 하나, 비활성 허용 등
};
```

- 핀 목록 = `staticPins` + `dynamicPins(...)`. 같은 키가 두 번 나오면 등록·검증 오류다.
- `dynamicPins`는 문서 읽기 view를 받는다. `CollapsedGraph`가 하위 그래프 경계에서 핀을 유도해야 하기 때문이다(GD25). 이때 의존 관계(노드 → 참조 그래프)를 등록하고, 참조 그래프의 경계 노드가 바뀌면 그 노드의 핀 cache를 무효화한다.
- 핀 목록 cache는 노드 단위다. 속성이 바뀌거나 등록된 의존 그래프가 바뀔 때만 무효화한다.
- 표시 이름·도움말은 영어로 등록한다(런타임 메시지 규칙). 한도는 UiToolkit UD14·UD16의 상수를 쓴다.

### 6.3 정규 순서와 불변식

정규 순서는 다음과 같다.
- 그래프는 GraphId 순, 노드는 NodeId 순이다.
- 간선은 (입력 NodeId, 입력 PinKey, 출력 NodeId, 출력 PinKey) 순이다.
- 입력 기본값·고아 핀은 PinKey 순, 배치 항목은 id 순이다.

순서는 저장 바이트에 들어간다. 그래서 같은 내용은 같은 바이트가 된다. 삭제 후 undo로 되살린 노드도 원래 자리에 돌아간다. `OrderedMap`의 삽입 순서 의존(`OrderedMap.h:3–14`)을 피하려고 정렬 배열을 쓴다.

`GraphMutator`는 명령마다 **바뀐 요소에 대해서만** 불변식을 검사한다. 디버그 빌드는 명령 뒤 전체 검사를 켤 수 있다.

| ID | 불변식 |
|---|---|
| GI1 | 간선 양 끝 노드가 같은 그래프에 있다 |
| GI2 | 간선의 핀이 그 노드의 현재 핀 목록 또는 고아 핀에 있고, 방향이 출력 → 입력이다 |
| GI3 | 입력 핀의 간선 수 ≤ `maxEdges` |
| GI4 | 같은 (출력, 입력) 쌍의 간선은 하나다 |
| GI5 | 6.3절 정규 순서. `ordered` 출력 핀의 `order`는 0부터 빈틈없고, 그 밖의 간선은 `order` = 0 |
| GI6 | 배치의 노드 항목은 그 그래프의 노드와 1:1이다 |
| GI7 | 고아 핀에는 연결이나 사용자 값이 하나 이상 있다 |
| GI8 | 같은 PinKey가 현재 핀 목록과 고아 핀에 동시에 있지 않다 |
| GI9 | 문서 안 NodeId·GraphId·주석 id는 서로 겹치지 않는다 |
| GI10 | 재경유 사슬에 순환이 없다 |

순환 금지·타입 호환은 공통 불변식이 아니다. 도메인 schema의 연결 판정과 검증이 정한다. 순환을 허용하는 도메인(상태 기계 전이 등)이 있기 때문이다.

### 6.4 고아 핀

서술 변경·migration·동적 핀 감소로 핀이 사라질 때, 그 핀에 간선이나 사용자 기본값이 있으면 `RetainedPin{key, direction, 마지막 타입, 값}`으로 보존한다. UE와 같은 규칙이다(`K2Node.cpp:1456–1511`).

- 고아 핀은 연결할 수 없다. 끊기와 삭제만 된다.
- 간선·값이 모두 없어지면(GI7) 고아 핀을 지운다. 이 제거도 같은 명령의 patch op로 기록한다.
- 속성 변경으로 같은 키가 핀 목록에 다시 생기면, 방향이 같을 때 고아 핀의 간선·값을 현재 핀으로 옮기고 고아 핀을 지운다(GI8). 이것도 같은 patch에 넣는다.
- 동적 핀의 타입이 바뀌어도 기존 간선은 지우지 않는다. 타입 불일치는 도메인 검증 진단으로 보고한다. 속성 변경과 그 undo가 대칭이 되고 사용자 연결이 조용히 사라지지 않는다.
- 도메인 컴파일은 고아 핀에 닿은 간선을 오류로 본다. 화면은 고아 핀을 별도 색·문구로 표시한다.

### 6.5 활성 상태

- `Disabled`는 `capabilities`에 비활성 허용이 있는 노드 종류에서만 쓸 수 있다.
- 의미는 공통이다. 비활성 노드는 도메인 컴파일 입력에서 빠지고, `passThrough` 쌍이 있으면 그 입력의 간선이 출력으로 이어진 것으로 본다. 없으면 출력에 닿은 간선이 끊긴 것으로 본다.
- UE의 `DevelopmentOnly`(`EdGraphNode.h:174`)는 두지 않는다. Blueprint 실행에서 개발 빌드에만 켜지는 개념이다. 데이터 그래프 도메인에서 필요하면 도메인 노드 속성(예: 품질 switch)으로 표현한다.

## 7. 그래프 schema

```cpp
class GraphSchema {   // 도메인이 구현. 상태 없음, 여러 스레드에서 읽기 가능
public:
    virtual DomainId Domain() const = 0;
    virtual const PinTypeTable& PinTypes() const = 0;

    // 연결 판정. 동기·빠름(화면 hover마다 호출). 문서는 읽기 전용 view로 받는다.
    virtual ConnectVerdict CanConnect(const GraphReadView&, PinRef from, PinRef to) const = 0;

    // 노드 메뉴. 빈 곳·핀에서 끌기·선택 문맥별
    virtual void CollectMenuEntries(const MenuContext&, NodeMenuSink&) const = 0;

    // 새 그래프의 기본 노드(머티리얼 출력 노드 등)
    virtual void CreateDefaultNodes(GraphRole, GraphCommandList&) const {}

    // 붙여넣기 허용·대체(문서당 하나인 노드 등), 바깥 간선 유지 허용
    virtual PasteDecision CanPasteNode(const GraphReadView&, const GraphNode&) const;
    virtual bool AllowExternalEdgeReconnect() const { return false; }

    // 문서 검증(빠른 구조 검사). 컴파일 진단과 별개
    virtual void Validate(const GraphReadView&, DiagnosticSink&) const {}

    // 의미 정규화: 노드 하나의 의미 바이트에서 뺄 것(예: 파라미터 기본값)을 거른다(8.4절)
    virtual void NormalizeSemanticNode(const GraphNode&, SemanticWriter&) const;

    // 자산 drop → 노드 생성 명령
    virtual bool BuildDropCommands(const DropContext&, GraphCommandList&) const { return false; }
};

enum class ConnectAction : uint8 {
    Deny, Make, MakeReplacingInput, MakeReplacingOutput, MakeReplacingBoth,
    MakeWithConversion,   // conversionNodeType 사용
};

struct ConnectVerdict {
    ConnectAction action;
    NodeTypeId conversionNodeType;   // MakeWithConversion일 때
    DiagnosticCode reasonCode;       // 거부·경고 사유(영어 메시지 표의 키)
};
```

- 등록은 `RegisterGraphSchema(const GraphSchema&, ModuleLease)`로 한다. 한 `DomainId`에 schema는 하나다.
- `CanConnect`는 순환 검사를 포함할 수 있다. 공통 층은 순환 검사 도우미를 제공한다. 입력 쪽에서 출력 쪽으로 도달 가능한지를 재경유를 포함해 파생 인접 색인으로 본다. 비용은 도달 범위에 비례한다. 화면은 drag 동안 결과를 cache한다.
- UE의 promotion(`MAKE_WITH_PROMOTION`, 연산자 노드 타입 승격)은 도메인이 wildcard 해석(GD10)으로 표현한다. 별도 판정 값을 두지 않는다. 파라미터 승격("Promote to Parameter")은 메뉴 항목의 명령 목록으로 만든다.
- 메뉴 항목은 데이터다: `NodeMenuEntry{표시 이름, 분류 경로, 키워드, 도움말, 순위 가중치, 생성 명령 목록}`. 실행 함수 포인터를 두지 않는다. 항목을 고르면 그 명령 목록을 `GraphMutator`에 보낸다. 그래서 메뉴 항목도 headless로 시험한다.
- split pin(K2 전용)은 두지 않는다. 구조체 성분 분해가 필요한 도메인은 Break·Make 노드로 한다(머티리얼 Make/Break Attributes와 같다).

## 8. 변경·undo·통지

### 8.1 공통 명령

| 명령 | 효과 | 역 patch |
|---|---|---|
| `AddNode(graph, nodeId, type, properties, layout)` | 노드 추가. nodeId는 호출자가 준다(8.5절 발급기) | `RemoveNodes` + 그 노드 데이터 |
| `RemoveNodes(graph, ids)` | 노드와 닿은 간선·배치 제거 | 노드·간선·배치 복원(정규 순서라 같은 자리) |
| `Connect(edge)` / `Disconnect(edge)` | 간선 추가·제거. `Connect`는 schema 판정을 거친다. `MakeReplacing*`이면 기존 간선 제거를 같은 patch에 넣는다 | 반대 |
| `ReorderEdges(outputPin, 순서)` | `ordered` 출력 핀의 간선 순번 재배치 | 옛 순서 |
| `SetNodeProperties(node, fieldPath, value)` | 노드 속성 필드 변경(UiToolkit CU4 속성 경로, 해석은 도메인 쪽 도우미가 Record 바이트로 변환). 핀 목록이 바뀌면 6.4절 고아 처리·재결합 op를 함께 넣는다 | 옛 바이트 + 고아 처리 역 |
| `SetInputDefault(node, pin, value)` / `ResetInputDefault` | 입력 기본값 | 옛 값 |
| `SetLayout(...)` | 위치·크기·접힘·고급 핀 표시·말풍선 | 옛 값 |
| `AddComment`·`RemoveComment`·`SetComment` | 주석 상자 | 반대 |
| `SetBookmark` | 북마크 | 옛 값 |
| `SetEnableState(node, state)` | 노드 활성 전환(6.5절) | 옛 값 |
| `AddGraph`·`RemoveGraph`·`RenameGraph` | 하위 그래프 | 반대 |
| `MoveNodesToGraph(ids, target)` | 노드와 그 사이 간선을 다른 그래프로 옮김(접기·펼치기의 기본 연산) | 반대 |
| `MigrateNode(node, 새 버전 데이터)` | migration 결과 적용(GD13) | 옛 노드 데이터 |
| `RemoveRetainedPin(node, key)` | 고아 핀 제거(간선 포함) | 복원 |

도메인 명령과 화면 명령은 이 명령들의 목록이다.

- 변환 노드를 끼워 연결: `AddNode`, `Connect`(앞), `Connect`(뒤)
- 재경유 삽입: `AddNode`, `Disconnect`(원 간선), `Connect`(앞), `Connect`(뒤)
- 삭제 후 다시 잇기(UE `DeleteAndReconnectNodes`): 통과할 간선 `Connect` 목록과 `RemoveNodes`
- 접기: `AddGraph`, 하위 그래프의 `GraphInput`·`GraphOutput` `AddNode`, `MoveNodesToGraph`, 부모의 `CollapsedGraph` `AddNode`, 경계 간선 `Disconnect`·`Connect`들

### 8.2 적용 절차

1. 화면·스크립트가 EditorBackend 명령으로 `(AssetId, expectedRevision, commandList)`를 보낸다(계획 22.1절). 문서 소유 lane(GD24)에서 처리한다.
2. `GraphMutator`가 문서 revision을 확인한다. 다르면 `Conflict`다.
3. 명령마다 검증한다: 불변식 GI1–GI10(바뀐 요소), schema 판정, 서술의 허용(삭제 가능 등). 하나라도 실패하면 전체를 적용하지 않고 `ValidationError`를 돌려준다. 문서는 바뀌지 않는다.
4. 검증 단계에서 정·역 op와 배열 변경에 필요한 용량을 모두 예약한다. 할당 실패는 여기서 `Result` 오류로 끝나고, 문서는 바뀌지 않는다.
5. 적용한다. 예약한 용량 안에서 할당 없이 진행한다. 정·역 op를 `GraphPatch`에 모으고 문서 revision을 올린다.
6. Transaction에 patch를 넣는다. bracket 안이면 bracket이 닫힐 때 합친다.
7. 통지 묶음 하나를 보낸다.

**bracket.**
- `BeginInteraction(AssetId)` → 명령 여러 개 → `EndInteraction`(확정) 또는 `CancelInteraction`(역 patch로 복원)이다. node drag·핀 기본값 slider가 쓴다.
- 문서마다 bracket은 하나만 열린다. 중첩은 오류다.
- bracket이 열려 있는 동안 다른 출처(RPC·스크립트·다른 화면)의 명령은 `Busy`로 거부한다. 기다리지 않는다. 그래서 Cancel의 역 patch는 늘 유효하다.
- bracket 안의 patch는 합친다. 같은 필드를 여러 번 바꾸면 첫 옛 값과 마지막 새 값만 남는다.
- 통지는 즉시 보내되 `interactive` 표지를 단다.
  - 컴파일 coordinator는 `interactive` 통지로 재컴파일을 시작하지 않고, 확정 통지를 기다린다(계획 14.8절 debounce).
  - 재컴파일 없이 반영되는 값(머티리얼 dynamic parameter, 계획 14.7절)은 도메인이 `interactive` 통지로도 미리보기에 바로 반영한다. 어느 필드가 그런 값인지는 도메인이 정한다.
- UiToolkit 6절의 slider drag 규칙과 같다.

### 8.3 통지 종류

| 통지 | 내용 |
|---|---|
| `NodeAdded`·`NodeRemoved` | 그래프·NodeId |
| `NodePropertiesChanged` | NodeId·바뀐 필드 경로 |
| `NodePinsChanged` | NodeId(핀 목록 재계산 결과가 달라짐) |
| `InputDefaultChanged` | NodeId·PinKey |
| `EdgeAdded`·`EdgeRemoved` | 간선 |
| `EdgeOrderChanged` | 출력 핀 |
| `NodeLayoutChanged` | NodeId 목록 |
| `CommentChanged`·`BookmarkChanged` | id |
| `NodeEnableStateChanged` | NodeId |
| `GraphAdded`·`GraphRemoved`·`GraphRenamed` | GraphId |
| `RetainedPinChanged` | NodeId·PinKey |
| `NodeTypesChanged` | 등록·해제로 해석이 바뀐 NodeTypeId 목록(문서 바이트·revision 불변, 10절) |
| `DocumentReloaded` | 문서 전체 교체(파일 다시 읽기 등) |

묶음에는 `{AssetId, 새 문서 revision, semanticTouched, 원인(Edit·Undo·Redo·Migration·Reload·Registry), interactive}`가 붙는다.

- `semanticTouched`는 이 묶음이 의미 데이터(`GraphDocument`)를 건드렸는지 나타낸다. 재경유만 바뀐 경우도 참이다.
- 구독자의 반응은 다음과 같다.
  - 화면: 증분 갱신
  - 도메인 compile coordinator: `semanticTouched`일 때 semantic hash를 다시 계산하고, 바뀌었을 때만 컴파일
  - 찾기 색인, 진단 패널

### 8.4 의미 바이트와 hash

공통 층은 정규 의미 바이트를 만드는 함수를 제공한다. 도메인 semantic hash(예: 머티리얼 시스템 5.5절)는 이 바이트를 SHA-256한 값이다.

1. 재경유를 접는다. 재경유 노드를 빼고 간선을 이어 붙인다(GI10).
2. 남은 노드를 NodeId 순으로 쓴다. 노드마다 `GraphSchema::NormalizeSemanticNode`를 부른다.
   - 기본 구현은 (type, schemaVersion, 속성 Record 바이트, 입력 기본값, 활성 상태, 고아 핀)을 쓴다.
   - 도메인은 빼야 할 필드(예: 머티리얼의 scalar·vector·texture 파라미터 기본값)를 걸러 쓴다.
3. 접은 간선을 정규 순서로 쓴다. 비활성 노드의 통과 처리(6.5절)는 도메인 컴파일 입력에서 하고, 의미 바이트에는 활성 상태만 넣는다.

hash는 **migration을 거친 문서**로 계산한다. 쿠커는 저장 바이트를 읽은 뒤 메모리에서 같은 migration을 적용하고 hash를 계산한다. 그래서 에디터에서 연 문서와 쿠커의 너겟 키가 같다.

### 8.5 Guid 발급

`NodeId`·`GraphId`·주석 id는 `GraphIdSource`에서 받는다.
- 에디터는 `TryNewGuid`(무작위 UUID v4)를 쓴다.
- 시험·template·결정적 import는 seed 발급기를 주입한다. seed 발급기의 id는 (seed, 대상 AssetId, 적용 순번)에서 유도한다. 같은 template을 같은 문서에 두 번 적용해도 겹치지 않는다.
- 그래도 겹치면 GI9 검증이 `AddNode`를 거부한다.
- 같은 입력의 붙여넣기·template 적용이 같은 id를 내야 golden 시험이 성립한다. UE MetaSound `FScopeDeterminism`과 같은 목적이다.

## 9. 복사·붙여넣기·직렬화

### 9.1 저장

- 도메인 자산은 `GraphDocument`(의미)와 `GraphDocumentLayout`(배치)을 별도 리플렉션 필드로 가진다. EditorOnly 표시는 필드 단위 정적 표지다. 그래서 배치를 쿠킹에서 빼려는 도메인은 배치 필드에 EditorOnly를 붙인다. 머티리얼은 두 필드 모두 EditorOnly다(머티리얼 시스템 2절 대응표).
- 속성은 Record 바이트(GD23)다. 모르는 노드도 바이트 그대로 저장한다.
- `GraphMutator`는 두 struct를 함께 다루고 revision 하나로 묶는다.

### 9.2 그래프 조각

```json
{
  "format": "rocks.graph-fragment",
  "formatVersion": 1,
  "domain": "Material",
  "sourceAsset": "<AssetId, 선택>",
  "nodes": [
    {
      "id": "<NodeId>",
      "type": "Material.Multiply",
      "typeVersion": 2,
      "properties": { "<리플렉션 텍스트 형식>": "..." },
      "inputDefaults": { "B": { "value": 0.5 } },
      "layout": { "x": 120, "y": -40 }
    }
  ],
  "edges": [ { "from": ["<NodeId>", "Result"], "to": ["<NodeId>", "A"], "order": 0 } ],
  "comments": [],
  "externalEdges": []
}
```

- 쓰기는 `JsonWriter` Canonical이라 같은 조각은 같은 바이트다. 읽기는 Json5 문법도 받는다(사람이 고친 조각).
- 속성은 등록된 타입이면 리플렉션 텍스트 형식(`ReflectionText`)으로 쓴다. 모르는 타입이면 `"propertiesRecord": "<base64 Record 바이트>"`로 쓴다. 그래서 모르는 노드도 같은 문서 사이 복사에서 바이트가 보존된다.
- 붙여넣기 처리는 다음과 같다.
  1. 도메인이 다르면 전체를 거부한다.
  2. NodeId·주석 id를 새로 발급하고 조각 안 간선을 다시 잇는다.
  3. 붙이는 위치를 기준으로 배치를 옮긴다.
  4. `properties` 텍스트인데 지금 타입이 등록되어 있지 않은 노드는 바이트로 바꿀 수 없다. 그 노드와 닿은 간선을 빼고, 진단으로 보고한다. 나머지는 붙인다.
  5. `propertiesRecord` 노드는 자리 노드로 붙인다.
  6. migration이 필요한 노드는 migration 명령을 함께 넣는다.
  7. 결과 명령 목록 하나를 `GraphMutator`에 보낸다. undo 1개다.
- 바깥 간선은 같은 문서이고 대상 노드가 아직 있으며 `AllowExternalEdgeReconnect()`가 참일 때만 다시 잇는다. 다시 이을 때도 schema 판정을 거친다. 기본값은 끊기로, UE와 같다(`EdGraphUtilities.cpp:46–102`).
- 같은 형식을 template 조각(계획 22.3절)과 commandlet dump에도 쓴다.
- 조각 codec은 리플렉션 타입 조회가 필요하므로 NodeGraphTools(Developer)에 있다(11.1절).

### 9.3 텍스트 dump

`graph-dump`는 문서 전체를 9.2절 형식(의미 + 배치)으로 출력한다. `--semantic-only`는 배치를 뺀다. golden 시험과 source control diff 보기(계획 14.5절 "source control diff")에 쓴다.

## 10. 노드 등록·버전

**등록.** `RegisterNodeType(DomainId, const NodeTypeDescriptor&, ModuleLease)`. 등록 때 다음을 검사한다.
- 핀 키 중복
- 타입 색인 범위
- 속성 struct ID가 리플렉션에 등록되어 있는지(도메인 쪽 등록 도우미가 확인)
- migration 함수가 현재 버전까지 이어지는지

**열 때 migration.**
- 노드의 `typeSchemaVersion`이 현재보다 낮으면 `migrate`를 버전마다 차례로 적용해 `MigrateNode` 명령 목록을 만든다.
- 이 목록은 문서 열기 단계에서 적용한다. undo 이력에 넣지 않으며, 열기 직후의 문서가 undo 이력의 시작점이다.
- 결과는 진단 정보 수준으로 보고한다(머티리얼 시스템 E13).
- migration이 핀 키를 바꾸면 간선·기본값을 새 키로 옮긴다. 옮길 곳이 없으면 고아 핀으로 보존한다.
- 저장하지 않고 닫으면 파일은 옛 버전 그대로다. 쿠커는 8.4절대로 같은 migration을 메모리에서 거친다.

**노드 교체.** migration은 다른 `NodeTypeId`를 돌려줄 수 있다(1:1). 여러 노드 → 하나(MetaSound `INodeUpdateTransform`)는 NG8에서 정한다.

**해제.** 문서와 patch는 속성을 바이트로 가지므로(GD23) 노드 종류 lease를 쥐지 않는다. `Unregister` 순서는 다음과 같다.
1. 메뉴와 새 해석 요청에서 그 종류를 뺀다.
2. 진행 중인 해석·컴파일 작업을 drain한다.
3. lease를 놓는다.
4. `NodeTypesChanged`를 보낸다. 열린 문서의 그 종류 노드는 자리 노드로 표시된다. 문서 바이트와 revision은 그대로다.

다시 등록되면 같은 통지로 원래 표시로 돌아간다.

## 11. 모듈과 화면

### 11.1 모듈

| 모듈 | 계층 | 책임 | 의존 | 금지 |
|---|---|---|---|---|
| **NodeGraph** | Engine Runtime(`Engine/Source/Runtime/NodeGraph`) | 문서·배치 타입, 서술·schema 등록표, `GraphMutator`·patch·통지·bracket, 파생 색인, 불변식 검사, 의미 바이트, migration 실행, 순환 검사 도우미, 진단 형식, Guid 발급기 | Core·MetaSchema·Serialization | ObjectRuntime·UI·장치·도메인 모듈 |
| **NodeGraphTools** | Engine Developer(`Engine/Source/Developer/NodeGraphTools`) | 조각 JSON codec(리플렉션 텍스트 변환), `graph-dump`·`graph-validate`·`graph-diff` commandlet, 문서 diff 계산 | NodeGraph·ObjectRuntime·ReflectionText·Core | 도메인 모듈, Release 링크 |
| **GraphView** | Engine Editor(`Engine/Source/Editor/GraphView`) | 11.2절 화면 | UiFramework·EditorFramework·NodeGraph·NodeGraphTools·SpatialIndex(2D 격자, GD16) | 도메인 모듈(GD17), Game·Client·Server 도달(BuildTargets BD20) |
| 도메인 schema | 각 도메인(예: MaterialSchema) | `GraphSchema` 구현·서술 등록 | NodeGraph | — |
| 도메인 편집기 | 각 도메인 Editor 모듈(예: EditorMaterial) | `GraphView` 배치·도메인 시각 등록·overlay·명령 결합 | GraphView·도메인 | 화면에서 의미·검증 재구현(계획 14.6절) |

NodeGraph는 Runtime 계층이다. 그러나 게임이 쓰지 않으면 Release 링크에 들어가지 않는다. 쿠킹된 게임은 도메인의 컴파일 결과만 읽는다. 런타임 그래프 생성(NG8)을 켜는 도메인만 링크한다.

### 11.2 GraphView 구성

| 구성 | 책임 | UE 대응 |
|---|---|---|
| `GraphView` | 공개 Element: 문서·그래프 지정, 명령 목록 결합, 이벤트(선택·더블클릭·메뉴 요청), 하위 그래프 열기·breadcrumb | `SGraphEditor`·`FGraphEditorEvents` |
| `GraphCanvas` | pan·zoom(UE와 같은 20단계 0.1–2.0, 기본 1:1, 20단계를 LOD 5등급에 대응)·격자·격자 맞춤·가장자리 자동 pan·전체 보기와 선택 맞춤 zoom·marquee(바꾸기·더하기·빼기·반전)·선택 집합(NodeId) | `SNodePanel` |
| 노드 시각 | 제목·핀 줄·기본값 편집기·접힘·고급 핀·연결 없는 핀 숨기기·오류 표지·말풍선. 기본 시각 하나 + 도메인 등록 시각 | `SGraphNode`·`SGraphNodeDefault`·`SCommentBubble` |
| 핀 시각·기본값 편집기 | `PinTypeTable`의 값 종류로 고르는 타입별 기본값 편집기(bool·정수·실수·벡터·색·enum·자산 선택·문자열), 기본값 복원 | `SGraphPin*`·`CreateK2PinWidget` |
| `GraphVisualRegistry` | 도메인이 노드 종류·핀 타입별 시각·색·연결선 모양을 등록. 조회 순서: 노드 종류 → 도메인 기본 → 공통 기본 | `FNodeFactory`·`FGraphPanel*Factory` |
| `WirePainter` | cubic 곡선(UE 접선 설정값 대조), hover(bbox 선검사 + 표본 거리, 2D 격자로 후보 축소), 자르기 선 교차, 흐름 핀 강조. 화면 수명 동안 하나(매 프레임 생성 안 함) | `FConnectionDrawingPolicy`·`FGraphSplineOverlapResult` |
| 연결 drag | hover마다 `CanConnect`(drag 동안 cache), drop 때 명령 목록, 빈 곳 drop은 노드 메뉴(핀 문맥), Ctrl drag로 연결 옮기기, 핀 연결 모두 끊기 | `FDragConnection` |
| 노드 drag | bracket 안 `SetLayout`, 주석 상자 소속은 drag 시작 때 계산(GD18) | `FDragNode` |
| 노드 메뉴·palette | 등록표·schema 메뉴 항목 검색(토큰·분류·가중치), 키보드 탐색, palette drag | `SGraphActionMenu`·`SGraphPalette` |
| 편집 명령 결합 | 복사·잘라내기·붙여넣기·복제·삭제·삭제 후 다시 잇기·undo·redo·전체 선택·상류/하류 선택·활성 전환 | host 편집기 결합 + `GraphEditorActions` |
| 주석·재경유 | 주석 상자 생성·크기·색·글자 크기, 연결선 더블클릭 재경유 삽입 | `SGraphNodeComment`·`SGraphNodeKnot` |
| 정렬 명령 | 왼쪽·오른쪽·위·아래·가운데 정렬, 가로·세로 분배, 쌓기, 연결 곧게 펴기 | `FGraphEditorCommands` 정렬 묶음 |
| 북마크 이동 | 번호 북마크 저장·이동(Ctrl+숫자) | QuickJump |
| 찾기 | 노드 제목·속성 문자열·핀 이름·주석 검색, 결과로 이동. 문서 revision 단위 색인 | `SFindInGraph` |
| diff | 두 문서 revision을 NodeId로 맞춰 추가·삭제·속성·간선·배치 차이 표시(계산은 NodeGraphTools) | `FGraphDiffControl` |
| overlay hook | 도메인이 노드 badge·핀 값 tooltip·연결선 강조(실행 중 값·미리보기 썸네일)를 제공 | `GetOverlayBrushes`·`GetNodeInfoPopups` |
| 읽기 전용 미리보기 | tooltip·찾기 결과용 작은 그래프 | `SGraphPreviewer` |

**UE에서 가져오지 않는 것.**
- Kismet 전용 위젯·breakpoint·pin watch. 값 관찰이 필요한 도메인은 overlay hook으로 구현한다.
- split pin(7절)
- 노드 맞붙이기(UE에서 꺼져 있음)
- `DEPRECATED_NodeWidget` 역참조

미니맵은 UE에도 없다. 요구가 생기면 이 문서 개정으로 추가한다.

### 11.3 성능 목표

수치는 NG6 측정 뒤 확정한다. 측정 항목과 판정 방식은 다음과 같다.

**화면.**
- 노드 1,000·5,000·20,000개 문서에서 다음을 잰다.
  - pan·zoom 프레임 시간
  - hover 판정 시간, marquee 시간
  - 노드 하나 속성 변경의 화면 갱신 시간
- 노드 하나를 바꿀 때의 갱신 비용이 문서 크기에 비례하지 않음을 확인한다(GD15).

**모델.**
- `GraphMutator` 명령의 검증·통지 비용과 undo·redo 비용은 파생 색인 덕에 바뀐 노드·간선 수에 비례한다. 문서 크기에 비례하지 않음을 시험으로 확인한다.
- 정렬 배열 삽입·삭제의 이동 비용(문서 크기에 선형)은 따로 잰다.
- 20,000개 문서에서 명령 하나의 이동 비용이 프레임 예산을 넘으면, 메모리 표현을 색인 기반 Map으로 바꾸고 정렬은 저장·hash 때만 한다. 이 변경은 저장 형식을 바꾸지 않는다.

## 12. 도메인 적용

| 도메인 | 문서·단계 | 적용 |
|---|---|---|
| 머티리얼 | [머티리얼 시스템](MaterialSystem.ko.md) MS1·14절, 계획 14.5–14.9절 | `MaterialGraph`는 `GraphDocument`(domain `Material`)다. 5.2절 노드·핀·간선 정의는 이 문서 6절과 같다. `MaterialNodeDescriptor`는 `NodeTypeDescriptor` + 머티리얼 확장이다(단계·capability, lowering은 MaterialCompiler 쪽 등록). 대응: E1 → 7절 메뉴, E2 → 8절, E3 → `CanConnect`, E4 → GD19, E13 → GD14·10절. 출력 노드는 `CreateDefaultNodes` + 문서당 하나 capability. 파라미터 기본값 제외는 `NormalizeSemanticNode` |
| 오디오 | [오디오 시스템](AudioSystem.ko.md) AU10 `SoundEvent`, M7 `SoundGraph` | `SoundEvent`(랜덤·레이어·switch·지연·loop): 출력 하나로 모이는 트리. 자식 목록은 동적 입력 핀(속성의 입력 수)으로 둔다(GD4). `SoundGraph`(MetaSounds형): 데이터 핀 + 실행 trigger 핀(`Flow`). 블록 크기·실시간 안전성 검증은 도메인 몫. 런타임 그래프 생성이 필요하면 NG8 |
| 애니메이션 | 계획 15절(1522행) blend tree·state machine·graph compiler | blend tree는 데이터 그래프. state machine은 `Structural` 핀(전이)과 하위 그래프(상태마다). cook 때 cycle·타입·누락 핀 검증 → task plan |
| VFX | 계획 21절 | 모듈 스택·데이터 그래프. 컴파일은 도메인 |
| PCG | 계획 24.3절 | 데이터 그래프, 타입 변환 노드 자동 삽입(`MakeWithConversion`) |
| AI | 계획 24.1절 StateTree 계열 | 필요하면 `Structural` 핀과 `ordered` 출력 간선으로 표현. BT형 좌표 순서는 쓰지 않는다(GD4) |
| Sparks 시각 편집 | [레벨 스크립트](LevelScript.ko.md) LD2(보류) | 만들게 되면 이 모델 + Sparks 코드 생성기. 실행은 Sparks. 공통 등록표에 gameplay 노드를 두지 않는다(GD20) |

## 13. 검증

에셋 시스템·Sparks 구현 시험과 같은 철저함을 요구한다. 규칙마다 양성·음성 사례를 둔다. undo 일치 판정은 정규 저장 바이트로 한다.

| ID | 시험 | 판정 |
|---|---|---|
| GT1 | 명령별 양성·음성 | 8.1절 명령마다 성공 사례와 불변식 GI1–GI10·schema 판정 위반 사례. 위반이면 문서 바이트 불변 |
| GT2 | undo·redo 왕복 | 명령마다 `적용 → undo`가 원 문서 바이트와 같고, `undo → redo`가 적용 결과와 같다. 통지 순서도 golden과 대조 |
| GT3 | 무작위 명령열 | seed 무작위 명령열(단계 수는 이름 있는 상수, 1만 이상). 매 단계 전체 불변식 검사, 임의 지점까지 undo 후 redo 결과 일치. 실패 시 seed와 최소화한 명령열 출력 |
| GT4 | bracket | 확정·취소·중첩 거부·bracket 중 다른 출처 `Busy`·같은 필드 반복 병합. 취소 후 바이트 불변 |
| GT5 | 조각 왕복 | 복사 → 붙여넣기의 id 재발급·간선 재연결·바깥 간선 정책. 같은 seed면 같은 바이트. 다른 도메인 거부. `propertiesRecord` 노드 바이트 보존. 미등록 텍스트 노드 제외와 진단 |
| GT6 | 실패 주입 | 할당 실패를 검증·예약 단계 각 지점에 주입. 문서 불변·`Result` 오류·누수 0. 적용 단계에서 할당 0 확인 |
| GT7 | migration | 버전 사슬, 키 재지정, 고아 핀 생성·재결합, 1:1 교체, 모르는 노드 왕복 바이트 불변, 에디터와 쿠커의 hash 일치 |
| GT8 | 등록·해제 | lease 해제 drain, 해제 뒤 문서 바이트·revision 불변과 자리 표시, 재등록 복원 |
| GT9 | 의미·배치 분리 | 배치 명령과 재경유 편집이 의미 바이트를 바꾸지 않음. `semanticTouched` 표지가 정확함. 도메인 정규화(기본값 제외)가 적용됨 |
| GT10 | golden dump | 표본 문서의 `graph-dump` 출력 golden |
| GT11 | 화면 CPU 시험 | 가짜 플랫폼(UiToolkit UT1)에서 hover·marquee·연결 drag·노드 drag·메뉴 검색·편집 명령 결합의 명령 결과, 통지 → 증분 갱신 대상 수 |
| GT12 | 화면 픽셀 | UiToolkit UV6 방식 픽셀 비교(노드·핀·연결선·LOD 단계) |
| GT13 | 성능 | 11.3절 측정, 노드 하나 변경의 갱신 비용이 크기와 무관함 |
| GT14 | 정책 | NodeGraph의 ObjectRuntime·도메인 의존 0건, NodeGraphTools·GraphView의 도메인 의존 0건, GraphView의 Game·Client·Server 도달 0건(BuildTargets BD20) |

## 14. 단계

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| NG0 | 문서·배치 타입, 서술·등록표, 정규 순서, 불변식 검사, 의미 바이트 | MetaSchema·Serialization(구현됨) | GT1 중 구조 사례, GT9 바이트 부분, GT14 의존 |
| NG1 | `GraphMutator`·공통 명령·patch·파생 색인·통지·bracket·Guid 발급기 | NG0 | GT1–GT4, GT6 |
| NG2 | NodeGraphTools: 조각 JSON codec·붙여넣기·`graph-dump`·`graph-validate`·`graph-diff` | NG1, ReflectionText(구현됨) | GT5, GT10 |
| NG3 | migration·고아 핀·자리 노드·등록 해제·하위 그래프 경계 노드 | NG1 | GT7·GT8, 접기·펼치기 왕복 |
| NG4 | EditorBackend Transaction·명령 schema 연결(계획 22.1절 RPC 포함), lane 소유 | NG1, EditorBackend Transaction(UT4 중 headless 부분) | Transaction 왕복·expected revision 충돌·`Busy` 시험 |
| NG5 | `GraphCanvas`·기본 노드·핀 시각·기본값 편집기·연결선·선택·drag·연결 drag, 최소 노드 메뉴(검색 목록), 편집 명령 결합(복사·붙여넣기·복제·삭제·undo·redo) | NG4, NG2, UiToolkit UT3 | GT11 기본, GT12 |
| NG6 | palette·주석·재경유·정렬·북마크·찾기·상하류 선택·2D 격자·성능 측정 | NG5, 공용 2D 격자(GR5) | GT11 전체, GT13 측정과 목표 확정 |
| NG7 | diff 화면·overlay hook·하위 그래프 이동·접기·펼치기 화면 | NG6, NG3 | diff·접기 화면 시험, 머티리얼 MAT M4 "정상 제작 Gate" 그래프 부분 |
| NG8 | 후속: 여러 노드 → 하나 migration, wildcard 해석 도우미, 런타임 그래프 생성(도메인 요구 시), 3-way merge(계획 22.4절 Linked 갱신) | 요구 시 | 각 항목 착수 때 이 문서 개정 |

머티리얼 쪽 연결은 다음과 같다(계획 14.5절 표의 그래프 편집 행).
- MS1은 NG0–NG3을 쓴다.
- 머티리얼 에디터 M3 "최소 편집"은 NG4–NG5를 쓴다. 노드 추가(최소 메뉴)·연결·삭제·복사·붙여넣기·undo가 NG5에 들어 있다.
- M4 "정상 제작 Gate"는 NG6–NG7을 쓴다.

## 15. 다른 문서 개정 요청

GR1·GR2·GR3·GR5는 개정 3에서 반영했다. GR4는 오디오 문서를 다른 작업이 고치는 중이라 그 뒤에 반영한다.

| ID | 문서 | 개정 내용 | 상태 |
|---|---|---|---|
| GR1 | [UI 툴킷](UiToolkit.ko.md) 2·6·9절 | "그래프 편집" 행을 `GraphView` 모듈(Editor 계층, 이 문서 11절)로 바꾸고, EditorFramework 책임에서 그래프 Element를 뺀다. UT6의 "그래프 Element"를 NG5–NG7로 잇는다 | 반영(UiToolkit 초안 2.4) |
| GR2 | [머티리얼 시스템](MaterialSystem.ko.md) 3.2·5.2·5.5·14·15절 | 3.2절: MaterialSchema가 NodeGraph에 의존한다. 5.2절: 형식은 이 문서 6절을 참조하고, "재경유·주석은 화면 노드"를 "재경유는 의미 비교 때 접히는 공통 노드, 주석은 배치 데이터"로 고친다. 5.5절: semantic hash는 이 문서 8.4절 의미 바이트 + `NormalizeSemanticNode`(파라미터 기본값 제외)로 정의하고, 입력 기본값·활성 상태·고아 핀 포함을 명시하며, migration 뒤 계산임을 적는다. 14절: E1–E4·E13의 제공 모듈에 NodeGraph를 더한다. 15.1절: MS1 선행의 "리플렉션 B9(`InstancedStruct`) 노드 속성 저장"을 NG0–NG3(Record 바이트, GD23)으로 바꾼다 | 반영(MaterialSystem 개정 7) |
| GR3 | [엔진 구현 계획](EngineImplementationPlan.ko.md) 14.6·22.2절 | 14.6절 MaterialSchema 행과 그래프 canvas 재사용 문장에 NodeGraph·GraphView를 적는다. 22.2절에 "노드 그래프 편집 patch는 노드 그래프 설계 8절"을 더한다(22.2절의 graph patch는 객체 계층용이므로 구분) | 반영(계획 개정 1.18) |
| GR4 | [오디오 시스템](AudioSystem.ko.md) AU10 | `SoundEvent`·`SoundGraph`가 NodeGraph 위에 선다고 적는다 | 미반영 |
| GR5 | [공간 색인](SpatialIndex.ko.md) | 2D 균일 격자 후속을 공용 구현으로 정하고, 그래프 화면 요구(GD16: 캔버스 좌표 bbox 질의·갱신 빈도)를 적는다. NG6 선행이다 | 반영(SpatialIndex 개정 8, 5.3절) |

## 16. 위험

| ID | 위험 | 대응 |
|---|---|---|
| GK1 | 공통 층이 도메인 요구에 끌려 커진다(RigVM controller 25,645줄) | 공통 명령을 고정 집합으로 두고 도메인 명령은 조합으로만 만든다(GD5). 새 공통 명령은 이 문서 개정으로만 추가한다 |
| GK2 | 화면 규모(UE 공통 부분 약 29,000줄)가 커서 에디터 일정의 임계 경로가 된다 | NG5에서 머티리얼 M3 최소 편집 범위만 먼저 하고, NG6–NG7은 M4 Gate에 맞춘다 |
| GK3 | 서술에서 유도하는 핀의 동적 계산 비용이 반복된다 | 노드 단위 cache와 무효화 조건(6.2절). GT13에서 측정한다 |
| GK4 | patch 기반 undo의 역 op 누락 | GT2·GT3 무작위 왕복, 명령마다 역 op 표(8.1절) 대조 |
| GK5 | 속성을 바이트로 보관하면 편집 때마다 decode·encode 비용이 든다 | 속성 편집은 노드 하나 단위다. 도메인 쪽 도우미가 열린 노드의 decode 결과를 문서 revision 기준으로 cache한다. GT13에서 측정한다 |
| GK6 | 공통 층이 gameplay 비주얼 스크립팅으로 확대된다 | GD20, 도메인 단위 등록표, gameplay 노드 금지를 리뷰 점검 항목으로 둔다 |
| GK7 | 정렬 배열의 삽입 이동 비용이 큰 문서에서 커진다 | 11.3절 측정 기준과 메모리 표현 교체 경로(저장 형식 불변) |

## 17. 개정 기록

- 개정 1(2026-11-22): 초판. 사용자 요청("언리얼의 노드그래프 시스템을 분석하고 rocks에 도입하기 위한 계획")에 따라 작성했다. UE 5.8.4 조사는 세 갈래로 했다.
  - EdGraph 데이터·GraphEditor 화면
  - 도메인 사용례: 머티리얼·Sound Cue·애니메이션 그래프·Behavior Tree·Niagara·PCG·Dataflow
  - 모델 우선 구조: RigVM·MetaSound Frontend·실험 플러그인

  결정 GD1–GD22는 모두 권장안이다.
- 개정 2(2026-11-22): 교차 검증 1회 반영. UE 인용 약 50개를 대조한 결과, 줄 범위·수치는 맞았다. 반영 내용은 다음과 같다.
  - **필수 9.**
    - 모델 모듈의 ObjectRuntime 의존 제거: 속성을 Record 바이트로 보관(GD23), NodeGraphTools 분리(GD21)
    - 모르는 노드 보존을 리플렉션 기능 없이 성립시키고, 해제 때 lease 문제 해소(GD14·10절)
    - `OrderedMap` 삽입 순서 대신 정렬 배열 정규 순서(6.3절)
    - 재경유 규정 통일(6.1·8.4절, GR2)
    - revision 하나 + semantic hash 판정(GD8)
    - 의미 바이트의 도메인 정규화 hook과 migration 뒤 hash(8.4절)
    - 간선 순번은 출력 핀 전용, `ReorderEdges` 추가, 조각에 order(GD4·8.1·9.2절)
    - JSON 쓰기는 Canonical 엄격 JSON(GD12·9.2절)
    - 최소 메뉴·편집 명령 결합을 NG5로 이동(14절)
  - **권장 14.**
    - bracket 중 다른 출처 `Busy`, interactive 미리보기(8.2절)
    - 고아 핀 재결합·제거 기록·타입 변경 처리(6.4절, GI8)
    - 해제 통지(10절), 검증 단계 용량 예약(8.2절)
    - 파생 색인과 지역 검사(6.1·6.3·11.3절)
    - UiCore hit grid 재사용과 공용 2D 격자(GD16·GR5)
    - `PinKey` = Name(GD2), 활성 상태 의미와 `DevelopmentOnly` 제외(6.5절)
    - 하위 그래프 경계 노드(GD25), 스레드 소유(GD24)
    - 계획 22.2절 인용 정정과 undo budget(GD6·1절·GR3)
    - 배치 struct 분리로 EditorOnly(9.1절)
    - seed 발급기 충돌 방지(8.5절·GI9)
    - UE 공통 명령 추가: 북마크·상하류 선택·연결 없는 핀 숨기기·삭제 후 다시 잇기·핀 연결 끊기·기본값 복원·복제·맞춤 zoom(11.2절)
  - **참고 11.**
    - `ImportNodesFromText` 파일 수, MetaSound Tick 호출 경로, `INodeUpdateTransform` 실험 버전
    - LOD 표현, 머티리얼 PostUndo 인용, Core 경로
    - 8.1절 명령 나열, 진단의 AssetId, `rocks::nodegraph` 이름공간
    - B9 구현 완료 표기(1절), Runtime 배치 근거(GD21)

  리플렉션 개정 요청(개정 1의 GR3)은 GD23으로 필요 없어져 지우고, 요청 번호를 GR1–GR5로 다시 매겼다.
- 개정 3(2026-11-22): 다른 문서 개정 요청 GR1·GR2·GR3·GR5를 해당 문서에 반영하고 15절에 상태 열을 더했다. GR4(오디오)는 미반영이다. GD16의 2D 격자 참조를 공간 색인 5.3절로 고쳤다.
