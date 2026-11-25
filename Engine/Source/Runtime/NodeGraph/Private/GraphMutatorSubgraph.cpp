#include "GraphMutatorCore.h"

#include <Core/Assert.h>
#include <NodeGraph/GraphClone.h>

// 하위 그래프 인터페이스·경계 노드 명령 규칙이다(Docs/Design/NodeGraph 10.1절 NG3c).

namespace rocks::nodegraph::detail {

namespace {

bool SubgraphIsZero(const Guid& id) noexcept {
    return id.high == 0 && id.low == 0;
}

bool SubgraphNameNone(Name name) noexcept {
    return !name.IsValid() || name.IsNone();
}

void SubgraphSwap(Array<GraphBoundaryPin>& left, Array<GraphBoundaryPin>& right) noexcept {
    Array<GraphBoundaryPin> temporary(Move(left));
    left = Move(right);
    right = Move(temporary);
}

} // namespace

Guid GraphMutatorCore::OwnedGraph(Guid collapsedNode) const noexcept {
    for (const Graph& graph : document_.graphs) {
        if (graph.role == GraphRole::Collapsed && graph.ownerNode == collapsedNode) {
            return graph.id;
        }
    }

    return Guid{};
}

Guid GraphMutatorCore::ParentGraph(const Graph& graph) const noexcept {
    if (graph.role != GraphRole::Collapsed || SubgraphIsZero(graph.ownerNode)) {
        return Guid{};
    }

    for (const Graph& candidate : document_.graphs) {
        if (NodeIndex(candidate, graph.ownerNode) != IndexNone) {
            return candidate.id;
        }
    }

    return Guid{};
}

bool GraphMutatorCore::InsideTree(Guid graph, Guid root) const noexcept {
    // graph에서 부모를 따라 올라가며 root를 만나는지 본다. 깊이는 그래프 수를 넘지 않는다.
    Guid current = graph;
    for (usize step = 0; step <= document_.graphs.Size(); ++step) {
        if (current == root) {
            return true;
        }

        const usize index = GraphIndex(current);
        if (index == IndexNone) {
            return false;
        }

        current = ParentGraph(document_.graphs[index]);
        if (SubgraphIsZero(current)) {
            return false;
        }
    }

    return false;
}

Result<void> GraphMutatorCore::CheckPlacement(Guid graph, const GraphNode& node) noexcept {
    const Graph* target = FindGraph(graph);
    if (target == nullptr) {
        return Result<void>::Success();
    }

    if (registry_.IsBoundary(node.type)) {
        boundaryTouched_ = true;
        if (target->role != GraphRole::Collapsed) {
            return Fail(GraphCommandReason::BoundaryNode, graph, node.id);
        }
    }

    if (registry_.IsCollapsed(node.type)) {
        boundaryTouched_ = true;
        const Guid owned = OwnedGraph(node.id);
        if (SubgraphIsZero(owned)) {
            return Fail(GraphCommandReason::BoundaryNode, graph, node.id);
        }

        // 자기 하위 그래프 나무 안에 둘 수 없다(GI16 순환).
        if (InsideTree(graph, owned)) {
            return Fail(GraphCommandReason::CollapsedCycle, graph, node.id);
        }
    }

    return Result<void>::Success();
}

Result<void> GraphMutatorCore::SetGraphBoundary(const GraphCommand& command) noexcept {
    Graph* graph = FindGraph(command.graph);
    if (graph == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    if (graph->role != GraphRole::Collapsed) {
        return Fail(GraphCommandReason::GraphRole, command.graph);
    }

    // GI15 경계 목록 규칙.
    const usize total = command.inputs.Size() + command.outputs.Size();
    if (total > MaxPinsPerNode) {
        return Fail(GraphCommandReason::BoundaryInvalid, command.graph);
    }

    const auto at = [&command](usize position) noexcept -> const GraphBoundaryPin& {
        return position < command.inputs.Size() ? command.inputs[position]
                                                : command.outputs[position - command.inputs.Size()];
    };

    for (usize position = 0; position < total; ++position) {
        const GraphBoundaryPin& pin = at(position);
        bool valid = !SubgraphNameNone(pin.key) && pin.type.IsValid() && pin.maxEdges >= 1 &&
                     (pin.kind == PinKind::Data || pin.kind == PinKind::Flow);
        for (usize other = 0; valid && other < position; ++other) {
            valid = !(at(other).key == pin.key);
        }

        if (!valid) {
            return Fail(GraphCommandReason::BoundaryInvalid, command.graph, Guid{}, pin.key);
        }
    }

    boundaryTouched_ = true;
    // 핀이 바뀌는 노드: 이 그래프의 경계 노드와 부모의 Graph.Collapsed. 옛 핀 목록(고아 핀의 마지막
    // 타입)을 먼저 센다.
    struct Affected {
        explicit Affected(Allocator& allocator) noexcept : pins(allocator) {
        }

        Guid graph;
        Guid node;
        PinList pins;
        bool known = false;
    };

    Array<Affected> affected(allocator_);
    const auto addAffected = [this, &affected](const Graph& owner,
                                               const GraphNode& node) noexcept -> Result<void> {
        auto added = affected.TryEmplaceBack(allocator_);
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        Affected& entry = *added.Value();
        entry.graph = owner.id;
        entry.node = node.id;
        auto known = PinsOf(owner, node, entry.pins);
        if (!known) {
            return Result<void>::Failure(known.Error());
        }

        entry.known = known.Value();
        return Result<void>::Success();
    };

    for (const GraphNode& node : graph->nodes) {
        if (registry_.IsBoundary(node.type)) {
            if (auto added = addAffected(*graph, node); !added) {
                return added;
            }
        }
    }

    const Guid parentId = ParentGraph(*graph);
    if (const Graph* parent = FindGraph(parentId); parent != nullptr) {
        const usize index = NodeIndex(*parent, graph->ownerNode);
        if (index != IndexNone) {
            if (auto added = addAffected(*parent, parent->nodes[index]); !added) {
                return added;
            }
        }
    }

    // 경계 목록은 그래프 머리 op 하나로 바꾼다(적용 때 GraphBoundaryChanged·NodePinsChanged 기록).
    GraphPatchOp op = MakeOp(GraphSlotKind::GraphBoundary, command.graph);
    op.hasValue = true;
    if (auto copied = op.graphValue.inputs.TryAppend(command.inputs.View()); !copied) {
        return copied;
    }

    if (auto copied = op.graphValue.outputs.TryAppend(command.outputs.View()); !copied) {
        return copied;
    }

    if (auto emitted = Emit(Move(op)); !emitted) {
        return emitted;
    }

    // 사라진 경계 핀에 닿은 간선·값은 고아 핀으로, 다시 생긴 키는 재결합한다(핀 재정리).
    for (const Affected& entry : affected) {
        Graph* owner = FindGraph(entry.graph);
        ROCKS_ASSERT(owner != nullptr);
        const usize index = NodeIndex(*owner, entry.node);
        ROCKS_ASSERT(index != IndexNone);
        GraphNode changed(allocator_);
        if (auto cloned = CloneGraphNode(owner->nodes[index], changed); !cloned) {
            return cloned;
        }

        if (auto reconciled = ReconcileNode(*owner, Move(changed), Span<const PinKeyRename>(),
                                            entry.known ? &entry.pins : nullptr, nullptr);
            !reconciled) {
            return reconciled;
        }
    }

    return Result<void>::Success();
}

Result<void> GraphMutatorCore::RemoveGraphTree(Guid graphId) noexcept {
    boundaryTouched_ = true;
    Graph* graph = FindGraph(graphId);
    if (graph == nullptr || FindLayoutGraph(graphId) == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, graphId);
    }

    // 삭제 불가 노드가 있으면 거부한다. 연쇄로 지우는 그래프의 경계 노드는 뺀다(10.1절).
    Array<Guid> children(allocator_);
    for (const GraphNode& node : graph->nodes) {
        const RegisteredNodeType* type = registry_.FindNodeType(document_.domain, node.type);
        if (type != nullptr && !registry_.IsBoundary(node.type) &&
            !HasCapability(type->descriptor.capabilities, NodeCapabilities::CanDelete)) {
            return Fail(GraphCommandReason::CannotDelete, graphId, node.id);
        }

        if (registry_.IsCollapsed(node.type)) {
            const Guid owned = OwnedGraph(node.id);
            if (!SubgraphIsZero(owned)) {
                if (auto pushed = children.TryPushBack(owned); !pushed) {
                    return pushed;
                }
            }
        }
    }

    // 안쪽 하위 그래프부터 지운다(그래프 배열이 줄어 graph 포인터는 다시 찾는다).
    for (const Guid& child : children) {
        if (auto removed = RemoveGraphTree(child); !removed) {
            return removed;
        }
    }

    graph = FindGraph(graphId);
    ROCKS_ASSERT(graph != nullptr);

    // 내용을 먼저 뺀다: 간선 → 노드 배치·노드 → 주석 → 북마크 → 그래프.
    Array<GraphEdge> edges(allocator_);
    if (auto copied = edges.TryAppend(graph->edges.View()); !copied) {
        return copied;
    }

    for (const GraphEdge& edge : edges) {
        if (auto erased = EraseEdge(graphId, edge); !erased) {
            return erased;
        }
    }

    Array<Guid> nodes(allocator_);
    for (const GraphNode& node : FindGraph(graphId)->nodes) {
        if (auto pushed = nodes.TryPushBack(node.id); !pushed) {
            return pushed;
        }
    }

    for (const Guid& id : nodes) {
        GraphPatchOp nodeLayout = MakeOp(GraphSlotKind::NodeLayout, graphId);
        nodeLayout.id = id;
        if (auto emitted = Emit(Move(nodeLayout)); !emitted) {
            return emitted;
        }

        GraphPatchOp node = MakeOp(GraphSlotKind::Node, graphId);
        node.id = id;
        if (auto emitted = Emit(Move(node)); !emitted) {
            return emitted;
        }
    }

    Array<Guid> comments(allocator_);
    Array<uint32> bookmarks(allocator_);
    const GraphLayout* current = FindLayoutGraph(graphId);
    for (const CommentBox& comment : current->comments) {
        if (auto pushed = comments.TryPushBack(comment.id); !pushed) {
            return pushed;
        }
    }

    for (const Bookmark& bookmark : current->bookmarks) {
        if (auto pushed = bookmarks.TryPushBack(bookmark.number); !pushed) {
            return pushed;
        }
    }

    for (const Guid& id : comments) {
        GraphPatchOp op = MakeOp(GraphSlotKind::Comment, graphId);
        op.id = id;
        if (auto emitted = Emit(Move(op)); !emitted) {
            return emitted;
        }
    }

    for (const uint32 number : bookmarks) {
        GraphPatchOp op = MakeOp(GraphSlotKind::Bookmark, graphId);
        op.number = number;
        if (auto emitted = Emit(Move(op)); !emitted) {
            return emitted;
        }
    }

    GraphPatchOp op = MakeOp(GraphSlotKind::Graph, graphId);
    return Emit(Move(op));
}

Result<void> GraphMutatorCore::CheckSubgraphsAtEnd(usize lastCommand) noexcept {
    if (!boundaryTouched_) {
        return Result<void>::Success();
    }

    commandIndex_ = static_cast<uint32>(lastCommand);
    for (const Graph& graph : document_.graphs) {
        usize inputs = 0;
        usize outputs = 0;
        for (const GraphNode& node : graph.nodes) {
            inputs += registry_.IsGraphInput(node.type) ? 1 : 0;
            outputs += registry_.IsGraphOutput(node.type) ? 1 : 0;
            // Graph.Collapsed마다 하위 그래프가 하나다(GI16).
            if (registry_.IsCollapsed(node.type) && SubgraphIsZero(OwnedGraph(node.id))) {
                return Fail(GraphCommandReason::BoundaryNode, graph.id, node.id);
            }
        }

        if (graph.role != GraphRole::Collapsed) {
            continue;
        }

        // 경계 노드가 각각 하나이고(GI15), 부모 Graph.Collapsed가 다른 그래프에 있다(GI16).
        if (inputs != 1 || outputs != 1) {
            return Fail(GraphCommandReason::BoundaryNode, graph.id);
        }

        const Guid parent = ParentGraph(graph);
        const Graph* owner = FindGraph(parent);
        if (owner == nullptr || parent == graph.id ||
            !registry_.IsCollapsed(owner->nodes[NodeIndex(*owner, graph.ownerNode)].type)) {
            return Fail(GraphCommandReason::BoundaryNode, graph.id, graph.ownerNode);
        }
    }

    return Result<void>::Success();
}

Result<void> GraphMutatorCore::ApplyBoundaryOp(GraphPatchOp& op, bool record) noexcept {
    Graph* graph = FindGraph(op.graph);
    if (graph == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (record) {
        // 기록할 자리를 먼저 잡아 바꾼 뒤에는 실패하지 않게 한다(그래프 하나 + 부모 하나 + 경계
        // 노드들).
        constexpr usize GraphAndParentChanges = 2;
        usize changes = GraphAndParentChanges;
        for (const GraphNode& node : graph->nodes) {
            changes += registry_.IsBoundary(node.type) ? 1 : 0;
        }

        if (auto reserved = pending_.changes.TryEnsureCapacity(pending_.changes.Size() + changes);
            !reserved) {
            return reserved;
        }
    }

    SubgraphSwap(graph->inputs, op.graphValue.inputs);
    SubgraphSwap(graph->outputs, op.graphValue.outputs);
    op.replaced = true;
    if (!record) {
        return Result<void>::Success();
    }

    ROCKS_ASSERT(
        Record(GraphChangeKind::GraphBoundaryChanged, op.graph, Guid{}, Guid{}, Name(), Name(), 0));
    for (const GraphNode& node : graph->nodes) {
        if (registry_.IsBoundary(node.type)) {
            ROCKS_ASSERT(Record(GraphChangeKind::NodePinsChanged, op.graph, node.id, Guid{}, Name(),
                                Name(), 0));
        }
    }

    const Guid parent = ParentGraph(*graph);
    if (!SubgraphIsZero(parent)) {
        ROCKS_ASSERT(Record(GraphChangeKind::NodePinsChanged, parent, graph->ownerNode, Guid{},
                            Name(), Name(), 0));
    }

    return Result<void>::Success();
}

} // namespace rocks::nodegraph::detail
