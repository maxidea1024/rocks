#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <NodeGraph/GraphDocument.h>

// 그래프 공통 명령과 명령 목록이다(Docs/Design/NodeGraph 8.1·8.6절). 명령은 데이터다 — 메뉴
// 항목·RPC· headless 시험이 같은 목록을 만들어 GraphMutator::Execute에 넘긴다. 도메인 명령(변환
// 노드 끼우기 등)은 이 명령들의 목록이다.
//
// 사용 예(노드 둘을 더하고 잇기, undo 1개):
//   GraphCommandList list(allocator);
//   list.AddNode(mainGraph, multiply, multiplyLayout);
//   list.AddNode(mainGraph, constant, constantLayout);
//   list.Connect(mainGraph, edge);
//   GraphCommandFailure failure;
//   auto patch = mutator.Execute(source, mutator.Revision(), list, &failure);
//
// 수명: 목록은 넘긴 값을 복사해 가진다. allocator는 목록보다 오래 살아야 한다.
namespace rocks::nodegraph {

// 명령 종류다(8.1절 표). MigrateNode는 NG3에서 더한다.
enum class GraphCommandKind : uint8 {
    AddNode,
    RemoveNodes,
    Connect,
    Disconnect,
    ReorderEdges,
    SetNodeProperties,
    SetInputDefault,
    ResetInputDefault,
    SetLayout,
    AddComment,
    RemoveComment,
    SetComment,
    SetBookmark,
    SetEnableState,
    AddGraph,
    RemoveGraph,
    RenameGraph,
    MoveNodesToGraph,
    RemoveRetainedPin,
    InsertNode,
    InsertEdge
};

// 명령 하나다. 칸은 종류마다 쓰는 것만 본다(GraphCommandList의 함수 주석).
struct ROCKS_NODEGRAPH_API GraphCommand {
    explicit GraphCommand(Allocator& allocator) noexcept
        : nodeData(allocator), layout(allocator), nodes(allocator), edges(allocator),
          bytes(allocator), comment(allocator), title(allocator) {
    }

    GraphCommandKind kind = GraphCommandKind::AddNode;
    Guid graph;
    Guid node;
    Guid target;
    Name pin;
    GraphNode nodeData;
    NodeLayout layout;
    GraphEdge edge;
    Array<Guid> nodes;
    Array<GraphEdge> edges;
    Array<uint8> bytes;
    CommentBox comment;
    Bookmark bookmark;
    bool hasBookmark = false;
    // Connect가 schema 판정 Make만 받는다(붙여넣기의 바깥 간선 — 바꾸기·변환이면 ConnectDenied).
    bool requireMake = false;
    NodeEnableState enableState = NodeEnableState::Enabled;
    GraphRole role = GraphRole::Collapsed;
    uint32 domainRole = 0;
    String title;
};

// 명령 목록이다. 함수마다 명령 하나를 끝에 더한다(값을 복사한다). 할당 실패는 그 오류이고 목록은
// 그대로다.
class ROCKS_NODEGRAPH_API GraphCommandList {
public:
    explicit GraphCommandList(Allocator& allocator) noexcept : commands_(allocator) {
    }

    // graph에 node를 더한다. node.id는 호출자가 정한다(GraphIdSource). layout.node는 node.id로
    // 맞춘다.
    Result<void> AddNode(Guid graph, const GraphNode& node, const NodeLayout& layout) noexcept;

    // graph의 노드들과 닿은 간선·배치를 뺀다.
    Result<void> RemoveNodes(Guid graph, Span<const Guid> nodes) noexcept;

    // graph에 edge를 잇는다(order는 무시하고 mutator가 정한다). requireMake면 schema 판정이 Make일
    // 때만 잇고 바꾸기·변환 판정은 ConnectDenied다(붙여넣기 — 기존 간선을 바꾸지 않는다).
    Result<void> Connect(Guid graph, const GraphEdge& edge, bool requireMake = false) noexcept;

    // graph에서 edge(키만 본다)를 끊는다.
    Result<void> Disconnect(Guid graph, const GraphEdge& edge) noexcept;

    // graph의 ordered 출력 핀(fromNode, fromPin)의 간선 순서를 order(키만 본다, 새 순서)로 바꾼다.
    Result<void> ReorderEdges(Guid graph, Guid fromNode, Name fromPin,
                              Span<const GraphEdge> order) noexcept;

    // 노드 속성을 Record 바이트 전체로 바꾼다.
    Result<void> SetNodeProperties(Guid graph, Guid node, Span<const uint8> properties) noexcept;

    Result<void> SetInputDefault(Guid graph, Guid node, Name pin, Span<const uint8> value) noexcept;

    Result<void> ResetInputDefault(Guid graph, Guid node, Name pin) noexcept;

    // 노드 배치를 바꾼다(layout.node는 node로 맞춘다).
    Result<void> SetLayout(Guid graph, Guid node, const NodeLayout& layout) noexcept;

    Result<void> AddComment(Guid graph, const CommentBox& comment) noexcept;

    Result<void> RemoveComment(Guid graph, Guid comment) noexcept;

    Result<void> SetComment(Guid graph, const CommentBox& comment) noexcept;

    // 북마크 bookmark.number를 bookmark로 둔다.
    Result<void> SetBookmark(Guid graph, const Bookmark& bookmark) noexcept;

    // 북마크 number를 지운다(없으면 무동작).
    Result<void> ClearBookmark(Guid graph, uint32 number) noexcept;

    Result<void> SetEnableState(Guid graph, Guid node, NodeEnableState state) noexcept;

    // 하위 그래프를 더한다(역할 Collapsed·DomainDefined, 배치 제목 title).
    Result<void> AddGraph(Guid graph, GraphRole role, uint32 domainRole, StringView title) noexcept;

    Result<void> RemoveGraph(Guid graph) noexcept;

    Result<void> RenameGraph(Guid graph, StringView title) noexcept;

    // source 그래프의 nodes와 그 사이 간선·배치를 target 그래프로 옮긴다.
    Result<void> MoveNodesToGraph(Guid source, Span<const Guid> nodes, Guid target) noexcept;

    Result<void> RemoveRetainedPin(Guid graph, Guid node, Name key) noexcept;

    // 완성된 노드 데이터(입력 기본값·고아 핀·활성 상태 포함, 등록되지 않은 종류 — 자리 노드 — 도
    // 된다)를 graph에 넣는다(붙여넣기·template — Docs/Design/NodeGraph 9.4절). layout.node는
    // node.id로 맞춘다. 입력 기본값·고아 핀 목록은 넣을 때 정규 순서로 맞춘다.
    Result<void> InsertNode(Guid graph, const GraphNode& node, const NodeLayout& layout) noexcept;

    // schema 판정 없이 간선을 넣는다(붙여넣기 — 9.4절). 끝은 현재 핀이나 그 노드의 고아 핀이고, 알
    // 수 없는 종류의 끝은 핀을 묻지 않는다. ordered 출력 핀이면 끝에 붙이고, 출력 끝이 알 수 없는
    // 종류나 고아 핀이면 edge.order를 그대로 둔다(순번 규칙이 없다 — 6.3절).
    Result<void> InsertEdge(Guid graph, const GraphEdge& edge) noexcept;

    Span<const GraphCommand> View() const noexcept {
        return commands_.View();
    }

    usize Size() const noexcept {
        return commands_.Size();
    }

    void Clear() noexcept {
        commands_.Clear();
    }

    Allocator& GetAllocator() const noexcept {
        return commands_.GetAllocator();
    }

private:
    Result<GraphCommand*> Push(GraphCommandKind kind, Guid graph) noexcept;

    Array<GraphCommand> commands_;
};

// 명령 검증 위반의 사유다(8.6절 명령 규칙). 이름은 GraphCommandReasonName이다(영어).
enum class GraphCommandReason : uint8 {
    None,
    GraphMissing,
    GraphExists,
    GraphRole,
    MainGraph,
    SameGraph,
    IdZero,
    NodeMissing,
    NodeExists,
    TypeUnknown,
    TypeVersion,
    PropertiesSchema,
    NodeValues,
    SinglePerDocument,
    CannotDelete,
    PinMissing,
    PinDirection,
    RetainedPin,
    EdgeExists,
    EdgeMissing,
    EdgeLimit,
    ConnectDenied,
    ConversionInvalid,
    NotOrdered,
    OrderMismatch,
    PinsDropped,
    ValueHidden,
    ValueEmpty,
    DefaultMissing,
    CommentExists,
    CommentMissing,
    EnableState,
    CrossingEdge,
    RetainedMissing,
    // 명령 목록에 같은 노드 id가 두 번 있다.
    DuplicateNode,
    // 새 핀 목록에 고아 핀과 같은 키가 생긴다(GI8 — 재결합은 NG3).
    RetainedShadowed,
    // 재경유 사슬에 순환이 생긴다(GI10).
    RerouteCycle,
    // 값 바이트가 MaxPropertyRecordBytes를 넘는다.
    ValueTooLarge,
    // 그래프의 노드·간선 수가 상한(MaxGraphNodes·MaxGraphEdges)에 이르렀다.
    GraphFull,
    // 명령 뒤 전체 불변식 검사(SetFullValidation)가 실패했다 — mutator 결함이다.
    InvariantBroken,
    // 넣는 노드 데이터가 틀렸다(InsertNode): 입력 기본값 핀·고아 핀 키가 겹치거나 None이다, 고아 핀
    // 방향이 정의되지 않았다, 재경유 노드에 고아 핀이 있다, 목록 끝에 값도 간선도 없는 고아 핀이
    // 남았다(GI7).
    NodeDataInvalid
};

ROCKS_NODEGRAPH_API const char* GraphCommandReasonName(GraphCommandReason reason) noexcept;

// 명령 실패 정보다. 해당하지 않는 칸은 0·None이다.
struct GraphCommandFailure {
    // 실패한 명령의 목록 번호다.
    uint32 command = 0;
    GraphCommandReason reason = GraphCommandReason::None;
    Guid graph;
    Guid node;
    Name pin;
};

} // namespace rocks::nodegraph
