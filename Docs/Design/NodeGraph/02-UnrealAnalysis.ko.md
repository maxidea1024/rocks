# 노드 그래프(NodeGraph) 설계

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
