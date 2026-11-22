#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <NodeGraph/GraphChange.h>
#include <NodeGraph/GraphCommand.h>
#include <NodeGraph/GraphDocument.h>
#include <NodeGraph/GraphIdSource.h>
#include <NodeGraph/GraphPatch.h>
#include <NodeGraph/GraphSchema.h>
#include <NodeGraph/NodeTypeRegistry.h>

// GraphMutator의 내부다(Docs/Design/NodeGraph.ko.md 8.6절). GraphMutatorCore.cpp가 색인·op
// 적용·명령 실행· undo·redo·bracket·통지를, GraphMutatorCommands.cpp가 명령마다의 검증과 op 생성을
// 맡는다.
namespace rocks::nodegraph::detail {

// 기본 연결 판정이다(GraphSchema::CanConnect 기본 구현과 schema 없는 도메인이 쓴다).
ConnectVerdict DefaultConnectVerdict(const ConnectQuery& query) noexcept;

// 나가는 간선 색인 항목이다(메모리 전용 — 토큰 순이라 저장하지 않는다).
struct OutgoingEdge {
    Guid graph;
    Guid from;
    Name fromPin;
    Guid to;
    Name toPin;
};

class GraphMutatorCore {
public:
    GraphMutatorCore(GraphDocument& document, GraphDocumentLayout& layout,
                     const NodeTypeRegistry& registry, GraphIdSource& ids,
                     Allocator& allocator) noexcept;

    GraphMutatorCore(const GraphMutatorCore&) = delete;
    GraphMutatorCore& operator=(const GraphMutatorCore&) = delete;

    Result<void> Initialize() noexcept;

    uint64 Revision() const noexcept {
        return revision_;
    }

    Result<GraphPatch> Execute(uint64 source, uint64 expectedRevision,
                               const GraphCommandList& commands,
                               GraphCommandFailure* failure) noexcept;
    Result<void> Undo(uint64 source, GraphPatch& patch) noexcept;
    Result<void> Redo(uint64 source, GraphPatch& patch) noexcept;
    Result<void> BeginInteraction(uint64 source) noexcept;
    Result<GraphPatch> EndInteraction(uint64 source) noexcept;
    Result<void> CancelInteraction(uint64 source) noexcept;

    bool InteractionOpen() const noexcept {
        return interaction_;
    }

    Result<void> AddListener(GraphChangeListener& listener) noexcept;
    void RemoveListener(GraphChangeListener& listener) noexcept;

    void SetFullValidation(bool enabled) noexcept {
        fullValidation_ = enabled;
    }

private:
    // ---- 조회(GraphMutatorCore.cpp) ----
    usize GraphIndex(Guid id) const noexcept;
    Graph* FindGraph(Guid id) noexcept;
    GraphLayout* FindLayoutGraph(Guid id) noexcept;
    static usize NodeIndex(const Graph& graph, Guid id) noexcept;
    static usize LayoutNodeIndex(const GraphLayout& layout, Guid id) noexcept;
    static usize CommentIndex(const GraphLayout& layout, Guid id) noexcept;
    static usize BookmarkIndex(const GraphLayout& layout, uint32 number) noexcept;
    bool IdExists(Guid id) const noexcept;
    // 간선 정규 순서의 하한 위치다(names 조회 오류를 돌려준다). found는 그 위치가 같은 키인지다.
    Result<usize> EdgeLowerBound(const Graph& graph, const GraphEdge& key,
                                 bool& found) const noexcept;
    // node로 들어오는 간선 중 pin(None이면 모두)의 간선을 out에 덧붙인다.
    Result<void> CollectIncoming(const Graph& graph, Guid node, Name pin,
                                 Array<GraphEdge>& out) const noexcept;
    // node의 출력 핀 pin(None이면 모두)에서 나가는 간선을 out에 덧붙인다(색인 + 간선 값).
    Result<void> CollectOutgoing(const Graph& graph, Guid node, Name pin,
                                 Array<GraphEdge>& out) noexcept;
    // node의 현재 핀 목록이다. 알 수 없는 종류면 false다(out은 비어 있다).
    Result<bool> PinsOf(const Graph& graph, const GraphNode& node, PinList& out) const noexcept;

    // ---- op(GraphMutatorCore.cpp) ----
    GraphPatchOp MakeOp(GraphSlotKind kind, Guid graph) noexcept;
    // op를 적용한다(맞바꾸기). record면 통지를 pending_에 쌓는다(실패하면 op를 되돌리고 오류).
    Result<void> ApplyOp(GraphPatchOp& op, bool record) noexcept;
    Result<void> ApplyGraphOp(GraphPatchOp& op, bool record) noexcept;
    Result<void> ApplyNodeOp(GraphPatchOp& op, bool record) noexcept;
    Result<void> ApplyLayoutOp(GraphPatchOp& op, bool record) noexcept;
    Result<void> ApplyEdgeOp(GraphPatchOp& op, bool record) noexcept;
    Result<void> ApplyCommentOp(GraphPatchOp& op, bool record) noexcept;
    Result<void> ApplyBookmarkOp(GraphPatchOp& op, bool record) noexcept;
    Result<void> ApplyTitleOp(GraphPatchOp& op, bool record) noexcept;
    Result<void> RecordNodeChanges(Guid graph, const GraphNode& before,
                                   const GraphNode& after) noexcept;
    Result<void> Record(GraphChangeKind kind, Guid graph, Guid node, Guid other, Name pin,
                        Name otherPin, uint32 number) noexcept;
    Result<void> IndexId(Guid id) noexcept;
    void UnindexId(Guid id) noexcept;
    Result<usize> OutgoingSlot(const OutgoingEdge& key, bool& found) const noexcept;
    // ops를 역순으로 적용해 되돌린다(실패는 불변식 결함 — Fatal).
    void Revert(Array<GraphPatchOp>& ops, usize count) noexcept;
    // op 하나를 work_에 담고 적용한다(명령 구현이 쓴다).
    Result<void> Emit(GraphPatchOp&& op) noexcept;
    Result<void> MergeInto(GraphPatch& target, HashMap<uint64, uint32>& keys) noexcept;
    void Notify(GraphChangeCause cause, bool interactive) noexcept;
    Result<void> CheckFullValidation() noexcept;
    Result<void> ApplyPatch(GraphPatch& patch, bool reverse, GraphChangeCause cause) noexcept;

    // ---- 명령(GraphMutatorCommands.cpp) ----
    Result<void> Fail(GraphCommandReason reason, Guid graph, Guid node = Guid{},
                      Name pin = Name()) noexcept;
    Result<void> ExecuteCommand(const GraphCommand& command) noexcept;
    Result<void> AddNode(const GraphCommand& command) noexcept;
    Result<void> RemoveNodes(const GraphCommand& command) noexcept;
    Result<void> Connect(const GraphCommand& command) noexcept;
    Result<void> ConnectResolved(Graph& graph, const GraphEdge& edge, const PinDescriptor& fromPin,
                                 const PinDescriptor& toPin, bool checkSchema) noexcept;
    Result<void> InsertConversion(Graph& graph, const GraphEdge& edge,
                                  Name conversionType) noexcept;
    Result<void> Disconnect(const GraphCommand& command) noexcept;
    Result<void> ReorderEdges(const GraphCommand& command) noexcept;
    Result<void> SetNodeProperties(const GraphCommand& command) noexcept;
    Result<void> SetInputDefault(const GraphCommand& command) noexcept;
    Result<void> ResetInputDefault(const GraphCommand& command) noexcept;
    Result<void> SetLayout(const GraphCommand& command) noexcept;
    Result<void> AddComment(const GraphCommand& command) noexcept;
    Result<void> RemoveComment(const GraphCommand& command) noexcept;
    Result<void> SetComment(const GraphCommand& command) noexcept;
    Result<void> SetBookmark(const GraphCommand& command) noexcept;
    Result<void> SetEnableState(const GraphCommand& command) noexcept;
    Result<void> AddGraph(const GraphCommand& command) noexcept;
    Result<void> RemoveGraph(const GraphCommand& command) noexcept;
    Result<void> RenameGraph(const GraphCommand& command) noexcept;
    Result<void> MoveNodesToGraph(const GraphCommand& command) noexcept;
    Result<void> RemoveRetainedPin(const GraphCommand& command) noexcept;
    // 간선 하나를 빼는 op를 낸다.
    Result<void> EraseEdge(Guid graph, const GraphEdge& edge) noexcept;
    // 간선을 뺀 뒤 양 끝의 고아 핀이 간선·값을 모두 잃었으면 지운다(6.4절, GI7). skip 노드는 보지
    // 않는다.
    Result<void> PruneRetainedEnds(Graph& graph, const GraphEdge& edge, Guid skip) noexcept;
    Result<void> PruneRetainedPin(Graph& graph, Guid node, Name key) noexcept;
    // edge를 더하면 재경유 사슬에 순환이 생기는가(GI10).
    Result<bool> RerouteCycle(const Graph& graph, const GraphEdge& edge) noexcept;
    // ids에 같은 id가 있으면 그 id를 돌려준다(없으면 0).
    Result<Guid> FindDuplicate(Span<const Guid> ids) noexcept;
    // (fromNode, fromPin)의 간선 순번을 핀 서술에 맞춘다: ordered면 0부터 빈틈없이(지금 순번 순),
    // 아니면 0.
    Result<void> NormalizeOutputOrder(Graph& graph, Guid fromNode, Name fromPin) noexcept;
    // node를 바꾼 사본(node 전체)으로 노드 값 바꾸기 op를 낸다.
    Result<void> ReplaceNode(Guid graph, GraphNode&& node) noexcept;
    Result<void> CloneNodeFor(const GraphNode& source, GraphNode& out) noexcept;

    GraphDocument& document_;
    GraphDocumentLayout& layout_;
    const NodeTypeRegistry& registry_;
    GraphIdSource& ids_;
    Allocator& allocator_;
    uint64 revision_ = 0;
    // 문서 상태 토큰이다(patch의 stateBefore_·stateAfter_와 맞춘다). 바꾸는 적용마다 새 값을
    // 받는다.
    uint64 state_ = 1;
    uint64 nextState_ = 2;
    // 통지 중이면 RemoveListener는 자리를 비우기만 한다(반복 중 무효화 방지).
    bool notifying_ = false;
    bool initialized_ = false;
    bool fullValidation_ = false;

    // 문서 안 id(GraphId·NodeId·주석 id) 정렬 배열이다(GI9).
    Array<Guid> idIndex_;
    // 나가는 간선 색인이다.
    Array<OutgoingEdge> outgoing_;
    Array<GraphChangeListener*> listeners_;
    GraphChangeBatch pending_;

    // 지금 명령 목록이 적용한 op다(실패하면 되돌린다).
    Array<GraphPatchOp> work_;
    GraphCommandFailure* failure_ = nullptr;
    uint32 commandIndex_ = 0;

    bool interaction_ = false;
    uint64 interactionSource_ = 0;
    bool interactionSemantic_ = false;
    GraphPatch interactionPatch_;
    HashMap<uint64, uint32> interactionKeys_;

    // 명령 구현의 임시 저장소다.
    PinList pinsA_;
    PinList pinsB_;
    Array<GraphEdge> edgesA_;
    Array<GraphEdge> edgesB_;
};

} // namespace rocks::nodegraph::detail
