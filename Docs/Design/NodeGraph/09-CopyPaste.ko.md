# 노드 그래프(NodeGraph) 설계

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
      "properties": { "type": "MultiplyProperties", "value": { "<리플렉션 텍스트 형식>": "..." } },
      "inputDefaults": { "B": { "type": "FloatValue", "value": { "value": 0.5 } } },
      "layout": { "x": 120.0, "y": -40.0 }
    }
  ],
  "edges": [ { "from": ["<NodeId>", "Result"], "to": ["<NodeId>", "A"], "order": 0 } ],
  "comments": [],
  "externalEdges": []
}
```

- 쓰기는 `JsonWriter` Canonical이라 같은 조각은 같은 바이트다. 읽기는 Json5 문법도 받는다(사람이 고친 조각).
- 속성은 등록된 타입이면 리플렉션 텍스트 형식(`ReflectionText`)으로 쓴다(`{"type", "value"}`). 모르는 타입이면 `{"record": "<base64 Record 바이트>"}`로 쓴다. 그래서 모르는 노드도 같은 문서 사이 복사에서 바이트가 보존된다. 정확한 형식은 9.4절이다.
- 붙여넣기 처리는 다음과 같다.
  1. 도메인이 다르면 전체를 거부한다.
  2. NodeId·주석 id를 새로 발급하고 조각 안 간선을 다시 잇는다.
  3. 붙이는 위치를 기준으로 배치를 옮긴다.
  4. `properties` 텍스트인데 지금 타입이 등록되어 있지 않은 노드는 바이트로 바꿀 수 없다. 그 노드와 닿은 간선을 빼고, 진단으로 보고한다. 나머지는 붙인다.
  5. `record` 형식 속성의 모르는 종류 노드는 자리 노드로 붙인다.
  6. migration이 필요한 노드는 migration 명령을 함께 넣는다.
  7. 결과 명령 목록 하나를 `GraphMutator`에 보낸다. undo 1개다.
- 바깥 간선은 같은 문서이고 대상 노드가 아직 있으며 `AllowExternalEdgeReconnect()`가 참일 때만 다시 잇는다. 다시 이을 때도 schema 판정을 거친다. 기본값은 끊기로, UE와 같다(`EdGraphUtilities.cpp:46–102`).
- 같은 형식을 template 조각(계획 22.3절)과 commandlet dump에도 쓴다.
- 조각 codec은 리플렉션 타입 조회가 필요하므로 NodeGraphTools(Developer)에 있다(11.1절).

### 9.3 텍스트 dump

`graph-dump`는 문서 전체를 9.2절 형식(의미 + 배치)으로 출력한다. `--semantic-only`는 배치를 뺀다. golden 시험과 source control diff 보기(계획 14.5절 "source control diff")에 쓴다.

### 9.4 NG2 구현 기준

**단계.** NG2a 독립 struct Record(리플렉션 보강 26절 — ObjectSerialization), NG2b NodeGraphTools 텍스트 codec(조각·문서), NG2c 복사·붙여넣기(공통 명령 `InsertNode`·`InsertEdge`, schema `CanPasteNode`·`AllowExternalEdgeReconnect`), NG2d 문서 diff·commandlet `graph-dump`·`graph-validate`·`graph-diff`. 단계마다 리뷰어 1명이다.

**레코드 값(속성·입력 기본값·고아 핀 값).** `PropertyRecord` 하나는 다음 둘 중 하나다.
- 텍스트: `{"type": "<struct 도구 이름, 없으면 struct Guid>", "value": <ExportJson 객체>}`. 쓰기는 Record header의 struct를 리플렉션 TypeRegistry에서 열어(리플렉션 보강 26절 `AcquireStructRecordType`) `LoadStructRecord`로 값을 만든 뒤 `ExportJson`한다. 옛 버전 Record는 마이그레이션된 현재 버전으로 쓰인다. 읽기는 `type`을 Guid로 먼저, 아니면 도구 이름으로 열고(`AcquireByToolName`) 기본 생성 값에 `ImportJson`한 뒤 `SaveStructRecord`한다.
- 바이트: `{"record": "<base64>"}`(Core `Encoding/Base64.h`, RFC 4648 패딩). 타입이 등록되어 있지 않거나 해석이 실패하면(손상·newer·담기 조건 위반) 이것으로 쓴다. 읽기는 Record header만 검사하고(`ReadRecordHeader`) 바이트를 그대로 쓴다. 쓰기 옵션 `records = true`(commandlet `--records`)는 모든 값을 바이트로 쓴다(바이트 일치 dump).
- 빈 Record(속성 struct가 없는 종류)는 키를 쓰지 않는다.
- 텍스트 형식은 바이트 일치를 약속하지 않는다(마이그레이션·리플렉션 텍스트 정규화). 현재 버전 Record는 `SaveStructRecord`가 결정적이므로 텍스트 왕복이 같은 바이트다(시험한다).

**조각 형식(`rocks.graph-fragment`, formatVersion 1).** 최상위 키는 이 순서로 쓴다: `format`, `formatVersion`, `domain`, `sourceAsset`(있을 때), `sourceGraph`, `nodes`, `edges`, `externalEdges`, `comments`.
- 노드: `id`, `type`, `typeVersion`, `properties`(레코드 값, 비면 없음), `enableState`(`Disabled`일 때만, 값은 enum 이름), `inputDefaults`(핀 이름 → 레코드 값 객체, 정규 순서, 비면 없음), `retainedPins`(배열 — `key`, `direction`, `lastType`(0이 아니면), `value`(있으면)), `layout`(`x`·`y`는 늘, `width`·`height`·`collapsed`·`showAdvanced`·`bubble`·`bubbleVisible`은 기본값이 아닐 때만).
- 간선: `{"from": ["<NodeId>", "<핀>"], "to": ["<NodeId>", "<핀>"], "order": n}`(`order`는 0이 아닐 때만). `edges`는 조각 노드끼리, `externalEdges`는 조각 노드와 밖 노드 사이 간선(원래 끝 그대로)이다.
- 주석: `id`, `x`, `y`, `width`, `height`, `text`, `color`(`[r, g, b, a]`), `fontSize`.
- Guid는 36자 소문자 표기(`{}` 서식), Name은 표시 문자열(NamePool 원문 + 번호 접미)이다. 실수는 `Float`(단정밀도 최단 표기)다.
- 읽기: Json5 문법을 받고, 모르는 키·중복 키·형식 오류는 그 줄·열과 함께 거부한다(문서 전체 실패 — 부분 읽기 없음). `formatVersion`은 1만 받는다.

**문서 형식(`rocks.graph-document`, formatVersion 1 — `graph-dump`).** `format`, `formatVersion`, `domain`, `documentFormatVersion`, `mainGraph`, `graphs`. 그래프마다 `id`, `role`(enum 이름), `domainRole`(0이 아니면), `title`(비면 없음), `nodes`, `edges`, `comments`, `bookmarks`(`number`, `x`, `y`, `zoom`)다. 노드·간선·주석 형식은 조각과 같다. 배열 순서는 정규 순서(6.3절)다. `semanticOnly` 쓰기는 `layout`·`title`·`comments`·`bookmarks`를 빼고, 그 문서를 읽으면 배치는 기본값이다. 읽은 문서는 `CanonicalizeGraphDocument`로 정규 순서를 맞춘 뒤 돌려준다(불변식 검사는 호출자 — `graph-validate`).

**API(NodeGraphTools).**
- `GraphTextContext{Allocator&, NamePool& names, ObjectRegistry& objects, const ObjectAccessContext& access}`: 레코드 값 텍스트화에 쓰는 리플렉션 Registry와 그래프 Name pool이다.
- `WriteGraphDocumentJson(context, document, layout*, options, StringBuilderBase&)`·`ReadGraphDocumentJson(context, StringView, document&, layout&, GraphTextDiagnostics&)`, 조각은 `WriteGraphFragmentJson`·`ReadGraphFragmentJson`과 메모리 형식 `GraphFragment`(domain, sourceAsset, sourceGraph, 노드(`GraphNode` + `NodeLayout` + 미해석 표지), 간선, 바깥 간선, 주석).
- 텍스트 속성의 타입이 지금 등록되어 있지 않으면 바이트로 바꿀 수 없다. 조각 읽기는 그 노드를 미해석 표지(`unresolvedType` — 텍스트의 type 문자열)와 함께 둔다(붙여넣기가 진단하고 뺀다 — 9.2절 4). 문서 읽기는 그 경우 실패다(문서를 바꿔 쓰지 않는다).
- 진단 `GraphTextDiagnostics`: (줄, 열, 코드, 메시지) 목록, JSON 오류는 `JsonError` 위치, 리플렉션 텍스트 오류는 `TextDiagnostics`의 위치를 옮긴다.

**복사(`CopyGraphFragment`).** 입력은 문서·배치, 그래프 id, 노드 id 목록, 주석 id 목록, sourceAsset이다. 노드·배치·주석을 그대로 담고, 고른 노드끼리의 간선은 `edges`, 고른 노드와 나머지 사이 간선은 `externalEdges`다. 노드의 `CanDuplicate`가 거짓이면 뺀다(알려진 종류만). 없는 id는 `NotFound`다.

**붙여넣기(`BuildPasteCommands`).** 입력: 조각, 대상 문서의 `GraphReadView`·등록표, 대상 그래프 id, 붙이는 위치(조각 노드·주석 왼쪽 위가 그 위치가 된다), 대상 AssetId, `GraphIdSource`. 출력: 명령 목록 하나(undo 1개), 진단 목록, 옛 id → 새 id 표(노드·주석).
1. 도메인이 다르면 전체 실패다(`InvalidArgument`). 대상 그래프가 없으면 `NotFound`다.
2. 노드마다 판정한다(뺀 노드는 진단 하나): 미해석 텍스트 속성, 알려진 종류인데 `typeVersion`이 서술 버전과 다름(옛 버전은 migration — NG3, 새 버전은 거부), 속성 Record schema가 서술과 다름, `CanDuplicate` 거짓, `SinglePerDocument` 충돌(문서에 있거나 조각 안에 둘째), schema `CanPasteNode`가 `Deny`.
3. 남은 노드에 새 id(발급기 — 조각 노드 순서), 위치 이동, `InsertNode`를 만든다. 값 없는 고아 핀은 붙는 간선이 남지 않으면 뺀다(GI7).
4. 조각 간선 중 두 끝이 남은 노드인 것을 원래 순번 순으로 `InsertEdge`한다(schema 판정 없음 — 원본에서 유효했던 연결을 그대로 옮긴다. UE도 복사 집합 안 링크는 그대로 둔다 — `EdGraphUtilities.cpp:48–51`).
5. 바깥 간선은 대상 AssetId가 조각의 sourceAsset과 같고, `AllowExternalEdgeReconnect()`가 참이고, 밖 끝 노드가 대상 그래프에 있고, 그 핀이 지금 핀 목록에 있으며, `CanConnect` 판정이 `Make`일 때만 `Connect`한다. 바꾸기·변환 판정은 붙여넣기가 기존 간선을 바꾸게 되므로 하지 않고 진단한다. 기본은 끊기다.
6. 주석에 새 id와 위치 이동, `AddComment`.
- 같은 조각·대상·seed 발급기면 같은 명령 목록, 실행 뒤 같은 바이트다(GT5).

**공통 명령 추가(NG2c).**
- `InsertNode(graph, node, layout)`: 완성된 노드 데이터를 넣는다(붙여넣기·template·NG3 열기 복원). id가 0이 아니고 문서에 없음, 그래프 있음, 노드 수 상한, 값 크기 상한. 알려진 종류면 `AddNode`의 종류 규칙(버전·속성 schema·`SinglePerDocument`)에 더해 입력 기본값이 현재 입력 핀이고 Hidden이 아님(GI14), 고아 핀이 현재 핀을 가리지 않음(GI8)을 본다. 모르는 종류(자리 노드)는 종류 규칙 없이 바이트 그대로 넣는다. 값 없는 고아 핀은 넣을 수 있다 — 목록 끝의 전체 상태가 GI7을 지켜야 한다(붙여넣기가 보장, 시험용 전체 검사가 확인).
- `InsertEdge(graph, edge)`: schema 판정 없이 간선을 넣는다. 양 끝 노드가 그래프에 있음, 알려진 종류의 끝은 현재 핀이거나 그 노드의 고아 핀(방향 일치), 같은 간선 없음, 입력 상한(알려진 핀), 재경유 순환 없음(GI10). ordered 출력 핀이면 순번은 끝에 붙인다(주어진 `order`는 무시 — 붙여넣기가 원래 순번 순으로 넣는다). 출력 끝이 알 수 없는 종류나 고아 핀이면 주어진 `order`를 그대로 둔다(순번 규칙이 없다 — 6.3절, 자리 노드 바이트 보존).
- 둘 다 8.6절의 크기 상한·실패 규칙을 따른다.

**schema 추가(NG2c).** `CanPasteNode(view, node) -> PasteDecision{Allow, Deny}`(기본 Allow), `AllowExternalEdgeReconnect()`(기본 거짓).

**구현(NG2c)에서 정한 점.**
- `InsertNode`는 노드 안 입력 기본값·고아 핀 목록을 `CompareNames` 삽입 정렬로 정규 순서에 맞추고, 같은 키가 있으면 새 사유 `NodeDataInvalid`다. 크기 상한은 속성·입력 기본값·고아 핀 값 모두에 건다. 고아 핀 방향이 정의되지 않은 값이어도 `NodeDataInvalid`다.
- `InsertEdge`는 NG1 `ConnectResolved`(schema 판정 끔)를 쓴다. 고아 핀·알 수 없는 종류 끝은 상한·순번 없는 서술로 두고(방향은 본다 — 알 수 없는 종류는 방향도 모른다), 출력 끝이 그런 끝이면 주어진 순번을 둔다.
- 붙여넣기(`NodeGraphTools/GraphPaste.h`): 진단 코드 `GraphPasteCode`(UnresolvedProperties·TypeVersion·PropertiesSchema·CannotDuplicate·SinglePerDocument·PasteDenied·ExternalEdgeDropped·EdgeDropped, 심각도 Warning), 결과 `GraphPasteResult{commands, diagnostics, nodes, comments}`(옛 id → 새 id). 같은 문서 판정은 대상 AssetId가 0이 아니고 조각의 sourceAsset과 같을 때다. 바깥 간선 판정의 간선 수(`ConnectQuery`)는 붙여넣기 뒤 수(기존 + 조각 + 앞서 고른 바깥 간선)다. 조각 간선은 원래 순번의 안정 정렬 순으로 `InsertEdge`한다.
- 복사(`CopyGraphFragment`)는 등록표를 받아 `CanDuplicate` 거짓 노드를 뺀다. 같은 id가 두 번이면 InvalidArgument다.
- 리뷰 1회 반영(필수 2·권장 5·참고 다수).
  - `InsertNode`: 재경유 노드의 고아 핀(GI13)·None 키(GI11)를 `NodeDataInvalid`로 거부한다. 목록 끝에 그 목록이 넣은 노드의 값 없는 고아 핀에 간선이 없으면 `NodeDataInvalid`다(GI7 — 붙여넣기 밖 호출자도 지킨다). 낡은 Name 토큰도 `NodeDataInvalid`다.
  - `InsertEdge`: 핀 키 None은 `PinMissing`이다(알 수 없는 종류 끝도).
  - `Connect`에 `requireMake`(명령 칸)를 두었다: 판정이 Make가 아니면 `ConnectDenied`다. 붙여넣기의 바깥 간선은 이것으로 보낸다 — 빌드 때와 실행 때 schema 판정이 달라도 붙여넣기가 기존 간선을 바꾸지 않는다.
  - 붙여넣기는 InsertNode 규칙(활성 상태·키·고아 핀 방향·가림·재경유 고아 핀, 대상 그래프의 핀 계산, 입력 기본값이 Hidden이 아닌 현재 입력 핀)을 미리 보고 걸리는 노드를 새 진단 `NodeData`로 뺀다. 조각 간선도 끝 핀이 현재 핀·고아 핀(방향 일치)인지 미리 보고 아니면 `EdgeDropped`다. 그래서 명령 목록 전체가 거부되지 않는다.
  - 진단의 노드 id는 조각 쪽 옛 id다(조각 간선은 빠진 끝, 바깥 간선은 조각 쪽 끝).
  - 복사는 조각 노드·주석을 id 순으로 담는다(고른 순서와 무관 — 새 id 배정이 재현된다). 주석은 `CloneCommentBox`로 복제한다. 조각 간선 순서는 (순번, 조각 위치) 정렬이다.
  - NG3로 남김: 고아 핀·자리 노드 출력에 남긴 순번은 그 핀이 현재 핀으로 재결합될 때 다시 매긴다. Collapsed 노드를 붙여넣으면 하위 그래프 참조를 공유한다(UE는 하위 그래프도 복제 — 경계 노드와 함께 정한다).
- 시험: NodeGraph `NodeGraph_InsertNodeKeepsCompleteData`·`NodeGraph_InsertNodeRules`·`NodeGraph_InsertEdgeEnds`(undo·redo 바이트)·`NodeGraph_InsertNodeDataInvariants`·`NodeGraph_ConnectRequireMake`와 무작위 명령열(새 명령 포함), NodeGraphTools `GraphPaste_*` 6개(GT5 — 왕복·undo·같은 seed 같은 바이트·텍스트 경유, 노드 판정·자리 노드, 바깥 간선 정책, 고아 핀 정리, 복사 오류, 사전 판정과 두 번 붙이기).

**diff(`ComputeGraphDiff`).** 두 문서(배치 선택)를 GraphId·NodeId·간선 키·주석 id·북마크 번호로 맞춘다. 키로 정렬한 사본을 병합한다(O(N log N) — 입력이 정규 순서가 아니어도 된다, 다만 GI5의 키 중복 없음을 전제한다). 항목: 그래프 추가·삭제·역할·제목, 노드 추가·삭제·종류·버전·속성 바이트·활성 상태·입력 기본값(핀마다)·고아 핀(키마다), 간선 추가·삭제·순번, 배치(노드마다), 주석 추가·삭제·변경, 북마크. `semanticOnly`면 배치·제목·주석·북마크를 보지 않는다. 출력은 항목마다 한 줄이고 순서는 그래프 id 순, 그래프 안에서 그래프 → 노드(id 순) → 간선(정규 간선 순서 — 핀 원문 순이라 Name pool의 발급 순서와 무관) → 배치 → 주석 → 북마크다(결정적). 속성 차이는 바이트 비교다(값 텍스트를 덧붙이는 것은 화면 diff — NG7 — 와 함께 정한다).

**commandlet(RocksCommandlet).** NodeGraphTools가 `RegisterGraphCommandlets(registry, context)`를 두고 호스트가 부른다. 입력은 문서 형식 텍스트 파일이다(도메인 에셋 안의 문서를 읽는 경로는 도메인 단계가 같은 commandlet 문맥에 읽기 hook으로 더한다). 문맥 `GraphCommandletContext{allocator, names, registry, setupNodes, setupTypes, user}`: 호스트가 Name pool·노드 종류 등록표를 이미 가지면(편집기 프로세스) 그것을 주고, 아니면 명령마다 새로 만들어 `setupNodes`로 채운다. 리플렉션 TypeRegistry는 명령마다 새로 만들어 `setupTypes`로 속성·값 struct를 등록한다.
- `graph-dump <파일> [--semantic-only] [--records] [--out=<파일>]`: 읽고 정규화해 다시 쓴다.
- `graph-validate <파일>`: `ValidateGraphDocument` + 등록된 도메인 schema `Validate` 진단을 쓴다(schema 검사는 공통 불변식 위반이 없을 때만 — `GraphReadView`는 유효 문서를 전제한다). 오류 진단이 있으면 1, 경고·정보만이면 0이다.
- `graph-diff <파일 A> <파일 B> [--semantic-only]`: diff 줄을 쓴다.
- 종료 코드: 0 성공(같음·오류 진단 없음), 1 차이·오류 진단 있음, 2 사용법 오류(모르는 옵션, 두 번째 입력·`--out`, 빈 `--out=`), 3 입력 오류(파일·형식·크기 — 형식 오류는 `<경로>:<줄>:<열>: <코드>: <메시지>`, 문서 파일 상한 64 MiB = JSON 읽기 기본 입력 한도), 4 내부 오류(할당·등록 hook·문맥 오류 — 줄 `<명령>: internal error (<오류 이름>)`). 경로는 길이를 받는 Open으로 연다(경로 안 NUL 거부). NG2에는 도메인이 없어 호스트는 hook 없이 등록한다 — 등록되지 않은 도메인 문서는 모든 노드가 자리 노드로 읽히고 `graph-validate`는 도메인 미등록 진단(`GraphDomainNotRegisteredCode` = 1000 — GI 번호와 겹치지 않는 값)을 낸다. `graph-validate` 줄은 `<심각도> <코드> graph <id> node <id> [pin <이름>]: <메시지>`다.

**버린 대안.** (1) 붙여넣기를 `AddNode` + `SetInputDefault` + `Connect`로만 만들기: 자리 노드·고아 핀·입력이 찬 핀 연결을 옮길 수 없고 schema 판정이 원본 연결을 바꿀 수 있다. (2) 레코드 값을 늘 base64로 쓰기: 사람이 읽고 고치고 diff로 보는 목적(GD12·GD22)을 잃는다. (3) 문서 읽기에서 미해석 노드를 자리 노드로 바꾸기: dump 왕복이 속성을 잃는다.

**구현(NG2d)에서 정한 점.**
- `NodeGraphTools/GraphDiff.h`: `ComputeGraphDiff`(항목 `GraphDiffEntry{kind, graph, node, other, pin, otherPin, number}` — 문서 머리·그래프 추가·삭제·역할·제목, 노드 추가·삭제·종류·버전·속성·활성 상태, 입력 기본값·고아 핀(키마다), 간선 추가·삭제·순번, 노드 배치, 주석 추가·삭제·변경, 북마크), `FormatGraphDiff`(줄 머리말 `graph-added`·`node-properties`·`edge-order … order n` 등). 짝 맞추기는 키로 정렬한 사본의 병합이라 입력이 정규 순서가 아니어도 같은 결과다. 간선 키의 Name은 토큰 값으로 비교한다(같은 pool). 실수는 비트로 비교한다(-0.0 구별).
- `NodeGraphTools/GraphCommandlets.h`: `RunGraphDumpCommand`·`RunGraphValidateCommand`·`RunGraphDiffCommand`(시험이 직접 부른다)와 `RegisterGraphCommandlets`. RocksCommandlet(`Programs/Commandlet`)이 NodeGraphTools에 의존해 등록한다.
- 리뷰 1회 반영(필수 1·권장 6·참고 다수): 간선 줄 순서를 Name 토큰 순에서 정규 순서(원문)로 바꿨다(`ComputeGraphDiff`가 `NamePool&`를 받는다 — 짝 맞추기는 토큰 비교 그대로), 길이 있는 경로 Open, 내부 오류 종료 코드 4, 문서 파일 크기 상한, 위반 문서의 schema 검사 생략, `--out` 두 번 사용법 오류, 등록 때 names·registry 짝 검사, 호스트 문맥 선언 순서. 남긴 것: `--out` 쓰기 도중 실패하면 대상 파일이 잘린 채 남는다(임시 파일 + 이름 바꾸기는 Core 원자적 쓰기 도우미가 생기면), 노드 배치 비교는 두 쪽 모두 그 그래프 배치가 있을 때만이다.
- 시험: `GraphDiff_*` 5개(같음, 항목 종류 전부와 줄 형식·semanticOnly·입력 순서 무관, 순번, 나머지 종류, pool 발급 순서 무관), `GraphCommandlet_*` 5개(GT10 golden — `Tests/GraphDumpGolden.h`, `--out`·`--semantic-only`·`--records`, 사용법·파일·형식·둘째 파일 오류, 디렉터리 출력, 검증 통과·위반·도메인 미등록, 내부 오류 4, diff 같음·배치만 다름, 등록).

**한계.** 텍스트 속성은 등록된 리플렉션 struct만 된다(노드 속성 struct는 InstancedStruct 담기 조건 안 — 강한 참조 없음). commandlet은 텍스트 파일만 읽는다. diff는 이동한 노드(다른 그래프로 옮김)를 삭제 + 추가로 보인다.

**구현(NG2b)에서 정한 점.**
- 모듈 `Engine/Source/Developer/NodeGraphTools`(의존 NodeGraph·ObjectSerialization·ReflectionText), 헤더 `NodeGraphTools/GraphText.h`. 구현은 레코드 값(`GraphRecordText.cpp`)·쓰기(`GraphTextWrite.cpp`)·읽기(`GraphTextRead.cpp`)로 나눴다.
- 리플렉션 텍스트 용도는 문서가 `TextUse::Edit`, 조각이 `TextUse::Copy`(UE PPF_Copy — 복사 제외 필드)다.
- 레코드 값 텍스트의 키 순서는 `ExportJson`의 스키마 필드 순서다. 텍스트는 임시 JSON으로 먼저 만들고, 그 텍스트를 다시 읽어(`ImportJson` → `SaveStructRecord`) 읽은 값을 다시 쓴 바이트와 같을 때만 텍스트로 쓴다. 그 밖(해석 실패, `ExportJson`이 못 쓰는 종류, ReadOnly 필드처럼 Value 대상 import가 받지 않는 값, 용도의 출력 제외 필드처럼 텍스트에서 빠지는 값)은 바이트 형식이다. 그래서 "현재 버전 Record는 텍스트 왕복이 같은 바이트"가 늘 성립한다. 할당 실패만 쓰기 전체를 실패시킨다.
- `type` 문자열은 struct 도구 이름(`TypeSchema::toolName`)이 있으면 그것, 없으면 struct Guid 표기다. 읽기는 Guid 표기를 먼저 시도한다.
- `GraphTextContext`는 접근 문맥(`ObjectAccessContext`)을 값으로 가진다(`OwnerAccess()`가 임시 값을 돌려주므로).
- 읽기는 첫 오류에서 멈추고 진단 하나를 남긴다(줄·열은 그 JSON 값의 시작). 리플렉션 텍스트 오류는 그 진단(이름·경로·메시지)을 모두 옮긴다. 실수는 원문 숫자를 `TryParseFloat`로 바로 읽는다(double을 거치면 이중 반올림 — FLT_MAX가 거부된다). Name은 ASCII 대소문자를 구별하지 않으므로 대소문자만 다른 입력 기본값 핀·고아 핀 키는 중복으로 거부한다. base64 Record는 풀어 본 크기로 `MaxPropertyRecordBytes`를 다시 본다. 빈 type 문자열은 InvalidValue다. 정규화 실패도 진단을 남긴다.
- 배치 크기의 기본값 판정은 비트로 한다(-0.0은 기본값이 아니라 쓴다).
- 의존: Public NodeGraph, Private ObjectSerialization·ReflectionText(공개 헤더는 레코드 codec을 드러내지 않는다).
- 시험(`GraphText_*` 7개): 문서 왕복(바이트·텍스트 결정성, 텍스트·바이트 형식 혼합, `records`), 모든 배치 필드·제목·domainRole·-0.0·FLT_MAX 왕복, ReadOnly 필드 속성의 바이트 폴백, `semanticOnly`(의미 바이트 같음, 배치 기본값), Json5 입력, 조각 왕복과 미해석 노드, 읽기 오류 15종의 진단 종류·줄, 경계 입력(Record 크기 한도 + 1, 대소문자 중복 핀·고아 핀 키, 빈 type, 조각 키 누락, JSON 중복 키).
- 리뷰 1회 반영(필수 3·권장 5·참고 6): 위 쓰기 검증 왕복·실수 읽기·base64 크기·대소문자 중복·빈 type·정규화 진단·-0.0·의존 정리. 남긴 것: 깊이 120 넘는 struct는 출력 JSON 깊이 한도로 쓰기 전체가 실패한다(바이트 폴백 없음 — 현실 struct 깊이는 16 이하, 리플렉션 `MaxStructDepth`).
