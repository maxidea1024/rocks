#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Math/Color.h>
#include <Core/Math/Vector2.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>

// 노드 그래프 문서와 배치 데이터의 리플렉션 타입이다(Docs/Design/NodeGraph.ko.md 6.1절, UE 대응은
// 4절 표).
//
// 의미 데이터(GraphDocument)와 배치 데이터(GraphDocumentLayout)는 별도 struct다(GD8). 도메인 자산이
// 둘을 별도 리플렉션 필드로 갖고, 배치를 쿠킹에서 빼려면 배치 필드에 EditorOnly를 붙인다(9.1절).
//
// 이름 키(DomainId·NodeTypeId·PinKey)는 객체 Registry NamePool의 토큰이다. 같음은 토큰으로
// 비교하고, 정규 순서와 의미 바이트는 원문으로 정한다(GraphNameOrder.h).
//
// 사용 예(노드 하나짜리 문서):
//   GraphDocument document(allocator);
//   document.domain = names.TryIntern("Material").Value();
//   Graph main(allocator);
//   main.id = mainId;
//   GraphNode node(allocator);
//   node.id = nodeId;
//   node.type = names.TryIntern("Material.Multiply").Value();
//   node.typeSchemaVersion = 1;
//   main.nodes.TryPushBack(Move(node));
//   document.mainGraph = mainId;
//   document.graphs.TryPushBack(Move(main));
//   // 정규 순서·불변식은 CanonicalizeGraphDocument·ValidateGraphDocument(GraphValidation.h)가
//   맞추고 본다.
//
// 수명: 배열·문자열 멤버는 생성자에 준 allocator를 쓴다. allocator는 값보다 오래 살아야 한다.
// 스레드: 값 타입이다(동기화 없음). 문서는 소유 lane 하나에서만 읽고 바꾼다(GD24).
namespace rocks::nodegraph {

using GraphId = Guid;
using NodeId = Guid;
using CommentId = Guid;
using DomainId = Name;
using NodeTypeId = Name;
using PinKey = Name;

// 도메인 타입 표(GraphSchema::PinTypes)의 색인이다.
using PinTypeId = uint32;

// 지금 공통 형식 버전이다(GraphDocument::documentFormatVersion, GI12). 형식을 바꾸면 올리고 읽는 쪽
// 변환을 더한다.
inline constexpr uint32 GraphDocumentFormatVersion = 1;

// 속성 Record 하나의 최대 바이트다. 리플렉션 배열 상한(MaxElements 최대 1,048,576)과 같다.
// PropertyRecord::bytes의 MaxElements와 맞춘다.
inline constexpr uint32 MaxPropertyRecordBytes = 1048576;

// 그래프 하나의 최대 노드 수다(Graph::nodes의 리플렉션 기본 상한 65,536과 같다).
inline constexpr uint32 MaxGraphNodes = 65536;

// 그래프 하나의 최대 간선 수다. Graph::edges의 MaxElements와 맞춘다(노드는 리플렉션 기본 상한
// 65,536).
inline constexpr uint32 MaxGraphEdges = 262144;

// 그래프의 역할이다(6.1절).
ROCKS_ENUM()
enum class GraphRole : uint8 {
    // 문서의 주 그래프다. 문서에 하나다(GI12).
    Main = 0,
    // 접힌 하위 그래프다(GD25 CollapsedGraph 노드가 가리킨다).
    Collapsed = 1,
    // 도메인이 정의한 역할이다. 번호는 Graph::domainRole이다(예: 상태 기계의 상태마다 하위 그래프).
    DomainDefined = 2
};

// 노드 활성 상태다(6.5절). Disabled는 비활성 허용 capability가 있는 종류만 쓴다(GI13).
ROCKS_ENUM()
enum class NodeEnableState : uint8 {
    Enabled = 0,
    Disabled = 1
};

// 핀 방향이다. 간선은 출력 → 입력이다(GI2).
ROCKS_ENUM()
enum class PinDirection : uint8 {
    Input = 0,
    Output = 1
};

// 노드 속성·핀 값의 리플렉션 Record 바이트다(GD23): 비어 있거나 Record 하나(header schema = 값
// struct ID, schema version = 저장 당시 struct 버전 — Serialization/Record.h). 해석은 도메인과
// NodeGraphTools가 한다. 모르는 노드도 이 바이트를 그대로 보존한다(GD14).
ROCKS_STRUCT()
struct PropertyRecord {
    ROCKS_GENERATED_BODY()

    explicit PropertyRecord(Allocator& allocator) noexcept : bytes(allocator) {
    }

    ROCKS_PROPERTY(MaxElements = 1048576) Array<uint8> bytes;
};

// 입력 기본값 override 하나다(기본값과 다른 입력 값만 저장한다 — GD3).
ROCKS_STRUCT()
struct PinValueEntry {
    ROCKS_GENERATED_BODY()

    explicit PinValueEntry(Allocator& allocator) noexcept : value(allocator) {
    }

    ROCKS_PROPERTY() Name pin;
    ROCKS_PROPERTY() PropertyRecord value;
};

// 고아 핀이다(6.4절): 서술에서 사라졌지만 간선이나 사용자 값이 남아 보존한 핀. 연결할 수 없고
// 끊기·삭제만 된다.
ROCKS_STRUCT()
struct RetainedPin {
    ROCKS_GENERATED_BODY()

    explicit RetainedPin(Allocator& allocator) noexcept : value(allocator) {
    }

    ROCKS_PROPERTY() Name key;
    ROCKS_PROPERTY() PinDirection direction = PinDirection::Input;
    // 사라지기 전 마지막 핀 타입(PinTypeId)이다.
    ROCKS_PROPERTY() uint32 lastType = 0;
    // 사용자 값이다(입력 핀만, 없으면 비어 있다).
    ROCKS_PROPERTY() PropertyRecord value;
};

// 노드 인스턴스다. 핀 배열은 저장하지 않는다 — 핀 목록은 서술과 속성에서 유도한다(GD3).
ROCKS_STRUCT()
struct GraphNode {
    ROCKS_GENERATED_BODY()

    explicit GraphNode(Allocator& allocator) noexcept
        : properties(allocator), inputDefaults(allocator), retainedPins(allocator) {
    }

    ROCKS_PROPERTY() Guid id;
    ROCKS_PROPERTY() Name type;
    // 저장 당시 서술 버전이다(GD13). 1 이상.
    ROCKS_PROPERTY() uint32 typeSchemaVersion = 0;
    ROCKS_PROPERTY() PropertyRecord properties;
    // PinKey 순, 중복 없음(GI5).
    ROCKS_PROPERTY() Array<PinValueEntry> inputDefaults;
    // PinKey 순, 중복 없음(GI5).
    ROCKS_PROPERTY() Array<RetainedPin> retainedPins;
    ROCKS_PROPERTY() NodeEnableState enableState = NodeEnableState::Enabled;
};

// 간선 하나다: (출력 노드, 출력 핀) → (입력 노드, 입력 핀).
ROCKS_STRUCT()
struct GraphEdge {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Guid fromNode;
    ROCKS_PROPERTY() Name fromPin;
    ROCKS_PROPERTY() Guid toNode;
    ROCKS_PROPERTY() Name toPin;
    // ordered 출력 핀의 간선 순번(0부터 빈틈없음), 그 밖에는 0이다(GD4, GI5).
    ROCKS_PROPERTY() uint32 order = 0;
};

// 노드·간선 묶음 하나다.
ROCKS_STRUCT()
struct Graph {
    ROCKS_GENERATED_BODY()

    explicit Graph(Allocator& allocator) noexcept : nodes(allocator), edges(allocator) {
    }

    ROCKS_PROPERTY() Guid id;
    ROCKS_PROPERTY() GraphRole role = GraphRole::Main;
    // role이 DomainDefined일 때 도메인 정의 역할 번호, 그 밖에는 0이다(GI12).
    ROCKS_PROPERTY() uint32 domainRole = 0;
    // NodeId 순, 중복 없음(GI5).
    ROCKS_PROPERTY() Array<GraphNode> nodes;
    // (입력 NodeId, 입력 PinKey, 출력 NodeId, 출력 PinKey) 순, 중복 없음(GI4·GI5).
    ROCKS_PROPERTY(MaxElements = 262144) Array<GraphEdge> edges;
};

// 자산 하나의 그래프 묶음(주 그래프 + 하위 그래프)의 의미 데이터다.
ROCKS_STRUCT()
struct GraphDocument {
    ROCKS_GENERATED_BODY()

    explicit GraphDocument(Allocator& allocator) noexcept : graphs(allocator) {
    }

    // 도메인 schema 식별이다(예: "Material").
    ROCKS_PROPERTY() Name domain;
    ROCKS_PROPERTY() uint32 documentFormatVersion = GraphDocumentFormatVersion;
    ROCKS_PROPERTY() Guid mainGraph;
    // GraphId 순, 중복 없음(GI5).
    ROCKS_PROPERTY() Array<Graph> graphs;
};

// 노드 하나의 배치다. 위치·크기는 캔버스 단위이고 크기 0은 자동 크기다.
ROCKS_STRUCT()
struct NodeLayout {
    ROCKS_GENERATED_BODY()

    explicit NodeLayout(Allocator& allocator) noexcept : bubble(allocator) {
    }

    ROCKS_PROPERTY() Guid node;
    ROCKS_PROPERTY() Vec2 position;
    ROCKS_PROPERTY() Vec2 size;
    ROCKS_PROPERTY() bool collapsed = false;
    // 고급 핀 묶음을 펼쳐 보인다(UE AdvancedPinDisplay).
    ROCKS_PROPERTY() bool showAdvanced = false;
    // 말풍선 문구다(UE NodeComment).
    ROCKS_PROPERTY() String bubble;
    ROCKS_PROPERTY() bool bubbleVisible = false;
};

// 주석 상자다. 안의 노드 목록은 저장하지 않는다(GD18).
ROCKS_STRUCT()
struct CommentBox {
    ROCKS_GENERATED_BODY()

    explicit CommentBox(Allocator& allocator) noexcept : text(allocator) {
    }

    ROCKS_PROPERTY() Guid id;
    ROCKS_PROPERTY() Vec2 position;
    ROCKS_PROPERTY() Vec2 size;
    ROCKS_PROPERTY() String text;
    ROCKS_PROPERTY() LinearColor color;
    ROCKS_PROPERTY() uint32 fontSize = 0;
};

// 번호 북마크다(UE QuickJump: 번호 → 보기 중심·zoom).
ROCKS_STRUCT()
struct Bookmark {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() uint32 number = 0;
    ROCKS_PROPERTY() Vec2 viewCenter;
    ROCKS_PROPERTY() float zoom = 1.0f;
};

// 그래프 하나의 배치다.
ROCKS_STRUCT()
struct GraphLayout {
    ROCKS_GENERATED_BODY()

    explicit GraphLayout(Allocator& allocator) noexcept
        : title(allocator), nodes(allocator), comments(allocator), bookmarks(allocator) {
    }

    ROCKS_PROPERTY() Guid graph;
    // 하위 그래프 표시 이름이다. 표시에만 쓰고 의미 비교에 넣지 않는다(6.1절).
    ROCKS_PROPERTY() String title;
    // NodeId 순, 그 그래프의 노드와 1:1(GI6).
    ROCKS_PROPERTY() Array<NodeLayout> nodes;
    // id 순, 중복 없음.
    ROCKS_PROPERTY() Array<CommentBox> comments;
    // 번호 순, 중복 없음.
    ROCKS_PROPERTY() Array<Bookmark> bookmarks;
};

// 문서의 배치 데이터다(GD8).
ROCKS_STRUCT()
struct GraphDocumentLayout {
    ROCKS_GENERATED_BODY()

    explicit GraphDocumentLayout(Allocator& allocator) noexcept : graphs(allocator) {
    }

    // GraphId 순, 문서 그래프와 1:1(GI6).
    ROCKS_PROPERTY() Array<GraphLayout> graphs;
};

} // namespace rocks::nodegraph
