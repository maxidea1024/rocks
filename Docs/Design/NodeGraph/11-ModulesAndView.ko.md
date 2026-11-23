# 노드 그래프(NodeGraph) 설계

## 11. 모듈과 화면

### 11.1 모듈

| 모듈 | 계층 | 책임 | 의존 | 금지 |
|---|---|---|---|---|
| **NodeGraph** | Engine Runtime(`Engine/Source/Runtime/NodeGraph`) | 문서·배치 타입, 서술·schema 등록표, `GraphMutator`·patch·통지·bracket, 파생 색인, 불변식 검사, 의미 바이트, migration 실행, 순환 검사 도우미, 진단 형식, Guid 발급기 | ObjectRuntime·CoreTypeBindings(Core·MetaSchema·Serialization 포함) | 객체·`ObjectHandle`·`TypeLease`·`InstancedStruct`·객체 Registry 사용, UI·장치·도메인 모듈 |
| **NodeGraphTools** | Engine Developer(`Engine/Source/Developer/NodeGraphTools`) | 조각 JSON codec(리플렉션 텍스트 변환), `graph-dump`·`graph-validate`·`graph-diff` commandlet, 문서 diff 계산 | NodeGraph(공개), ObjectSerialization·ReflectionText(비공개 — 9.4절) | 도메인 모듈, Release 링크 |
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
