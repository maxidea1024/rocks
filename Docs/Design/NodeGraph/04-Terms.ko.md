# 노드 그래프(NodeGraph) 설계

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
