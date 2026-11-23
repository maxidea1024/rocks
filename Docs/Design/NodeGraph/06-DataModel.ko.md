# 노드 그래프(NodeGraph) 설계

## 6. 데이터 모델

### 6.1 구조

```cpp
// 리플렉션 struct(ROCKS_STRUCT)로 저장한다. Object 포인터·ObjectHandle·TypeLease·InstancedStruct 없음(원칙 1).
namespace rocks::nodegraph {

using GraphId = Guid;   using NodeId = Guid;   using CommentId = Guid;
using DomainId = Name;  using NodeTypeId = Name;   using PinKey = Name;   // 객체 Registry NamePool의 토큰
using PinTypeId = uint32;                                                  // 도메인 타입 표 색인

struct PropertyRecord {                 // 리플렉션 Record 바이트(GD23): 비어 있거나 Record 하나
    Array<uint8> bytes;
};

struct GraphDocument {                  // 의미 데이터
    DomainId domain;                    // 도메인 schema 식별(예: "Material")
    uint32 documentFormatVersion;       // 공통 형식 버전(GraphDocumentFormatVersion)
    GraphId mainGraph;
    Array<Graph> graphs;                // GraphId 순 정렬(6.3절)
};

struct Graph {
    GraphId id;
    GraphRole role;                     // Main, Collapsed, DomainDefined
    uint32 domainRole;                  // DomainDefined일 때 도메인 정의 역할 번호, 그 밖에는 0
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

struct PinValueEntry { PinKey pin; PropertyRecord value; };
struct RetainedPin   { PinKey key; PinDirection direction; PinTypeId lastType; PropertyRecord value; };

struct GraphEdge {
    NodeId fromNode; PinKey fromPin;
    NodeId toNode;   PinKey toPin;
    uint32 order;                       // ordered 출력 핀의 간선 순번, 아니면 0
};

struct GraphDocumentLayout {            // 배치 데이터(GD8), 도메인 자산이 별도 필드로 가진다
    Array<GraphLayout> graphs;          // GraphId 순, 문서 그래프와 1:1(GI6)
};

struct GraphLayout {
    GraphId graph;
    String title;                       // 하위 그래프 표시 이름(RenameGraph)
    Array<NodeLayout> nodes;            // NodeId 순, 그래프 노드와 1:1(GI6)
    Array<CommentBox> comments;         // id 순
    Array<Bookmark> bookmarks;          // 번호 순, 번호 중복 없음
};

struct NodeLayout { NodeId node; Vec2 position; Vec2 size; bool collapsed; bool showAdvanced; String bubble; bool bubbleVisible; };
struct CommentBox { CommentId id; Vec2 position; Vec2 size; String text; LinearColor color; uint32 fontSize; };
struct Bookmark   { uint32 number; Vec2 viewCenter; float zoom; };

}
```

- **이름 키.** `DomainId`·`NodeTypeId`·`PinKey`는 객체 Registry NamePool의 토큰(리플렉션 Name 필드)이다. 같음은 토큰으로 비교한다. 토큰 값은 실행마다 다르므로 정규 순서와 의미 바이트는 원문으로 정한다. 순서는 Name 같음 규칙과 일치하게 "ASCII 소문자로 바꾼 base 바이트 → 번호"이고 None이 맨 앞이다. 그래서 정규화·검증·의미 바이트 함수는 NamePool을 받는다.
- **배치 필드.** 위치·크기는 캔버스 단위 float(`Vec2`)이고 크기 0은 자동 크기다. 말풍선(`bubble`·`bubbleVisible`)은 UE `NodeComment`·`bCommentBubbleVisible`, 주석 상자의 색·글자 크기는 UE `CommentColor`(`FLinearColor`)·`FontSize`에 대응한다. 북마크는 UE QuickJump(번호 → 보기 위치·zoom)다.
- **그래프 이름**(`title`)은 배치 데이터다. 하위 그래프는 GraphId로 식별하고 이름은 표시에만 쓰므로 의미 비교·컴파일에 넣지 않는다.

- `PinValueEntry`는 `{PinKey, PropertyRecord}`다. 값 struct는 도메인 타입 표가 정한다. 자산 참조는 `AssetId`로 저장한다(그래서 [에셋 시스템](../AssetSystem/06-AssetTypes.ko.md) 6.2절 의존 종류로 보고된다).
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
    PinTypeId type;             // 도메인 타입 표 색인. wildcard면 UnresolvedPinType
    uint16 maxEdges;            // 입력 기본 1, 출력 기본 무제한(UnlimitedEdges)
    bool ordered;               // 출력 핀만: 간선 순번이 의미 있음(GD4)
    bool advanced;              // 고급 표시 묶음
    PinValuePolicy valuePolicy; // 기본값 편집 가능·읽기 전용·숨김
    StringView displayName;     // 영어 표시 이름(키와 독립)
};

struct NodeTypeDescriptor {
    NodeTypeId type;
    uint32 schemaVersion;                            // 1 이상
    Guid propertiesStruct;                           // 속성 struct ID(Record schema). 0이면 속성 없음
    Span<const PinDescriptor> staticPins;
    DynamicPinsFn dynamicPins;                       // (노드, 문서 읽기 view) → 추가 핀(선택)
    MigrateFn migrate;                               // 옛 버전 → 현재(선택, GD13 — NG3에서 더한다)
    Span<const PassThroughPair> passThrough;         // 비활성 때 통과할 입력 → 출력 쌍(선택, 6.5절)
    NodeMenuInfo menu;                               // 분류·키워드·도움말(영어 StringView)
    NodeCapabilities capabilities;                   // 복제·삭제 가능, 문서당 하나, 비활성 허용(비트)
};
```

- 핀 목록 = `staticPins` + `dynamicPins(...)`. 같은 키가 두 번 나오면 등록·검증 오류다.
- `dynamicPins`는 문서 읽기 view와 NamePool(동적 핀 키를 만든다)을 받는다. 동적 핀의 표시 이름은 핀 목록보다 오래 사는 data(서술 모듈 data 또는 노드 속성 바이트)를 가리킨다. `CollapsedGraph`가 하위 그래프 경계에서 핀을 유도해야 하기 때문이다(GD25). 이때 의존 관계(노드 → 참조 그래프)를 등록하고, 참조 그래프의 경계 노드가 바뀌면 그 노드의 핀 cache를 무효화한다.
- 핀 목록 cache는 노드 단위다. 속성이 바뀌거나 등록된 의존 그래프가 바뀔 때만 무효화한다.
- 표시 이름·도움말은 영어로 등록한다(런타임 메시지 규칙). 한도는 UiToolkit UD14·UD16의 상수를 쓴다(화면 단계 NG5에서 검사). 지역화 `Text`로 바꾸는 것은 화면이 한다.
- 서술의 `StringView`·`Span`이 가리키는 data는 서술을 등록한 모듈의 것이고, 등록 때 넘긴 module lease가 수명을 지킨다. 등록표는 핀·통과 쌍 배열을 소유 복사한다(계획 14.6절). 동적 핀의 표시 이름은 복사하지 않는다(위 `dynamicPins` 항목). NG1에서 핀 목록 cache를 두면 속성 변경 때 그 노드 cache를 함께 무효화하므로 dangling이 생기지 않는다.
- 등록 검사는 정적 핀과 동적 핀에 같은 핀 규칙을 쓴다: 키가 None이 아님, `maxEdges` ≥ 1, 방향·종류·값 정책이 정의된 값, `ordered`는 출력 핀만, 타입은 `UnresolvedPinType`이거나 도메인 타입 표 색인. 동적 핀은 `CollectPins`가 검사하고 위반이면 `InvalidArgument`다.
- **공통 노드 종류.** 등록표가 모든 도메인에 대해 직접 등록한다. NG0은 재경유 `Graph.Reroute`(입력 `In` 하나·`maxEdges` 1, 출력 `Out` 무제한, 둘 다 Data·`UnresolvedPinType`·값 정책 Hidden, 속성 없음)를 둔다. 재경유는 입력 기본값과 고아 핀을 두지 않는다(의미 바이트에서 접히면 사라지므로 — GI13·GI14가 거부한다). `Graph.Input`·`Graph.Output`·`Graph.Collapsed`(GD25)는 NG3에서 더한다. 도메인은 `Graph.` 접두 이름을 등록할 수 없다.

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
| GI11 | GraphId·NodeId·주석 id는 0이 아니다. 도메인·노드 종류·PinKey는 None이 아니다 |
| GI12 | `documentFormatVersion`이 지원 범위다. `mainGraph`가 문서에 있고 역할이 Main이며, Main 역할 그래프는 그것 하나다. Main·Collapsed 그래프의 `domainRole`은 0이다 |
| GI13 | 알려진 종류의 노드: `typeSchemaVersion`이 1 이상이고 서술 버전 이하다(낮으면 migration 대상 — NG3). 속성 Record의 schema가 서술의 `propertiesStruct`다(`propertiesStruct`가 0이면 속성이 비어 있다). `Disabled`면 서술에 비활성 허용이 있다. 재경유에는 고아 핀이 없다. 동적 핀을 계산할 수 있다(동적 핀 함수 오류·핀 규칙 위반이면 위반). 활성 상태·고아 핀 방향이 정의된 값이다 |
| GI14 | 알려진 종류의 노드: 입력 기본값의 키는 현재 입력 핀이고, 그 핀의 값 정책이 Hidden이 아니며, 값 Record는 비어 있지 않다 |

**알 수 없는 종류.** 등록되지 않은 종류의 노드(자리 노드, GD14)는 핀 목록이 없다. 그래서 그 노드 쪽 간선 끝의 GI2·GI3·GI5 순번·GI8 검사와 GI13·GI14를 하지 않는다. 그 밖(GI1·GI4·GI5 정렬·GI6·GI7·GI9·GI11)은 검사한다. 알려진 종류의 고아 핀 쪽 끝은 방향만 보고 `maxEdges`·순번은 보지 않는다(서술에서 사라진 핀이라 정보가 없다). 알 수 없는 종류의 고아 핀은 방향도 보지 않는다(그 노드의 핀 정보 전체를 모르므로). GI13을 어긴 알려진 종류의 노드(속성이 틀림, 동적 핀 계산 불가)도 핀 목록 모름으로 두고 알 수 없는 종류처럼 검사한다.

**잘못된 토큰.** 구조가 잘못된 Name 토큰(`Name::IsValid`가 거짓)은 검사를 오류로 끝내지 않고 GI11 위반으로 보고한다. 정규화·의미 바이트는 그런 토큰을 `InvalidArgument`로 거부한다.

**검사 결과.** 전체 검사는 위반을 모두 모아 돌려준다(목록 상한은 이름 있는 상수). 위반 하나는 (규칙, GraphId, NodeId, 관련 id, PinKey, 순번)이다. 진단 형식 `GraphDiagnostic`(GD19)으로의 변환은 도메인 검증과 함께 NG1에서 한다.

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
