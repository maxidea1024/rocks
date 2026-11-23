# 노드 그래프(NodeGraph) 설계

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

// NG0은 Domain·PinTypes·NormalizeSemanticNode만 둔다. 나머지는 쓰는 단계에서 더한다
// (CanConnect·Validate — NG1, CanPasteNode·AllowExternalEdgeReconnect — NG2, CreateDefaultNodes — NG1,
// CollectMenuEntries·BuildDropCommands — NG5).

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

- 등록은 `NodeTypeRegistry::RegisterSchema(const GraphSchema&, ModuleLease)`로 한다. 한 `DomainId`에 schema는 하나다. 노드 종류 등록(`RegisterNodeType`)보다 먼저 한다(핀 타입 색인 검사에 타입 표가 필요하다).
- `CanConnect`는 순환 검사를 포함할 수 있다. 공통 층은 순환 검사 도우미를 제공한다. 입력 쪽에서 출력 쪽으로 도달 가능한지를 재경유를 포함해 파생 인접 색인으로 본다. 비용은 도달 범위에 비례한다. 화면은 drag 동안 결과를 cache한다.
- UE의 promotion(`MAKE_WITH_PROMOTION`, 연산자 노드 타입 승격)은 도메인이 wildcard 해석(GD10)으로 표현한다. 별도 판정 값을 두지 않는다. 파라미터 승격("Promote to Parameter")은 메뉴 항목의 명령 목록으로 만든다.
- 메뉴 항목은 데이터다: `NodeMenuEntry{표시 이름, 분류 경로, 키워드, 도움말, 순위 가중치, 생성 명령 목록}`. 실행 함수 포인터를 두지 않는다. 항목을 고르면 그 명령 목록을 `GraphMutator`에 보낸다. 그래서 메뉴 항목도 headless로 시험한다.
- split pin(K2 전용)은 두지 않는다. 구조체 성분 분해가 필요한 도메인은 Break·Make 노드로 한다(머티리얼 Make/Break Attributes와 같다).
