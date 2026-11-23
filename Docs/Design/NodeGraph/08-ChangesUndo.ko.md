# 노드 그래프(NodeGraph) 설계

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
2. `GraphMutator`가 문서 revision을 확인한다. 다르면 `StaleHandle`이다(Core에 충돌 전용 코드가 없어 "기대한 revision이 낡았다"로 쓴다).
3. 명령을 차례로 적용한다. 명령마다 지금 문서(앞 명령이 적용된 상태)에 대해 검증한다: 불변식 GI1–GI14(바뀐 요소), schema 판정, 서술의 허용(삭제 가능 등). 같은 목록의 앞 명령이 더한 노드에 뒤 명령이 연결할 수 있다.
4. 하나라도 실패하면(검증 위반·할당 실패) 그때까지 적용한 op를 역순으로 되돌린다. 되돌리기는 할당하지 않는다(8.6절). 문서는 호출 전과 바이트가 같고, 통지는 나가지 않는다. 오류는 `InvalidArgument`(검증, 실패한 명령 번호와 사유를 함께 돌려준다)이거나 할당 오류다.
5. 모두 성공하면 문서 revision을 올리고 적용한 op 목록을 `GraphPatch`로 돌려준다.
6. Transaction에 patch를 넣는다(NG4). bracket 안이면 bracket이 닫힐 때 합친다.
7. 통지 묶음 하나를 보낸다.

개정 5까지의 절차(검증 → 용량 예약 → 무할당 적용)는 개정 6에서 위 절차로 바꿨다. 명령 목록의 앞 명령 결과를 뒤 명령이 보는 검증을 가상 상태 없이 할 수 있고, 같은 보장(원자성, 실패 때 문서 불변, 되돌리기 무할당)을 준다. 할당은 성공 경로의 적용 중에만 한다.

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

묶음에는 `{AssetId, 새 문서 revision, semanticTouched, 원인(Edit·Undo·Redo·Cancel·Migration·Reload·Registry), interactive}`가 붙는다. `Cancel`은 bracket 취소로 되돌린 묶음이다.

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

**형식(의미 바이트 v1).** 정수는 little-endian 고정 폭이고 바이트열은 u64 길이 + 내용이다.
- 머리: magic `RNGS`, 형식 번호 1, 도메인, `mainGraph`, 그래프 수. 문서 형식 버전(`documentFormatVersion`)은 쓰지 않는다. 형식 버전만 오른 문서는 hash가 같다.
- 그래프마다(GraphId 순): id, 역할, `domainRole`, 재경유를 뺀 노드 수와 노드들(NodeId 순), 접은 간선 수와 간선들.
- 간선 하나: 입력 NodeId, 입력 PinKey, 출력 NodeId, 출력 PinKey, 순번.
- 입력 배열 순서와 무관하다. 그래프·노드·입력 기본값·고아 핀·간선을 모두 정규 순서로 정렬해 쓴다. schema가 노드 writer의 쓰기 오류를 무시하고 성공을 돌려줘도 writer가 첫 오류를 기억해 전체를 실패시킨다.
- 노드: `NormalizeSemanticNode`가 노드 writer에 노드 하나를 정확히 한 번 쓴다. 기본은 (NodeId, 종류, `typeSchemaVersion`, 속성 바이트, 입력 기본값 (PinKey, 바이트) 목록, 활성 상태, 고아 핀 (PinKey, 방향, `lastType`, 바이트) 목록)이다. 도메인은 걸러낸 속성 바이트를 넘길 수 있다. 도메인 schema가 등록되지 않았으면 기본을 쓴다.
- Name은 정규 원문(ASCII 소문자 base + 번호)으로 쓴다. 표시 대소문자만 바뀐 키는 의미 바이트를 바꾸지 않는다.
- **접은 간선.** 재경유 사슬을 거슬러 원천(재경유가 아닌 노드의 출력 핀)을 찾아 (원천 → 대상)으로 쓴다. 순번은 사슬의 첫 간선(원천에서 나온 간선)의 순번이다. 원천이 없는 재경유에서 나온 간선은 쓰지 않는다. 재경유를 거쳐 같은 (출력, 입력) 쌍이 둘 이상 생기면 모두 쓴다(정규 순서, 같으면 순번 순). UE knot 접기와 같다.

### 8.5 Guid 발급

`NodeId`·`GraphId`·주석 id는 `GraphIdSource`에서 받는다.
- 에디터는 `TryNewGuid`(무작위 UUID v4)를 쓴다.
- 시험·template·결정적 import는 seed 발급기를 주입한다. seed 발급기의 id는 (seed, 대상 AssetId, 적용 순번)에서 유도한다. 같은 template을 같은 문서에 두 번 적용해도 겹치지 않는다.
- 그래도 겹치면 GI9 검증이 `AddNode`를 거부한다.
- 같은 입력의 붙여넣기·template 적용이 같은 id를 내야 golden 시험이 성립한다. UE MetaSound `FScopeDeterminism`과 같은 목적이다.

### 8.6 NG1 구현 기준

**명령 목록.** `GraphCommandList`는 `GraphCommand`(명령 종류 + 인자)의 배열이다. 명령은 데이터라 메뉴 항목·RPC·headless 시험이 같은 목록을 만든다(7절). 인자 묶음은 8.1절 표와 같고, `SetNodeProperties`는 필드 경로 대신 새 속성 Record 바이트 전체를 받는다(필드 경로 → 바이트 변환은 도메인 도우미 몫, NodeGraphTools가 리플렉션으로 제공 — NG2). `MigrateNode`는 NG3에서 더한다.

**op와 patch.** patch는 op 목록이다. op는 문서 슬롯 하나의 키와 보관 값이다.

| 슬롯 | 키 | 값 |
|---|---|---|
| 그래프 | GraphId | `Graph`(노드·간선이 빈 것) + `GraphLayout`(빈 것, 제목 포함) |
| 그래프 제목 | GraphId | 제목 문자열 |
| 노드 | GraphId·NodeId | `GraphNode` 전체 |
| 노드 배치 | GraphId·NodeId | `NodeLayout` |
| 간선 | GraphId·(출력 NodeId, 출력 PinKey, 입력 NodeId, 입력 PinKey) | `GraphEdge`(순번 포함) |
| 주석 상자 | GraphId·주석 id | `CommentBox` |
| 북마크 | GraphId·번호 | `Bookmark` |

- op 적용은 **맞바꾸기**다. 슬롯이 없고 op에 값이 있으면 넣고, 슬롯이 있고 op에 값이 없으면 빼서 op에 담고, 둘 다 있으면 값을 맞바꾼다. 적용 뒤 op는 옛 값을 담는다.
- 그래서 같은 op 목록을 역순으로 다시 적용하면 되돌려지고(undo), 그 결과를 다시 정순으로 적용하면 다시 하기(redo)다. patch는 방향 표지를 가진다.
- 적용은 값을 옮기기만 해서 할당하지 않는다. 슬롯 넣기는 정규 위치의 배열 삽입이라 용량이 모자라면 할당한다. 그러나 되돌리기·undo·redo는 LIFO이고 배열은 줄지 않는다. 그래서 되넣는 원소 수는 그 배열이 전에 가졌던 수 이하이고, 할당하지 않는다. 그래도 `Try` 계열로 넣고, 실패하면 그 묶음을 되돌린 뒤 오류를 돌려준다.
- 통지 기록(바뀐 필드 비교·통지 배열)은 undo·redo에서도 할당할 수 있다. 실패하면 그 op와 앞 op를 무할당으로 되돌리고 오류를 돌려준다. 실패 경로의 되돌리기는 할당이 하나도 없이 끝나야 하고, 시험은 "k번째부터 모든 할당 실패"로 이를 확인한다(13절 GT6).
- **합치기.** 같은 patch 안에서 "있음 → 있음" op(값 바꾸기)가 같은 키의 직전 op도 값 바꾸기면 새 op를 버린다. 처음 op가 원래 값을 담고 있으므로 결과가 같다. 넣기·빼기 op는 합치지 않는다. bracket의 drag는 노드마다 op 하나로 줄어든다(8.2절 bracket).
- patch는 문서 소유 mutator만 적용한다(mutator 식별을 담는다). patch 크기 추정(`ApproximateBytes`)은 Transaction undo budget(GD6)용이다.

**파생 색인.** 저장하지 않는다.
- 노드·그래프·주석은 정규 순서 배열이라 id 이진 탐색으로 찾는다(별도 표 없음).
- 들어오는 간선은 간선 정규 순서(입력 NodeId가 첫 키)의 범위다.
- 나가는 간선은 (그래프, 출력 NodeId, 출력 PinKey 토큰, 입력 NodeId, 입력 PinKey 토큰) 순 정렬 배열로 둔다.
- 문서 안 id(GraphId·NodeId·주석 id)는 정렬 배열 하나로 둔다(GI9).
- 핀 목록은 필요할 때 등록표로 계산한다(노드 cache는 NG6 성능 측정 뒤).
- 색인은 op 적용 때 함께 바뀌고, mutator를 만들 때 문서에서 한 번 세운다(문서는 검사를 통과해야 한다).

**명령 규칙(검증 위반이면 `InvalidArgument`).**
- `AddNode`: id가 0이 아니고 문서에 없음, 등록된 종류(자리 노드는 파일·붙여넣기로만 생긴다 — NG2·NG3), `typeSchemaVersion` = 서술 버전, 속성 Record schema가 서술과 같고 크기가 `MaxPropertyRecordBytes` 이하(`ValueTooLarge`), 입력 기본값·고아 핀 없음, `SinglePerDocument`면 문서에 같은 종류가 없음, 그래프 있음, 그래프 노드 수가 `MaxGraphNodes` 미만(`GraphFull`). 배치 항목을 함께 넣는다.
- `RemoveNodes`: 노드 있음, 같은 id가 두 번 없음(`DuplicateNode`), 알려진 종류면 `CanDelete`. 닿은 간선(나가는·들어오는 — 자기 자신으로 가는 간선은 한 번)과 배치 항목을 함께 뺀다. 공통 층은 자기 자신으로 가는 간선을 막지 않는다(순환 금지는 도메인 판정 — 6.3절). 빠진 간선이 ordered 출력 핀의 것이면 남은 간선 순번을 0부터 다시 매긴다.
- `Connect`: 양 끝 노드가 같은 그래프에 있고 알려진 종류, 끝 핀이 현재 핀 목록에 있음(고아 핀에는 연결하지 않는다 — 6.4절), 방향이 출력 → 입력, 같은 간선 없음. schema `CanConnect` 판정을 따른다: `Deny`면 위반, `MakeReplacingInput`·`MakeReplacingOutput`·`MakeReplacingBoth`면 해당 핀의 기존 간선을 같은 patch에서 뺀다, `MakeWithConversion`이면 변환 노드(id는 `GraphIdSource`)를 넣고 첫 입력·첫 출력으로 이어 붙인다. 그 뒤 입력 핀 간선 수가 `maxEdges`를 넘으면 위반이다. ordered 출력 핀이면 순번은 그 핀의 지금 간선 수(끝에 붙임)다.
  - 변환 노드도 `AddNode` 규칙(`SinglePerDocument`·노드 수 상한)을 따르고, 핀은 그 노드의 현재 핀 목록(동적 핀 포함)의 첫 입력·첫 출력이다. 변환은 대상 입력의 기존 간선을 빼지 않으므로 대상 입력이 차 있으면 `EdgeLimit`이다.
  - 재경유 노드끼리의 사슬이 순환이 되거나 재경유 자기 간선이면 `RerouteCycle`이다(GI10). 재경유가 아닌 노드를 거치는 순환은 도메인 판정이다.
  - 그래프 간선 수가 `MaxGraphEdges`에 이르렀으면 `GraphFull`이다.
- `Disconnect`: 간선 있음. ordered 출력 핀이면 뒤 간선 순번을 하나씩 당긴다.
- `ReorderEdges`: 출력 핀이 ordered이고, 준 순서가 그 핀 간선 전체의 순열이다.
- `SetNodeProperties`: 알려진 종류, Record schema가 서술과 같음, 크기 상한(`ValueTooLarge`). 새 핀 목록에서 사라지는 핀에 간선·입력 기본값이 있으면 위반이다(고아 처리는 NG3). 새 핀 목록에 고아 핀과 같은 키가 생기면 `RetainedShadowed`다(GI8 — 재결합은 NG3). 바이트가 지금과 같으면 op를 만들지 않는다. 핀 목록이 바뀌면 `NodePinsChanged`를 보낸다.
- `SetInputDefault`: 현재 입력 핀이고 값 정책이 Hidden이 아님, 값이 비어 있지 않고 크기 상한 이하(`ValueTooLarge`). `ResetInputDefault`: 그 기본값이 있음.
- `SetLayout`: 노드 있음. `AddComment`: id가 0이 아니고 문서에 없음. `RemoveComment`·`SetComment`: 있음. `SetBookmark`: 그래프 있음, 값이 없으면 지운다.
- `SetEnableState`: 정의된 값, `Disabled`면 `AllowDisable`.
- `AddGraph`: id가 0이 아니고 문서에 없음, 역할은 Collapsed·DomainDefined(Main은 문서를 만들 때만), `domainRole` 규칙(GI12). `RemoveGraph`: 주 그래프가 아님. 노드·간선·주석·북마크를 같은 patch에서 뺀다. `RenameGraph`: 그래프 있음.
- `MoveNodesToGraph`: 대상 그래프 있음·원본과 다름, 노드들이 원본에 있음, 같은 id가 두 번 없음(`DuplicateNode`), 옮긴 뒤 대상 그래프 노드 수가 `MaxGraphNodes` 이하(`GraphFull`), 옮기는 노드 집합과 나머지 사이 간선이 없음(경계 간선은 먼저 끊는다 — 8.1절 접기). 노드·그 사이 간선·배치를 옮긴다. 그래프에 따라 핀이 달라지는 노드(경계 노드)는 NG3에서 다룬다.
- `RemoveRetainedPin`: 고아 핀 있음. 그 핀의 간선을 함께 뺀다.
- **고아 핀 자동 정리(GI7).** 간선을 빼는 명령(`Disconnect`·`RemoveNodes`·바꾸기 연결·`RemoveRetainedPin`)은 빠진 간선 끝의 고아 핀이 값도 간선도 없게 되면 같은 patch에서 지운다(`RetainedPinChanged`). undo는 되살린다. 값이 있는 고아 핀은 남는다.
- **순번 다시 매기기.** ordered 출력 핀의 간선이 빠지면 남은 간선을 (지금 순번, 입력 NodeId, 입력 PinKey) 순으로 0부터 다시 매긴다. 순번이 같은 간선이 있어도 결과가 정해진다. 순번 통지 `EdgeOrderChanged`는 바뀐 출력 핀마다 하나다(간선마다가 아니다).
- 명령이 쓴 `GraphIdSource` id는 실패해 되돌려도 돌려놓지 않는다(발급기는 문서 밖 상태다). 같은 seed의 재현은 같은 명령열에서만 성립한다.
- 빈 명령 목록·바꾼 것이 없는 목록도 성공이며 revision을 올리고 빈 patch를 돌려준다(expected revision 규칙을 한 가지로 둔다).
- `SetFullValidation(true)`(시험용)면 목록 적용 뒤 문서 전체 검사를 하고, 위반이면 되돌린 뒤 사유 `InvariantBroken`으로 거부한다. 명령 검사가 놓친 위반이라는 뜻이고 시험에서 나오면 결함이다.
- 실패 정보 `GraphCommandFailure`는 (명령 번호, 사유 코드, 관련 id·핀)이다. 사유 코드는 영어 이름을 가진 enum이다.

**bracket.** `BeginInteraction(source)`·`EndInteraction(source)`·`CancelInteraction(source)`이다. source는 명령 출처 번호다(화면·RPC·스크립트마다 다르다).
- bracket이 열려 있으면 다른 source의 명령·undo·redo는 `Busy`다. 같은 source가 다시 열면 `Busy`(중첩)다.
- bracket 안 명령은 즉시 적용되고 `interactive` 통지를 보낸다. patch는 bracket patch 하나로 이어 붙이고 위 규칙으로 합친다.
- `EndInteraction`은 합친 patch를 돌려주고, 바뀐 것이 없는 확정 통지(`interactive` 거짓, revision 그대로, `semanticTouched`는 bracket 전체 기준)를 보낸다.
- `CancelInteraction`은 bracket patch를 되돌리고 원인 `Cancel` 통지를 보낸다. 통지 기록 할당이 실패하면 문서를 bracket 상태로 둔 채 오류를 돌려주고 bracket은 열려 있다(다시 취소하거나 확정한다). `EndInteraction`은 할당하지 않는다.

**undo 순서.** undo·redo는 LIFO다. mutator는 문서 상태 토큰을 둔다. 문서를 바꾼 적용마다 새 토큰을 받고, patch는 적용 전후 토큰을 담는다. undo는 지금 토큰이 patch의 "후"와 같을 때만, redo는 "전"과 같을 때만 한다. 다르면(마지막이 아닌 patch의 undo, 순서가 틀린 redo) `StaleHandle`이고 문서는 그대로다. 빈 patch는 전후 토큰이 같다. 순서가 틀린 맞바꾸기 적용은 다른 슬롯 값을 덮어 문서를 깨뜨리므로 검사로 막는다. Transaction(NG4)도 LIFO라 이 검사에 걸리지 않는다.

**통지.** 통지 묶음은 적용한 op에서 만든다. 노드 op는 넣기·빼기면 `NodeAdded`·`NodeRemoved`, 바꾸기면 바뀐 필드마다 `NodePropertiesChanged`(+ 핀 목록이 달라지면 `NodePinsChanged`)·`InputDefaultChanged`(핀마다)·`NodeEnableStateChanged`·`RetainedPinChanged`(키마다)다. 간선 op는 `EdgeAdded`·`EdgeRemoved`·`EdgeOrderChanged`다. 배치·주석·북마크·그래프 제목 op만 있는 묶음은 `semanticTouched`가 거짓이다. 구독은 `GraphChangeListener` 등록이다(mutator 소유 lane에서 부른다).

**Guid 발급기.** `GraphIdSource` interface의 `Next()`다.
- `RandomGraphIdSource`는 `TryNewGuid`다.
- `SeededGraphIdSource(seed, assetId)`는 SHA-256(seed ‖ assetId ‖ 순번)의 앞 16바이트다. 0이면 다음 순번이다. 순번은 발급기 상태다. 같은 seed·asset·호출 순서면 같은 id다.

**schema 추가(NG1).**
- `CanConnect(view, from, to)`: 기본 구현이 있다. 핀 종류가 같고 타입이 같거나 한쪽이 `UnresolvedPinType`이면 연결한다. 입력 핀이 `maxEdges`만큼 차 있으면 `MakeReplacingInput`(UE 단일 입력 핀 기본과 같다)이고, 그 밖은 `Deny`다.
- `CreateDefaultNodes(role, list)`: 새 그래프의 기본 노드 명령을 덧붙인다. 기본은 없음이다.
- `Validate(view, sink)`: 도메인 구조 검사다. 기본은 없음이다. 진단은 `GraphDiagnostic{AssetId, GraphId, NodeId, PinKey, 심각도, 코드, 메시지}`(GD19)이고, `GraphViolation`을 진단으로 바꾸는 함수를 둔다.
- `GraphReadView`는 문서·등록표의 읽기 전용 view다(노드 찾기, 핀 목록, 들어오는·나가는 간선).
- 순환 검사 도우미 `WouldCreateCycle(graph, from, to, scratch)`는 to 노드에서 나가는 간선을 따라 from 노드에 닿는지 본다(재경유 포함). 탐색 비용은 도달 범위에 비례하고, 인접 표를 만드는 데 그래프 간선 수에 비례하는 scratch 할당을 한다. 같은 노드 끼리면 참이다.
- 등록표의 종류 조회는 (도메인, 종류) hash 표다. hash 충돌이나 넣기 실패가 있으면 표를 불완전으로 표시하고, 빗나간 조회를 선형 탐색으로 확인한다. 완전한 표에서 빗나감은 곧 없음이다.

**NG3·NG4로 미루는 것(NG1 리뷰).** 통지 묶음의 AssetId(문서 소유 lane과 함께 — NG4), 통지 원인 Migration·Reload·Registry와 `DocumentReloaded`·`NodeTypesChanged`(NG3), `RemoveGraph`가 지운 하위 그래프를 가리키는 부모 Collapsed 노드 참조 정리(경계 노드와 함께 — NG3).
