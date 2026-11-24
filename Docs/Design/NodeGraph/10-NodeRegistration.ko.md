# 노드 그래프(NodeGraph) 설계

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

**해제.** 문서와 patch는 속성을 바이트로 가지므로(GD23) 노드 종류 lease를 쥐지 않는다. `Unregister` 순서는 다음과 같다(10.1절).
1. 조회(메뉴·새 해석 요청)에서 그 종류를 뺀다. 해제는 등록표 소유 lane에서 부르므로 그 lane에 진행 중인 해석이 없다(다른 스레드는 서술을 쥐지 않는다).
2. `NodeTypesChanged`를 보낸다. 구독자는 콜백 안에서 그 종류의 서술에서 얻은 것(핀 목록의 표시 이름 등 모듈 데이터를 가리키는 값)을 버린다. 열린 문서의 그 종류 노드는 자리 노드로 표시된다. 문서 바이트와 revision은 그대로다.
3. lease를 놓는다.

다시 등록되면 같은 통지로 원래 표시로 돌아간다.

### 10.1 NG3 구현 기준

NG3은 세 묶음으로 나눈다. 각 묶음은 앞 묶음에 기대고, 묶음마다 리뷰어 1명 판정을 받는다.

| 묶음 | 내용 |
|---|---|
| NG3a | 핀 재정리 공통 루틴, `SetNodeProperties`의 고아 처리·재결합, `MigrateFn`·migration 적용(`GraphMutator::Migrate`·`MigrateGraphDocument`) |
| NG3b | 등록표 변경 통지(`NodeTypesChanged`), `GraphMutator::RefreshNodeTypes`·`Reload`(`DocumentReloaded`), 통지 원인 Migration·Reload·Registry |
| NG3c | 하위 그래프 경계 노드 `Graph.Input`·`Graph.Output`·`Graph.Collapsed`, 그래프 인터페이스(`Graph`의 `ownerNode`·`inputs`·`outputs`)와 `SetGraphBoundary`, 접기·펼치기 명령 목록 도우미, `RemoveGraph`·`RemoveNodes`의 하위 그래프 정리 |

**핀 재정리(NG3a).** 노드의 핀 목록이 바뀌는 곳(속성 변경, migration, 경계 변경 — NG3c)은 모두 같은 내부 루틴 하나를 쓴다. 입력은 옛 노드, 새 노드 데이터(종류·버전·속성), 키 재지정 목록(migration만), 옛 핀 목록(알 때만)이다. 결과는 새 노드의 입력 기본값·고아 핀과 그 노드에 닿은 간선의 새 키·순번이다. 규칙은 6.4절을 구체화한 것이다.
1. 키 재지정: 간선 끝·입력 기본값·고아 핀의 키를 재지정 목록으로 바꾼다. 재지정에 없는 키는 그대로 간다(항등). 노드에 쓰인 서로 다른 두 옛 키가 같은 새 키로 가거나(예: 이미 B가 있는데 A → B) 새 키가 None이면 실패다(`MigrationInvalid`).
2. 간선 끝: 키가 새 핀 목록에 같은 방향으로 있으면 그 핀의 간선이다. 키가 없으면 그 키의 고아 핀 간선이 된다(고아 핀 방향 = 간선 끝 방향). 키가 있는데 방향이 다르면 실패다(`PinDirection` — 같은 키를 현재 핀과 고아 핀에 함께 둘 수 없다, GI8). 간선은 키로 핀을 가리키므로 키 재지정이 없으면 간선 데이터는 바뀌지 않는다.
3. 입력 기본값: 새 입력 핀이 있고 값 정책이 Hidden이 아니면 남긴다. 키가 없으면 그 키 고아 핀의 값이 된다. 키가 출력 핀이면 `PinDirection`, Hidden 입력 핀이면 `ValueHidden`으로 실패한다(GI8).
4. 기존 고아 핀: 같은 키가 새 핀 목록에 같은 방향으로 생기면 재결합한다(간선·값이 현재 핀으로, 고아 핀 제거 — GI8). 방향이 다르면 실패다(`RetainedShadowed`).
5. 결과 검사: 현재 핀에 남거나 재결합한 간선·값이 새 핀 규칙을 지켜야 한다. `maxEdges` 초과는 `EdgeLimit`, Hidden 핀의 값은 `ValueHidden`이다. 순번은 새 핀이 `ordered`면 (옛 순번, 정규 간선 순서) 순으로 0부터 다시 매기고, 아니면 0이다. 고아 핀으로 간 간선은 순번을 그대로 둔다(9.4절 — 재결합 때 다시 매긴다).
6. 고아 핀의 `lastType`은 옛 핀 목록을 알면 그 핀 타입, 모르면(migration) `UnresolvedPinType`이다. 간선·값이 없는 고아 핀은 만들지 않고, 남은 고아 핀 중 간선·값이 없어진 것은 지운다(GI7).

UE는 이름과 방향이 함께 맞아야 핀을 잇고(`ED/BlueprintGraph/Private/K2Node.cpp:907`) 반대 방향 고아 핀이 같은 이름의 현재 핀과 함께 있을 수 있다. Rocks는 GI8(키 하나에 현재 핀·고아 핀 중 하나)을 지키므로 그 경우를 실패로 둔다(한계 — 키를 (키, 방향)으로 넓히는 것은 요구가 생길 때).

`SetNodeProperties`는 NG1의 `PinsDropped`·`RetainedShadowed` 거부 대신 이 루틴을 쓴다(사유 `PinsDropped`는 없앤다). op는 속성 바꾸기·고아 핀 추가·제거·재결합·순번 바꾸기이고 모두 같은 patch에 들어간다. 통지는 `NodePropertiesChanged`, `NodePinsChanged`, 바뀐 고아 핀마다 `RetainedPinChanged`, 순번이 바뀐 핀마다 `EdgeOrderChanged`다.

**migration 함수(NG3a).**

```cpp
struct NodeMigrationInput {
    const GraphDocument& document;
    const Graph& graph;
    const GraphNode& node;       // 지금까지의 단계를 거친 노드(키 재지정 반영)
    uint32 fromVersion;
    NamePool& names;
};

struct PinKeyRename { PinKey from; PinKey to; };

struct NodeMigrationResult {     // 호출 전에 노드의 종류·버전·속성 사본으로 채워 둔다
    NodeTypeId type;             // 바꾸면 1:1 교체(같은 도메인, `Graph.` 종류 아님)
    uint32 schemaVersion;
    PropertyRecord properties;
    Array<PinKeyRename> renames; // 이 단계의 키 재지정
};

using MigrateFn = Result<void> (*)(const NodeMigrationInput&, NodeMigrationResult&) noexcept;
```

- 한 단계는 버전을 반드시 올린다. 같은 종류면 `fromVersion` < 결과 버전 ≤ 서술 버전, 다른 종류면 1 ≤ 결과 버전 ≤ 그 종류의 서술 버전이다. 결과가 서술 버전에 닿을 때까지 그 종류의 `migrate`를 거듭 부르고, 단계 수 상한은 `MaxMigrationSteps`(64)다. 1:1 교체 뒤에는 새 종류의 `migrate`가 이어받는다.
- 등록 검사: `schemaVersion` > 1인 서술은 `migrate`가 있어야 한다(없으면 `InvalidArgument`). 사슬이 실제로 끝까지 가는지는 실행 때 본다.
- 결과 속성 Record의 schema는 결과 종류 서술의 `propertiesStruct`여야 하고 크기는 `MaxPropertyRecordBytes` 이하다. 1:1 교체 결과가 `SinglePerDocument`를 어기면 실패다.
- 단계마다의 재지정은 차례로 합성한다. 한 단계 안에서 같은 옛 키가 두 번 나오거나 키가 None이면 실패다. 최종 키 재지정과 최종 핀 목록으로 핀 재정리 루틴을 한 번 돈다.
- 함수는 상태 없이 결정적이어야 한다(같은 입력 → 같은 결과). 그래야 에디터와 쿠커의 결과가 같다.
- 그 밖의 실패: `Disabled` 노드인데 결과 종류에 비활성 허용이 없음(`EnableState`), 결과 노드의 동적 핀을 계산할 수 없음(`PropertiesSchema`).
- 입력 기본값의 값 변환(핀 타입이 바뀐 경우)은 NG3에서 하지 않는다. 값은 키만 따라가고, 타입이 맞지 않는 값은 도메인 검증이 보고한다(한계 — 요구 시 `NodeMigrationResult`에 기본값 목록을 더한다).

**migration 적용(NG3a).**
- `GraphMutator::Migrate(source, GraphMigrationReport& report)`: 서술 버전보다 낮은 노드를 그래프 순·노드 순으로 찾아 노드마다 계획하고 바로 적용한다. 다음 노드는 앞 노드를 적용한 상태에서 계획한다(양 끝이 모두 옮겨지는 간선, 여러 노드가 같은 `SinglePerDocument` 종류로 바뀌는 경우 — 먼저 온 노드가 이긴다 — 가 결정적이다). 계획·적용이 실패한 노드(함수 오류·버전 규칙·schema·핀 재정리 실패·단계 상한)는 그 노드의 op만 되돌려 옛 버전으로 두고 보고서에 사유를 적는다. revision을 하나 올리고 원인 Migration 통지 묶음을 보낸다. undo 이력에 넣지 않으므로 그 전에 받은 patch는 모두 낡은 것이 된다(`StaleHandle`). 옮길 노드가 없으면 아무것도 하지 않는다(revision 그대로). bracket이 열려 있으면 `Busy`다.
- `MigrateGraphDocument(document, layout, registry, report)`: 같은 일을 하는 자유 함수다. 내부에서 임시 `GraphMutator`(새 id를 발급하지 않으므로 늘 실패하는 내부 발급기)를 만들어 `Migrate`를 부르므로 구현은 하나다. 쿠커·headless 열기·commandlet이 쓴다. 그래서 에디터와 쿠커의 결과 바이트·hash가 같다(GT7).
- 보고서 항목: (그래프, 노드, 옛 종류·버전, 새 종류·버전, 만든 고아 핀 수, 실패 사유 — 성공이면 `None`). 진단 변환은 `GraphDiagnostic`(GD19) 정보·오류 수준이다.
- **migration 전 노드.** 서술 버전보다 낮은 노드는 핀 목록을 모르는 노드다. GI13은 그 노드의 속성 schema·비활성 허용·동적 핀을 보지 않고, 나머지 불변식 검사는 알 수 없는 종류와 같게 한다(6.3절 GI13 개정). 등록표의 핀 목록 계산(`CollectPins`)도 버전이 서술과 다르면 `NotFound`다. 핀·서술이 필요한 명령(그 노드 쪽 끝의 `Connect`·변환 삽입, `SetNodeProperties`, `SetInputDefault`, `SetEnableState`, `ReorderEdges`)은 `TypeVersion`으로 거부한다. `RemoveNodes`·`Disconnect`·`ResetInputDefault`(핀 목록 없이 값만 지운다 — 알 수 없는 종류와 같다)·`SetLayout`·`MoveNodesToGraph`·`RemoveRetainedPin`과 `InsertEdge`(핀 모르는 끝은 방향만 본다 — 9.4절)는 된다.
- **붙여넣기.** 조각의 옛 버전 노드는 NG3에서도 진단과 함께 뺀다(`TypeVersion`). 붙여넣기 migration은 문서 밖 노드에 migration 함수를 부를 문맥(`NodeMigrationInput`의 문서·그래프)이 없어서다. 요구가 생기면 붙여넣을 그래프를 문맥으로 하는 계획 도우미를 더한다(9절 6단계·9.4절 개정).
- `MigrateNode`는 공개 명령이 아니다. `Migrate` 내부 op(노드 바꾸기 + 그 노드 간선 바꾸기)이고 RPC·붙여넣기 목록에 나오지 않는다(8.1절 표 개정).

**등록표 변경과 다시 읽기(NG3b).**
- 등록표는 NG0대로 소유 lane 하나가 쓴다. `AddListener`·`RemoveListener`로 `NodeTypeRegistryListener::OnNodeTypesChanged(Span<const NodeTypeKey>)`를 받는다(`NodeTypeKey` = 도메인·종류). 노드 종류 등록·해제 때 보낸다.
- 해제 순서(10절): 조회에서 빼기 → 통지 → lease 놓기. 다른 스레드는 서술을 쥐지 않는다(도메인 컴파일은 필요한 정보를 복사해 넘긴다). 여러 스레드 조회가 필요해지면 컴파일 조정자(머티리얼 MS1) 착수 때 정한다.
- `GraphMutator::RefreshNodeTypes(source, Span<const NodeTypeKey>)`: 문서 소유자가 등록표 통지를 받아 부른다(연결은 NG4 lane 몫). 등록표는 이미 바뀌었으므로 bracket이 열려 있거나 통지 콜백 안이면 키를 쌓아 두었다가 bracket이 끝날 때(확정·취소)·통지가 끝날 때 반영한다. 문서 도메인의 종류만 본다. 문서를 전체 검사한다(종류 하나의 변경이 여러 노드에 닿으므로). 해제돼 자리 노드가 되거나 서술 버전이 올라 migration 전 노드가 되면 늘 통과한다. 다시 등록된 종류의 버전이 문서 노드보다 낮거나, 같은 버전인데 핀이 달라 검사에 실패하면 `TypeMismatch`를 돌려주고, mutator는 `Reload` 전까지 명령·undo·redo·bracket·migration을 `DependencyFailed`로 거부한다. 검사·미루기 중 할당 실패도 같다(검사하지 못한 문서를 받지 않는다). 통과하면 종류마다 `NodeTypesChanged` 하나(종류 Name은 `GraphChange::pin` 칸)를 원인 Registry로 보낸다. revision·문서 바이트는 그대로다. 목록의 종류가 지금 등록돼 있으면(재등록) undo 이력을 낡게 한다 — patch 안에만 남은 그 종류 노드(지운 노드)가 바뀐 핀으로 되살아나 불변식을 깨지 않게 한다. 해제만 있으면 이력은 그대로다(자리 노드는 검사가 느슨하다).
- `GraphMutator::Reload(source, GraphDocument&&, GraphDocumentLayout&&)`: 새 내용의 도메인이 같아야 하고(`TypeMismatch`), 넘긴 문서의 할당자를 그대로 쓰며(mutator보다 오래 살아야 한다), 전체 검사와 id·간선 색인 구축을 새 내용에서 먼저 한 뒤 통과하면 바꿔 넣는다(원자적). revision을 하나 올리고 이력을 낡게 하며, `DependencyFailed` 상태를 푼다. 원인 Reload 묶음에 `DocumentReloaded` 하나를 보낸다. 검사·할당 실패면 옛 내용이 그대로다. bracket이 열려 있거나 통지 콜백 안이면 `Busy`다(`Migrate`도 같다). migration은 따로 부른다(`Reload` 뒤 `Migrate`).
- 통지 원인에 Migration·Reload·Registry, 통지 종류에 `NodeTypesChanged`·`DocumentReloaded`를 더한다(8.3절).

**하위 그래프 경계 노드(NG3c).** GD25의 세 공통 종류 `Graph.Input`·`Graph.Output`·`Graph.Collapsed`를 등록표가 모든 도메인에 등록한다. 경계 핀 목록은 노드 속성이 아니라 하위 그래프 자체의 필드(그래프 인터페이스)다. 세 종류 모두 속성이 없다.

```cpp
ROCKS_ENUM() enum class PinKind : uint8 { Data, Flow, Structural };   // NodeTypeDescriptor.h에서 옮긴다
ROCKS_STRUCT() struct GraphBoundaryPin {
    Name key;            // 경계 핀 키(Collapsed 노드와 경계 노드의 핀 키)
    PinKind kind;        // Data·Flow
    Name type;           // 도메인 타입 표의 타입 이름(PinTypeInfo 이름). 표에 없으면 UnresolvedPinType
    uint16 maxEdges;     // 받는 쪽 입력 핀의 간선 상한(1 이상)
};
// Graph에 더하는 필드(6.1절):
//   Guid ownerNode;                       // Collapsed: 이 그래프를 가리키는 부모의 Graph.Collapsed 노드, 그 밖 0
//   Array<GraphBoundaryPin> inputs;       // Collapsed: 경계 입력(바깥 → 안), 그 밖 빈 목록
//   Array<GraphBoundaryPin> outputs;      // Collapsed: 경계 출력(안 → 바깥), 그 밖 빈 목록
```

- 핀: `Graph.Input`은 자기 그래프 `inputs`마다 출력 핀(무제한), `Graph.Output`은 `outputs`마다 입력 핀(`maxEdges`)이다. `Graph.Collapsed`는 `ownerNode`가 자기인 그래프를 찾아 그 `inputs`를 입력 핀(`maxEdges`)으로, `outputs`를 출력 핀(무제한)으로 갖는다. 표시 이름은 키 원문이다(Name pool이 소유 — 수명 문제가 없다). 값 정책은 Editable이다(Collapsed 입력 기본값의 뜻은 도메인 컴파일이 정한다). 등록표 `CollectPins`가 세 종류를 직접 계산한다(타입 이름 → 색인은 문서 도메인 schema 표).
- 이렇게 둔 이유: 핀 목록을 셀 때 Record를 디코드하지 않으므로 ObjectRegistry·소유 스레드가 필요 없고(검증·쿠커·붙여넣기 어디서나 같다), 디코드한 임시 문자열 수명 문제도 없다. 하위 그래프의 인터페이스가 그래프에 있으므로 경계 노드를 지우고 다시 만들어도 인터페이스가 남는다(MetaSound graph interface와 같은 방향). 타입을 이름으로 두어 도메인 타입 표 순서가 바뀌어도 문서가 안정하다.
- 버린 대안: 경계 노드 속성 Record(+ ObjectRegistry 디코드 또는 전용 decoder) — 디코드 문맥·스레드 제약, 리플렉션 wire를 손으로 따라가는 decoder의 유지 비용. `Graph.Collapsed` 속성에 대상 GraphId — 같은 디코드 문제이고, 두 쪽 참조가 어긋날 수 있다(지금은 `ownerNode` 한쪽만 참조).
- 경계 노드 capability: `Graph.Input`·`Graph.Output`은 삭제·복제·비활성 불가다(UE도 tunnel 그래프 안 경계 노드를 지울 수 없다 — `ED/BlueprintGraph/Private/K2Node_Tunnel.cpp:132–149`). `Graph.Collapsed`는 삭제 가능·복제 불가(붙여넣기 제외)·비활성 불가다.
- 불변식(6.3절 표에 더함):
  - GI15: Collapsed 역할 그래프에는 `Graph.Input`·`Graph.Output`이 각각 정확히 하나 있고, 그 밖의 그래프에는 없다. `inputs`·`outputs` 키는 None이 아니고 둘을 합쳐 겹치지 않으며(`Graph.Collapsed` 핀 목록이 되므로) 합계는 `MaxPinsPerNode` 이하, kind는 Data·Flow, `maxEdges`는 1 이상이다. Collapsed가 아닌 그래프의 `ownerNode`는 0이고 경계 목록은 비어 있다.
  - GI16: Collapsed 역할 그래프의 `ownerNode`는 다른 그래프에 있는 `Graph.Collapsed` 노드다. `Graph.Collapsed` 노드마다 그것을 `ownerNode`로 갖는 그래프가 정확히 하나 있다. 가리킴 관계에 순환이 없다(Collapsed가 아닌 그래프에서 내려가는 나무). `Graph.Collapsed`는 어느 역할의 그래프에나 둘 수 있다.
- 명령:
  - `AddGraph`는 Collapsed 역할일 때 `ownerNode`(0 아님)를 받는다. 경계 목록은 빈 채로 시작한다.
  - 새 공통 명령 `SetGraphBoundary(graph, inputs, outputs)`: Collapsed 그래프의 경계 목록을 바꾼다. 같은 명령 안에서 그 그래프의 `Graph.Input`·`Graph.Output`과 부모 `Graph.Collapsed`를 핀 재정리한다(사라진 경계 핀에 닿은 간선·값은 고아 핀으로). 경계 목록은 그래프 머리 op 하나로 바뀌고, 그 op를 적용할 때(undo·redo 포함) `GraphBoundaryChanged`와 세 노드의 `NodePinsChanged`를 기록한다.
  - `RemoveNodes`가 `Graph.Collapsed`를 지우면 그 노드를 `ownerNode`로 갖는 하위 그래프(와 그 안의 하위 그래프)를 같은 patch에서 먼저 지운다(UE `UK2Node_Composite::DestroyNode` — `ED/BlueprintGraph/Private/K2Node_Composite.cpp:73–84`). 연쇄로 지우는 그래프 안의 경계 노드는 삭제 불가 검사에서 빼고, 그 밖의 노드에 삭제 불가가 있으면 `CannotDelete`다.
  - `RemoveGraph`는 `ownerNode`가 문서에 있는 그래프를 거부한다(`GraphReferenced` — 노드를 지워야 한다). 지우는 그래프 안에 `Graph.Collapsed`가 있으면 그 하위 그래프도 연쇄로 지운다(같은 삭제 불가 규칙).
  - `AddNode`·`InsertNode`: `Graph.Input`·`Graph.Output`은 Collapsed 그래프에만 넣을 수 있다(`BoundaryNode`). `Graph.Collapsed`는 그 노드를 `ownerNode`로 갖는 그래프가 이미 있고(같은 목록의 앞 `AddGraph`), 넣는 그래프가 그 하위 그래프 나무 안이 아니어야 한다(`WouldCreateCycle`).
  - `MoveNodesToGraph`는 경계 노드를 옮기지 않는다(`BoundaryNode`). `Graph.Collapsed`를 옮길 때 대상이 자기 하위 그래프 나무 안이면 거부한다(`WouldCreateCycle`).
  - GI15·GI16의 "정확히 하나"는 명령 목록 끝에 검사한다(`AddGraph` → 경계 노드 `AddNode` → 부모 `Graph.Collapsed` `AddNode` 목록이 중간 상태를 거치므로 — `InsertNode`의 GI7 목록 끝 검사와 같은 방식). 그래프·경계 노드·`Graph.Collapsed`를 건드린 목록만 검사한다. 어기면 목록 전체가 실패한다(`BoundaryNode`).
- **접기·펼치기 도우미.** `GraphFold.h`의 두 함수가 공통 명령 목록을 만든다(8.1절 접기 행).
  - `BuildCollapseCommands(view, layout, graph, nodes, ids, out)`: 경계를 건너는 간선을 정규 순서로 모아 안쪽 핀마다 경계 핀 하나(키 `In<n>`·`Out<n>`, 안쪽 핀의 kind·타입 이름·`maxEdges`)를 만든다. 순서는 건너는 간선 `Disconnect` → `AddGraph`(Collapsed, `ownerNode` = 새 Collapsed 노드 id) → `SetGraphBoundary` → 경계 노드 `AddNode` → `MoveNodesToGraph` → `Graph.Collapsed` `AddNode`(옮긴 노드 배치의 중심) → 바깥·안쪽 `Connect`(`requireMake`)다. 경계 노드 배치는 옮긴 노드 경계 상자의 왼쪽·오른쪽이다.
  - `BuildExpandCommands(view, graph, collapsedNode, out, report)`: 반대 순서다. `Graph.Collapsed`·경계 노드에 닿은 간선 `Disconnect` → 경계 노드가 아닌 노드 `MoveNodesToGraph`(부모로) → `Graph.Collapsed` `RemoveNodes`(경계 노드만 남은 하위 그래프가 연쇄로 지워진다) → 경계 핀마다 바깥 끝 × 안쪽 끝 `Connect`(`requireMake`) → `Graph.Collapsed` 입력 기본값은 바깥 간선이 없던 경계 핀의 안쪽 끝 입력 핀에 `SetInputDefault`(Hidden이면 버리고 진단)다.
  - 다시 잇는 `Connect`는 `requireMake`다(schema의 Replace·Deny 판정이 접기 전에 있던 연결을 바꾸지 않게 — 거부되면 목록 전체 실패).
  - 한계: 순번 있는 출력 핀을 건너는 간선은 다시 이을 때 끝에 붙는다(왕복 시험에서 뺀다). 하위 그래프 배치 좌표는 부모 좌표로 옮기지 않는다(화면 단계 NG7에서 정한다). 접기 대상에 경계 노드가 있으면 거부한다.
  - 머티리얼 MS1 컴파일이 Collapsed 그래프를 받는지는 MS1 착수 때 정한다(받지 않으면 도메인 검증 오류 — 공통 펼침 도우미는 그때).
- **기존 데이터.** 노드 없는 Collapsed 그래프를 쓰는 시험 표본·텍스트 문서는 NG3c에서 GI15·GI16에 맞게 고친다. 텍스트 codec(9.4절)과 의미 바이트(8.4절)는 `Graph`의 새 필드를 쓴다.
- **붙여넣기.** 하위 그래프 복제는 조각 형식에 그래프 목록이 필요하므로 NG7(접기 화면)과 함께 한다. NG3c에서는 세 종류 모두 복제 불가라서 복사 대상에서 빠지고, 붙여넣기에서도 진단과 함께 빠진다(`CannotDuplicate`). 그래서 GI15·GI16이 붙여넣기로 깨지지 않는다.
- 의미 바이트: 하위 그래프를 펼치지 않고 그대로 넣는다. 펼친 뜻은 도메인 컴파일이 정한다(공통 펼침 도우미는 도메인 요구 때).

**시험.**
- NG3a: 핀 재정리 규칙마다 양성·음성(간선 남김·고아 핀 생성·값 이동·재결합·방향 충돌·`maxEdges`·Hidden·순번 다시 매기기·GI7 정리), `SetNodeProperties` 왕복 undo·redo 바이트 동일, migration 사슬(여러 단계, 키 재지정 합성, 1:1 교체 후 이어받기, 단계 상한, 버전 규칙 위반, 속성 schema 틀림, `SinglePerDocument`), 실패 노드 보존과 보고, migration 전 노드 명령 거부, 에디터·자유 함수 결과 바이트·hash 동일(GT7), 이력 낡음, 할당 실패 주입.
- NG3b: 해제 → 자리 노드 표시·바이트·revision 불변·이력 유지, 재등록 복원·이력 낡음, 버전 올린 재등록 → migration 전 노드, 같은 버전 핀 바뀐·버전 낮춘 재등록 → `TypeMismatch`·`DependencyFailed`·`Reload`로 복구, `Reload` 성공·도메인 다름·검사 실패·`Busy`, bracket 중 등록 변경을 미뤘다가 끝날 때 반영, 통지 원인(GT8).
- NG3c: GI15·GI16 양성·음성, `SetGraphBoundary`의 세 노드 핀 재정리와 undo·redo 통지, 하위 그래프 연쇄 삭제와 undo, `GraphReferenced`, 접기·펼치기 왕복(펼친 뒤 의미 바이트가 접기 전과 같음), 무작위 명령열에 접기·펼치기 섞기, 붙여넣기 제외 진단.
