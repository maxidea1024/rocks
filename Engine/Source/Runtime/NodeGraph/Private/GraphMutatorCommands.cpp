#include "GraphMutatorCore.h"

#include <Core/Algorithms.h>
#include <Core/Assert.h>
#include <NodeGraph/GraphClone.h>
#include <NodeGraph/GraphNameOrder.h>
#include <NodeGraph/GraphReadView.h>
#include <Serialization/Record.h>

// GraphMutator 명령마다의 검증과 op 생성이다(Docs/Design/NodeGraph 8.1·8.6절 "명령 규칙").
namespace rocks::nodegraph::detail {

namespace {

bool CommandIsZero(const Guid& id) noexcept {
    return id.high == 0 && id.low == 0;
}

bool CommandNameNone(Name name) noexcept {
    return !name.IsValid() || name.IsNone();
}

// 속성 바이트가 서술의 속성 struct와 맞는가(GI13 규칙).
bool CommandPropertiesMatch(const NodeTypeDescriptor& descriptor,
                            Span<const uint8> bytes) noexcept {
    if (CommandIsZero(descriptor.propertiesStruct)) {
        return bytes.Size() == 0;
    }

    auto header = ReadRecordHeader(bytes);
    return header && header.Value().schema == descriptor.propertiesStruct;
}

bool CommandSameEdgeKey(const GraphEdge& left, const GraphEdge& right) noexcept {
    return left.fromNode == right.fromNode && left.fromPin == right.fromPin &&
           left.toNode == right.toNode && left.toPin == right.toPin;
}

} // namespace

Result<void> GraphMutatorCore::Fail(GraphCommandReason reason, Guid graph, Guid node,
                                    Name pin) noexcept {
    failure_->command = commandIndex_;
    failure_->reason = reason;
    failure_->graph = graph;
    failure_->node = node;
    failure_->pin = pin;
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

Result<void> GraphMutatorCore::ExecuteCommand(const GraphCommand& command) noexcept {
    switch (command.kind) {
    case GraphCommandKind::AddNode:
        return AddNode(command);
    case GraphCommandKind::RemoveNodes:
        return RemoveNodes(command);
    case GraphCommandKind::Connect:
        return Connect(command);
    case GraphCommandKind::Disconnect:
        return Disconnect(command);
    case GraphCommandKind::ReorderEdges:
        return ReorderEdges(command);
    case GraphCommandKind::SetNodeProperties:
        return SetNodeProperties(command);
    case GraphCommandKind::SetInputDefault:
        return SetInputDefault(command);
    case GraphCommandKind::ResetInputDefault:
        return ResetInputDefault(command);
    case GraphCommandKind::SetLayout:
        return SetLayout(command);
    case GraphCommandKind::AddComment:
        return AddComment(command);
    case GraphCommandKind::RemoveComment:
        return RemoveComment(command);
    case GraphCommandKind::SetComment:
        return SetComment(command);
    case GraphCommandKind::SetBookmark:
        return SetBookmark(command);
    case GraphCommandKind::SetEnableState:
        return SetEnableState(command);
    case GraphCommandKind::AddGraph:
        return AddGraph(command);
    case GraphCommandKind::RemoveGraph:
        return RemoveGraph(command);
    case GraphCommandKind::RenameGraph:
        return RenameGraph(command);
    case GraphCommandKind::MoveNodesToGraph:
        return MoveNodesToGraph(command);
    case GraphCommandKind::RemoveRetainedPin:
        return RemoveRetainedPin(command);
    case GraphCommandKind::InsertNode:
        return InsertNode(command);
    case GraphCommandKind::InsertEdge:
        return InsertEdge(command);
    }

    return Fail(GraphCommandReason::None, command.graph);
}

Result<void> GraphMutatorCore::CloneNodeFor(const GraphNode& source, GraphNode& out) noexcept {
    return CloneGraphNode(source, out);
}

Result<void> GraphMutatorCore::ReplaceNode(Guid graph, GraphNode&& node) noexcept {
    GraphPatchOp op = MakeOp(GraphSlotKind::Node, graph);
    op.id = node.id;
    op.node = Move(node);
    op.hasValue = true;
    return Emit(Move(op));
}

Result<void> GraphMutatorCore::EraseEdge(Guid graph, const GraphEdge& edge) noexcept {
    GraphPatchOp op = MakeOp(GraphSlotKind::Edge, graph);
    op.edgeKey = edge;
    op.edgeKey.order = 0;
    op.hasValue = false;
    return Emit(Move(op));
}

Result<void> GraphMutatorCore::PruneRetainedPin(Graph& graph, Guid node, Name key) noexcept {
    const usize index = NodeIndex(graph, node);
    if (index == IndexNone) {
        return Result<void>::Success();
    }

    const GraphNode& current = graph.nodes[index];
    usize found = IndexNone;
    for (usize position = 0; position < current.retainedPins.Size(); ++position) {
        if (current.retainedPins[position].key == key) {
            found = position;
        }
    }

    if (found == IndexNone || !current.retainedPins[found].value.bytes.IsEmpty()) {
        return Result<void>::Success();
    }

    // 그 고아 핀에 남은 간선이 있으면 둔다.
    Array<GraphEdge> remaining(allocator_);
    const Result<void> collected = current.retainedPins[found].direction == PinDirection::Input
                                       ? CollectIncoming(graph, node, key, remaining)
                                       : CollectOutgoing(graph, node, key, remaining);
    if (!collected) {
        return Result<void>::Failure(collected.Error());
    }

    if (!remaining.IsEmpty()) {
        return Result<void>::Success();
    }

    GraphNode changed(allocator_);
    if (auto cloned = CloneGraphNode(graph.nodes[index], changed); !cloned) {
        return cloned;
    }

    if (auto removed = changed.retainedPins.TryRemoveAt(found); !removed) {
        return removed;
    }

    return ReplaceNode(graph.id, Move(changed));
}

Result<void> GraphMutatorCore::PruneRetainedEnds(Graph& graph, const GraphEdge& edge,
                                                 Guid skip) noexcept {
    if (!(edge.toNode == skip)) {
        if (auto pruned = PruneRetainedPin(graph, edge.toNode, edge.toPin); !pruned) {
            return pruned;
        }
    }

    if (!(edge.fromNode == skip)) {
        return PruneRetainedPin(graph, edge.fromNode, edge.fromPin);
    }

    return Result<void>::Success();
}

Result<bool> GraphMutatorCore::RerouteCycle(const Graph& graph, const GraphEdge& edge) noexcept {
    const usize toIndex = NodeIndex(graph, edge.toNode);
    if (toIndex == IndexNone || !registry_.IsReroute(graph.nodes[toIndex].type)) {
        return Result<bool>::Success(false);
    }

    // 출력 쪽에서 재경유 입력을 거슬러 올라가며 to 재경유에 닿는지 본다(사슬 길이 ≤ 노드 수).
    Guid current = edge.fromNode;
    Array<GraphEdge> incoming(allocator_);
    for (usize steps = 0; steps <= graph.nodes.Size(); ++steps) {
        if (current == edge.toNode) {
            return Result<bool>::Success(true);
        }

        const usize index = NodeIndex(graph, current);
        if (index == IndexNone || !registry_.IsReroute(graph.nodes[index].type)) {
            return Result<bool>::Success(false);
        }

        incoming.Clear();
        if (auto collected = CollectIncoming(graph, current, Name(), incoming); !collected) {
            return Result<bool>::Failure(collected.Error());
        }

        if (incoming.IsEmpty()) {
            return Result<bool>::Success(false);
        }

        current = incoming[0].fromNode;
    }

    return Result<bool>::Success(true);
}

Result<Guid> GraphMutatorCore::FindDuplicate(Span<const Guid> ids) noexcept {
    Array<Guid> sorted(allocator_);
    if (auto copied = sorted.TryAppend(ids); !copied) {
        return Result<Guid>::Failure(copied.Error());
    }

    algo::Sort(sorted.View(), [](const Guid& left, const Guid& right) noexcept {
        return CompareGuid(left, right) < 0;
    });
    for (usize index = 1; index < sorted.Size(); ++index) {
        if (sorted[index] == sorted[index - 1]) {
            return Result<Guid>::Success(sorted[index]);
        }
    }

    return Result<Guid>::Success(Guid{});
}

Result<void> GraphMutatorCore::NormalizeOutputOrder(Graph& graph, Guid fromNode,
                                                    Name fromPin) noexcept {
    const usize nodeIndex = NodeIndex(graph, fromNode);
    if (nodeIndex == IndexNone) {
        return Result<void>::Success();
    }

    PinList pins(allocator_);
    auto known = PinsOf(graph, graph.nodes[nodeIndex], pins);
    if (!known) {
        return Result<void>::Failure(known.Error());
    }

    // 알 수 없는 종류·고아 핀은 순번 규칙이 없다(6.3절).
    const PinDescriptor* pin = known.Value() ? pins.Find(fromPin) : nullptr;
    if (pin == nullptr || pin->direction != PinDirection::Output) {
        return Result<void>::Success();
    }

    Array<GraphEdge> edges(allocator_);
    if (auto collected = CollectOutgoing(graph, fromNode, fromPin, edges); !collected) {
        return collected;
    }

    NamePool& names = registry_.Names();
    algo::Sort(edges.View(), [&names](const GraphEdge& left, const GraphEdge& right) noexcept {
        if (left.order != right.order) {
            return left.order < right.order;
        }

        if (const int32 compared = CompareGuid(left.toNode, right.toNode); compared != 0) {
            return compared < 0;
        }

        auto pin = CompareNames(names, left.toPin, right.toPin);
        return pin && pin.Value() < 0;
    });

    for (usize index = 0; index < edges.Size(); ++index) {
        const uint32 expected = pin->ordered ? static_cast<uint32>(index) : 0;
        if (edges[index].order == expected) {
            continue;
        }

        GraphPatchOp op = MakeOp(GraphSlotKind::Edge, graph.id);
        op.edgeKey = edges[index];
        op.edge = edges[index];
        op.edge.order = expected;
        op.hasValue = true;
        if (auto emitted = Emit(Move(op)); !emitted) {
            return emitted;
        }
    }

    return Result<void>::Success();
}

// ---- 노드 ----

Result<void> GraphMutatorCore::AddNode(const GraphCommand& command) noexcept {
    Graph* graph = FindGraph(command.graph);
    if (graph == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    const GraphNode& source = command.nodeData;
    if (CommandIsZero(source.id)) {
        return Fail(GraphCommandReason::IdZero, command.graph);
    }

    if (graph->nodes.Size() >= MaxGraphNodes) {
        return Fail(GraphCommandReason::GraphFull, command.graph, source.id);
    }

    if (source.properties.bytes.Size() > MaxPropertyRecordBytes) {
        return Fail(GraphCommandReason::ValueTooLarge, command.graph, source.id);
    }

    if (IdExists(source.id)) {
        return Fail(GraphCommandReason::NodeExists, command.graph, source.id);
    }

    const RegisteredNodeType* type = CommandNameNone(source.type)
                                         ? nullptr
                                         : registry_.FindNodeType(document_.domain, source.type);
    if (type == nullptr) {
        return Fail(GraphCommandReason::TypeUnknown, command.graph, source.id);
    }

    const NodeTypeDescriptor& descriptor = type->descriptor;
    if (source.typeSchemaVersion != descriptor.schemaVersion) {
        return Fail(GraphCommandReason::TypeVersion, command.graph, source.id);
    }

    if (!CommandPropertiesMatch(descriptor, source.properties.bytes.View())) {
        return Fail(GraphCommandReason::PropertiesSchema, command.graph, source.id);
    }

    if (!source.inputDefaults.IsEmpty() || !source.retainedPins.IsEmpty()) {
        return Fail(GraphCommandReason::NodeValues, command.graph, source.id);
    }

    if (source.enableState != NodeEnableState::Enabled &&
        (source.enableState != NodeEnableState::Disabled ||
         !HasCapability(descriptor.capabilities, NodeCapabilities::AllowDisable))) {
        return Fail(GraphCommandReason::EnableState, command.graph, source.id);
    }

    if (HasCapability(descriptor.capabilities, NodeCapabilities::SinglePerDocument)) {
        for (const Graph& other : document_.graphs) {
            for (const GraphNode& node : other.nodes) {
                if (node.type == source.type) {
                    return Fail(GraphCommandReason::SinglePerDocument, command.graph, source.id);
                }
            }
        }
    }

    // 동적 핀을 유도할 수 있어야 한다.
    auto pins = PinsOf(*graph, source, pinsA_);
    if (!pins) {
        if (pins.Error() == ErrorCode::OutOfMemory) {
            return Result<void>::Failure(pins.Error());
        }

        return Fail(GraphCommandReason::PropertiesSchema, command.graph, source.id);
    }

    GraphPatchOp node = MakeOp(GraphSlotKind::Node, command.graph);
    node.id = source.id;
    node.hasValue = true;
    if (auto cloned = CloneGraphNode(source, node.node); !cloned) {
        return cloned;
    }

    GraphPatchOp layout = MakeOp(GraphSlotKind::NodeLayout, command.graph);
    layout.id = source.id;
    layout.hasValue = true;
    if (auto cloned = CloneNodeLayout(command.layout, layout.layout); !cloned) {
        return cloned;
    }

    layout.layout.node = source.id;
    if (auto emitted = Emit(Move(node)); !emitted) {
        return emitted;
    }

    return Emit(Move(layout));
}

Result<void> GraphMutatorCore::RemoveNodes(const GraphCommand& command) noexcept {
    Graph* graph = FindGraph(command.graph);
    if (graph == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    auto duplicate = FindDuplicate(command.nodes.View());
    if (!duplicate) {
        return Result<void>::Failure(duplicate.Error());
    }

    if (!CommandIsZero(duplicate.Value())) {
        return Fail(GraphCommandReason::DuplicateNode, command.graph, duplicate.Value());
    }

    for (const Guid& id : command.nodes) {
        const usize index = NodeIndex(*graph, id);
        if (index == IndexNone) {
            return Fail(GraphCommandReason::NodeMissing, command.graph, id);
        }

        const RegisteredNodeType* type =
            registry_.FindNodeType(document_.domain, graph->nodes[index].type);
        if (type != nullptr &&
            !HasCapability(type->descriptor.capabilities, NodeCapabilities::CanDelete)) {
            return Fail(GraphCommandReason::CannotDelete, command.graph, id);
        }

        // 닿은 간선을 빼고, 다른 노드의 ordered 출력 핀 순번을 다시 맞춘다.
        edgesA_.Clear();
        if (auto collected = CollectIncoming(*graph, id, Name(), edgesA_); !collected) {
            return collected;
        }

        edgesB_.Clear();
        if (auto collected = CollectOutgoing(*graph, id, Name(), edgesB_); !collected) {
            return collected;
        }

        // 자기 자신으로 가는 간선(self-loop)은 들어오는 쪽에 이미 있다 — 두 번 빼지 않는다.
        Array<GraphEdge> touched(allocator_);
        if (auto copied = touched.TryAppend(edgesA_.View()); !copied) {
            return copied;
        }

        for (const GraphEdge& edge : edgesB_) {
            if (!(edge.toNode == id)) {
                if (auto pushed = touched.TryPushBack(edge); !pushed) {
                    return pushed;
                }
            }
        }

        for (const GraphEdge& edge : touched) {
            if (auto erased = EraseEdge(command.graph, edge); !erased) {
                return erased;
            }
        }

        for (const GraphEdge& edge : touched) {
            if (!(edge.fromNode == id)) {
                if (auto normalized = NormalizeOutputOrder(*graph, edge.fromNode, edge.fromPin);
                    !normalized) {
                    return normalized;
                }
            }

            if (auto pruned = PruneRetainedEnds(*graph, edge, id); !pruned) {
                return pruned;
            }
        }

        GraphPatchOp layout = MakeOp(GraphSlotKind::NodeLayout, command.graph);
        layout.id = id;
        if (auto emitted = Emit(Move(layout)); !emitted) {
            return emitted;
        }

        GraphPatchOp node = MakeOp(GraphSlotKind::Node, command.graph);
        node.id = id;
        if (auto emitted = Emit(Move(node)); !emitted) {
            return emitted;
        }
    }

    return Result<void>::Success();
}

// ---- 간선 ----

Result<void> GraphMutatorCore::Connect(const GraphCommand& command) noexcept {
    Graph* graph = FindGraph(command.graph);
    if (graph == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    const GraphEdge& edge = command.edge;
    const usize fromIndex = NodeIndex(*graph, edge.fromNode);
    const usize toIndex = NodeIndex(*graph, edge.toNode);
    if (fromIndex == IndexNone) {
        return Fail(GraphCommandReason::NodeMissing, command.graph, edge.fromNode);
    }

    if (toIndex == IndexNone) {
        return Fail(GraphCommandReason::NodeMissing, command.graph, edge.toNode);
    }

    auto fromKnown = PinsOf(*graph, graph->nodes[fromIndex], pinsA_);
    if (!fromKnown) {
        return Result<void>::Failure(fromKnown.Error());
    }

    auto toKnown = PinsOf(*graph, graph->nodes[toIndex], pinsB_);
    if (!toKnown) {
        return Result<void>::Failure(toKnown.Error());
    }

    if (!fromKnown.Value()) {
        return Fail(UnknownReason(graph->nodes[fromIndex]), command.graph, edge.fromNode);
    }

    if (!toKnown.Value()) {
        return Fail(UnknownReason(graph->nodes[toIndex]), command.graph, edge.toNode);
    }

    const auto checkEnd = [this, &command](const GraphNode& node, const PinList& pins, Name key,
                                           PinDirection direction,
                                           const PinDescriptor*& out) noexcept -> Result<void> {
        out = pins.Find(key);
        if (out == nullptr) {
            for (const RetainedPin& retained : node.retainedPins) {
                if (retained.key == key) {
                    return Fail(GraphCommandReason::RetainedPin, command.graph, node.id, key);
                }
            }

            return Fail(GraphCommandReason::PinMissing, command.graph, node.id, key);
        }

        if (out->direction != direction) {
            return Fail(GraphCommandReason::PinDirection, command.graph, node.id, key);
        }

        return Result<void>::Success();
    };

    const PinDescriptor* fromPin = nullptr;
    const PinDescriptor* toPin = nullptr;
    if (auto checked =
            checkEnd(graph->nodes[fromIndex], pinsA_, edge.fromPin, PinDirection::Output, fromPin);
        !checked) {
        return checked;
    }

    if (auto checked =
            checkEnd(graph->nodes[toIndex], pinsB_, edge.toPin, PinDirection::Input, toPin);
        !checked) {
        return checked;
    }

    // 핀 서술은 아래 op 적용 동안 바뀔 수 있는 목록을 가리키므로 값으로 잡는다.
    const PinDescriptor from = *fromPin;
    const PinDescriptor to = *toPin;
    return ConnectResolved(*graph, edge, from, to, true, nullptr, command.requireMake);
}

Result<void> GraphMutatorCore::ConnectResolved(Graph& graph, const GraphEdge& edge,
                                               const PinDescriptor& fromPin,
                                               const PinDescriptor& toPin, bool checkSchema,
                                               const uint32* keptOrder, bool requireMake) noexcept {
    bool exists = false;
    if (auto position = EdgeLowerBound(graph, edge, exists); !position) {
        return Result<void>::Failure(position.Error());
    }

    if (exists) {
        return Fail(GraphCommandReason::EdgeExists, graph.id, edge.toNode, edge.toPin);
    }

    auto cycle = RerouteCycle(graph, edge);
    if (!cycle) {
        return Result<void>::Failure(cycle.Error());
    }

    if (cycle.Value()) {
        return Fail(GraphCommandReason::RerouteCycle, graph.id, edge.toNode, edge.toPin);
    }

    edgesA_.Clear();
    if (auto collected = CollectIncoming(graph, edge.toNode, edge.toPin, edgesA_); !collected) {
        return collected;
    }

    edgesB_.Clear();
    if (auto collected = CollectOutgoing(graph, edge.fromNode, edge.fromPin, edgesB_); !collected) {
        return collected;
    }

    ConnectAction action = ConnectAction::Make;
    if (checkSchema) {
        ConnectQuery query;
        query.graph = &graph;
        query.from = PinRef{edge.fromNode, edge.fromPin};
        query.to = PinRef{edge.toNode, edge.toPin};
        query.fromPin = &fromPin;
        query.toPin = &toPin;
        query.toEdgeCount = static_cast<uint32>(edgesA_.Size());
        query.fromEdgeCount = static_cast<uint32>(edgesB_.Size());
        const GraphSchema* schema = registry_.FindSchema(document_.domain);
        const GraphReadView view(document_, registry_);
        const ConnectVerdict verdict =
            schema != nullptr ? schema->CanConnect(view, query) : DefaultConnectVerdict(query);
        action = verdict.action;
        if (action == ConnectAction::Deny || (requireMake && action != ConnectAction::Make)) {
            return Fail(GraphCommandReason::ConnectDenied, graph.id, edge.toNode, edge.toPin);
        }

        if (action == ConnectAction::MakeWithConversion) {
            return InsertConversion(graph, edge, verdict.conversionNodeType);
        }
    }

    // 바꾸기 판정이면 해당 핀의 기존 간선을 같은 patch에서 뺀다.
    Array<GraphEdge> replaced(allocator_);
    if (action == ConnectAction::MakeReplacingInput || action == ConnectAction::MakeReplacingBoth) {
        if (auto copied = replaced.TryAppend(edgesA_.View()); !copied) {
            return copied;
        }
    }

    if (action == ConnectAction::MakeReplacingOutput ||
        action == ConnectAction::MakeReplacingBoth) {
        if (auto copied = replaced.TryAppend(edgesB_.View()); !copied) {
            return copied;
        }
    }

    for (const GraphEdge& old : replaced) {
        if (auto erased = EraseEdge(graph.id, old); !erased) {
            return erased;
        }
    }

    for (const GraphEdge& old : replaced) {
        if (!(old.fromNode == edge.fromNode && old.fromPin == edge.fromPin)) {
            if (auto normalized = NormalizeOutputOrder(graph, old.fromNode, old.fromPin);
                !normalized) {
                return normalized;
            }
        }

        if (auto pruned = PruneRetainedEnds(graph, old, Guid{}); !pruned) {
            return pruned;
        }
    }

    if (graph.edges.Size() >= MaxGraphEdges) {
        return Fail(GraphCommandReason::GraphFull, graph.id, edge.toNode, edge.toPin);
    }

    edgesA_.Clear();
    if (auto collected = CollectIncoming(graph, edge.toNode, edge.toPin, edgesA_); !collected) {
        return collected;
    }

    if (toPin.maxEdges != UnlimitedEdges && edgesA_.Size() >= toPin.maxEdges) {
        return Fail(GraphCommandReason::EdgeLimit, graph.id, edge.toNode, edge.toPin);
    }

    // ordered 출력 핀이면 끝에 붙인다. 앞에서 같은 핀 간선을 뺐으면 남은 것을 먼저 맞춘다.
    if (auto normalized = NormalizeOutputOrder(graph, edge.fromNode, edge.fromPin); !normalized) {
        return normalized;
    }

    edgesB_.Clear();
    if (auto collected = CollectOutgoing(graph, edge.fromNode, edge.fromPin, edgesB_); !collected) {
        return collected;
    }

    GraphPatchOp op = MakeOp(GraphSlotKind::Edge, graph.id);
    op.edgeKey = edge;
    op.edgeKey.order = 0;
    op.edge = op.edgeKey;
    if (keptOrder != nullptr) {
        op.edge.order = *keptOrder;
    } else {
        op.edge.order = fromPin.ordered ? static_cast<uint32>(edgesB_.Size()) : 0;
    }

    op.hasValue = true;
    return Emit(Move(op));
}

// ---- 붙여넣기 명령(9.4절) ----

Result<void> GraphMutatorCore::CheckInsertedRetainedPins(Span<const GraphCommand> list) noexcept {
    for (usize index = 0; index < list.Size(); ++index) {
        const GraphCommand& command = list[index];
        if (command.kind != GraphCommandKind::InsertNode) {
            continue;
        }

        Graph* graph = FindGraph(command.graph);
        if (graph == nullptr) {
            continue;
        }

        const usize nodeIndex = NodeIndex(*graph, command.nodeData.id);
        if (nodeIndex == IndexNone) {
            continue;
        }

        const GraphNode& node = graph->nodes[nodeIndex];
        for (const RetainedPin& pin : node.retainedPins) {
            if (!pin.value.bytes.IsEmpty()) {
                continue;
            }

            edgesA_.Clear();
            const Result<void> collected = pin.direction == PinDirection::Input
                                               ? CollectIncoming(*graph, node.id, pin.key, edgesA_)
                                               : CollectOutgoing(*graph, node.id, pin.key, edgesA_);
            if (!collected) {
                return Result<void>::Failure(collected.Error());
            }

            if (edgesA_.IsEmpty()) {
                commandIndex_ = static_cast<uint32>(index);
                return Fail(GraphCommandReason::NodeDataInvalid, command.graph, node.id, pin.key);
            }
        }
    }

    return Result<void>::Success();
}

Result<bool> GraphMutatorCore::SortNodeLists(GraphNode& node) noexcept {
    NamePool& names = registry_.Names();
    // 작은 목록이라 삽입 정렬로 맞추고 같은 키를 찾는다(비교가 실패할 수 있어 algo::Sort를 쓰지
    // 않는다).
    for (usize index = 1; index < node.inputDefaults.Size(); ++index) {
        for (usize at = index; at > 0; --at) {
            auto compared =
                CompareNames(names, node.inputDefaults[at - 1].pin, node.inputDefaults[at].pin);
            if (!compared) {
                return Result<bool>::Failure(compared.Error());
            }

            if (compared.Value() == 0) {
                return Result<bool>::Success(false);
            }

            if (compared.Value() < 0) {
                break;
            }

            PinValueEntry moved(Move(node.inputDefaults[at]));
            node.inputDefaults[at] = Move(node.inputDefaults[at - 1]);
            node.inputDefaults[at - 1] = Move(moved);
        }
    }

    for (usize index = 1; index < node.retainedPins.Size(); ++index) {
        for (usize at = index; at > 0; --at) {
            auto compared =
                CompareNames(names, node.retainedPins[at - 1].key, node.retainedPins[at].key);
            if (!compared) {
                return Result<bool>::Failure(compared.Error());
            }

            if (compared.Value() == 0) {
                return Result<bool>::Success(false);
            }

            if (compared.Value() < 0) {
                break;
            }

            RetainedPin moved(Move(node.retainedPins[at]));
            node.retainedPins[at] = Move(node.retainedPins[at - 1]);
            node.retainedPins[at - 1] = Move(moved);
        }
    }

    return Result<bool>::Success(true);
}

Result<void> GraphMutatorCore::InsertNode(const GraphCommand& command) noexcept {
    Graph* graph = FindGraph(command.graph);
    if (graph == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    const GraphNode& source = command.nodeData;
    if (CommandIsZero(source.id)) {
        return Fail(GraphCommandReason::IdZero, command.graph);
    }

    if (graph->nodes.Size() >= MaxGraphNodes) {
        return Fail(GraphCommandReason::GraphFull, command.graph, source.id);
    }

    if (IdExists(source.id)) {
        return Fail(GraphCommandReason::NodeExists, command.graph, source.id);
    }

    if (CommandNameNone(source.type)) {
        return Fail(GraphCommandReason::TypeUnknown, command.graph, source.id);
    }

    bool tooLarge = source.properties.bytes.Size() > MaxPropertyRecordBytes;
    for (const PinValueEntry& entry : source.inputDefaults) {
        tooLarge = tooLarge || entry.value.bytes.Size() > MaxPropertyRecordBytes;
    }

    for (const RetainedPin& pin : source.retainedPins) {
        tooLarge = tooLarge || pin.value.bytes.Size() > MaxPropertyRecordBytes;
    }

    if (tooLarge) {
        return Fail(GraphCommandReason::ValueTooLarge, command.graph, source.id);
    }

    if (source.enableState != NodeEnableState::Enabled &&
        source.enableState != NodeEnableState::Disabled) {
        return Fail(GraphCommandReason::EnableState, command.graph, source.id);
    }

    for (const RetainedPin& pin : source.retainedPins) {
        if ((pin.direction != PinDirection::Input && pin.direction != PinDirection::Output) ||
            CommandNameNone(pin.key)) {
            return Fail(GraphCommandReason::NodeDataInvalid, command.graph, source.id, pin.key);
        }
    }

    for (const PinValueEntry& entry : source.inputDefaults) {
        if (CommandNameNone(entry.pin)) {
            return Fail(GraphCommandReason::NodeDataInvalid, command.graph, source.id, entry.pin);
        }
    }

    // 재경유 노드는 고아 핀을 갖지 않는다(GI13).
    if (source.type == registry_.RerouteType() && !source.retainedPins.IsEmpty()) {
        return Fail(GraphCommandReason::NodeDataInvalid, command.graph, source.id);
    }

    GraphPatchOp node = MakeOp(GraphSlotKind::Node, command.graph);
    node.id = source.id;
    node.hasValue = true;
    if (auto cloned = CloneGraphNode(source, node.node); !cloned) {
        return cloned;
    }

    auto sorted = SortNodeLists(node.node);
    if (!sorted) {
        // 키 토큰이 pool에 없으면(낡은 Name) 노드 데이터 오류다. 할당 실패는 그 오류다.
        if (sorted.Error() == ErrorCode::OutOfMemory) {
            return Result<void>::Failure(sorted.Error());
        }

        return Fail(GraphCommandReason::NodeDataInvalid, command.graph, source.id);
    }

    if (!sorted.Value()) {
        return Fail(GraphCommandReason::NodeDataInvalid, command.graph, source.id);
    }

    // 등록된 종류면 AddNode의 종류 규칙과 입력 기본값(GI14)·고아 핀 가림(GI8)을 본다. 모르는
    // 종류(자리 노드)는 바이트 그대로 넣는다.
    const RegisteredNodeType* type = registry_.FindNodeType(document_.domain, source.type);
    if (type != nullptr) {
        const NodeTypeDescriptor& descriptor = type->descriptor;
        if (source.typeSchemaVersion != descriptor.schemaVersion) {
            return Fail(GraphCommandReason::TypeVersion, command.graph, source.id);
        }

        if (!CommandPropertiesMatch(descriptor, source.properties.bytes.View())) {
            return Fail(GraphCommandReason::PropertiesSchema, command.graph, source.id);
        }

        if (source.enableState == NodeEnableState::Disabled &&
            !HasCapability(descriptor.capabilities, NodeCapabilities::AllowDisable)) {
            return Fail(GraphCommandReason::EnableState, command.graph, source.id);
        }

        if (HasCapability(descriptor.capabilities, NodeCapabilities::SinglePerDocument)) {
            for (const Graph& other : document_.graphs) {
                for (const GraphNode& existing : other.nodes) {
                    if (existing.type == source.type) {
                        return Fail(GraphCommandReason::SinglePerDocument, command.graph,
                                    source.id);
                    }
                }
            }
        }

        auto pins = PinsOf(*graph, node.node, pinsA_);
        if (!pins) {
            if (pins.Error() == ErrorCode::OutOfMemory) {
                return Result<void>::Failure(pins.Error());
            }

            return Fail(GraphCommandReason::PropertiesSchema, command.graph, source.id);
        }

        for (const PinValueEntry& entry : node.node.inputDefaults) {
            const PinDescriptor* pin = pinsA_.Find(entry.pin);
            if (pin == nullptr || pin->direction != PinDirection::Input) {
                return Fail(GraphCommandReason::PinMissing, command.graph, source.id, entry.pin);
            }

            if (pin->valuePolicy == PinValuePolicy::Hidden) {
                return Fail(GraphCommandReason::ValueHidden, command.graph, source.id, entry.pin);
            }

            if (entry.value.bytes.IsEmpty()) {
                return Fail(GraphCommandReason::ValueEmpty, command.graph, source.id, entry.pin);
            }
        }

        for (const RetainedPin& pin : node.node.retainedPins) {
            if (pinsA_.Find(pin.key) != nullptr) {
                return Fail(GraphCommandReason::RetainedShadowed, command.graph, source.id,
                            pin.key);
            }
        }
    }

    GraphPatchOp layout = MakeOp(GraphSlotKind::NodeLayout, command.graph);
    layout.id = source.id;
    layout.hasValue = true;
    if (auto cloned = CloneNodeLayout(command.layout, layout.layout); !cloned) {
        return cloned;
    }

    layout.layout.node = source.id;
    if (auto emitted = Emit(Move(node)); !emitted) {
        return emitted;
    }

    return Emit(Move(layout));
}

Result<void> GraphMutatorCore::ResolveInsertEnd(const Graph& graph, const GraphNode& node, Name key,
                                                PinDirection direction, PinList& pins,
                                                PinDescriptor& out, bool& kept) noexcept {
    auto known = PinsOf(graph, node, pins);
    if (!known) {
        return Result<void>::Failure(known.Error());
    }

    // 순번 규칙이 없는 끝(고아 핀·알 수 없는 종류)은 상한·순번 없는 서술로 둔다(6.3절 — 방향만
    // 본다).
    out = PinDescriptor{};
    out.key = key;
    out.direction = direction;
    out.maxEdges = UnlimitedEdges;
    out.ordered = false;
    kept = true;
    if (!known.Value()) {
        return Result<void>::Success();
    }

    if (const PinDescriptor* pin = pins.Find(key); pin != nullptr) {
        if (pin->direction != direction) {
            return Fail(GraphCommandReason::PinDirection, graph.id, node.id, key);
        }

        out = *pin;
        kept = false;
        return Result<void>::Success();
    }

    for (const RetainedPin& retained : node.retainedPins) {
        if (retained.key == key) {
            if (retained.direction != direction) {
                return Fail(GraphCommandReason::PinDirection, graph.id, node.id, key);
            }

            return Result<void>::Success();
        }
    }

    return Fail(GraphCommandReason::PinMissing, graph.id, node.id, key);
}

Result<void> GraphMutatorCore::InsertEdge(const GraphCommand& command) noexcept {
    Graph* graph = FindGraph(command.graph);
    if (graph == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    const GraphEdge& edge = command.edge;
    const usize fromIndex = NodeIndex(*graph, edge.fromNode);
    const usize toIndex = NodeIndex(*graph, edge.toNode);
    if (fromIndex == IndexNone) {
        return Fail(GraphCommandReason::NodeMissing, command.graph, edge.fromNode);
    }

    if (toIndex == IndexNone) {
        return Fail(GraphCommandReason::NodeMissing, command.graph, edge.toNode);
    }

    // 핀 키는 None일 수 없다(GI11 — 알 수 없는 종류의 끝도).
    if (CommandNameNone(edge.fromPin)) {
        return Fail(GraphCommandReason::PinMissing, command.graph, edge.fromNode, edge.fromPin);
    }

    if (CommandNameNone(edge.toPin)) {
        return Fail(GraphCommandReason::PinMissing, command.graph, edge.toNode, edge.toPin);
    }

    PinDescriptor from;
    PinDescriptor to;
    bool fromKept = false;
    bool toKept = false;
    if (auto resolved = ResolveInsertEnd(*graph, graph->nodes[fromIndex], edge.fromPin,
                                         PinDirection::Output, pinsA_, from, fromKept);
        !resolved) {
        return resolved;
    }

    if (auto resolved = ResolveInsertEnd(*graph, graph->nodes[toIndex], edge.toPin,
                                         PinDirection::Input, pinsB_, to, toKept);
        !resolved) {
        return resolved;
    }

    static_cast<void>(toKept);
    return ConnectResolved(*graph, edge, from, to, false, fromKept ? &edge.order : nullptr);
}

Result<void> GraphMutatorCore::InsertConversion(Graph& graph, const GraphEdge& edge,
                                                Name conversionType) noexcept {
    const RegisteredNodeType* type = CommandNameNone(conversionType)
                                         ? nullptr
                                         : registry_.FindNodeType(document_.domain, conversionType);
    if (type == nullptr || !CommandIsZero(type->descriptor.propertiesStruct)) {
        return Fail(GraphCommandReason::ConversionInvalid, graph.id, edge.toNode, edge.toPin);
    }

    if (HasCapability(type->descriptor.capabilities, NodeCapabilities::SinglePerDocument)) {
        for (const Graph& other : document_.graphs) {
            for (const GraphNode& node : other.nodes) {
                if (node.type == conversionType) {
                    return Fail(GraphCommandReason::SinglePerDocument, graph.id, node.id);
                }
            }
        }
    }

    // 변환 노드의 핀은 동적 핀까지 포함한 목록에서 고른다(첫 입력 핀·첫 출력 핀).
    GraphNode probe(allocator_);
    probe.type = conversionType;
    probe.typeSchemaVersion = type->descriptor.schemaVersion;
    PinList conversionPins(allocator_);
    auto pinsKnown = PinsOf(graph, probe, conversionPins);
    if (!pinsKnown) {
        if (pinsKnown.Error() == ErrorCode::OutOfMemory) {
            return Result<void>::Failure(pinsKnown.Error());
        }

        return Fail(GraphCommandReason::ConversionInvalid, graph.id, edge.toNode, edge.toPin);
    }

    const PinDescriptor* input = nullptr;
    const PinDescriptor* output = nullptr;
    for (const PinDescriptor& pin : conversionPins.View()) {
        if (input == nullptr && pin.direction == PinDirection::Input) {
            input = &pin;
        }

        if (output == nullptr && pin.direction == PinDirection::Output) {
            output = &pin;
        }
    }

    if (input == nullptr || output == nullptr) {
        return Fail(GraphCommandReason::ConversionInvalid, graph.id, edge.toNode, edge.toPin);
    }

    auto id = ids_.Next();
    if (!id) {
        return Result<void>::Failure(id.Error());
    }

    if (IdExists(id.Value())) {
        return Fail(GraphCommandReason::NodeExists, graph.id, id.Value());
    }

    // 변환 노드는 두 끝 노드 배치의 가운데에 둔다.
    GraphLayout* layout = FindLayoutGraph(graph.id);
    Vec2 position;
    if (layout != nullptr) {
        const usize fromLayout = LayoutNodeIndex(*layout, edge.fromNode);
        const usize toLayout = LayoutNodeIndex(*layout, edge.toNode);
        if (fromLayout != IndexNone && toLayout != IndexNone) {
            const Vec2 from = layout->nodes[fromLayout].position;
            const Vec2 to = layout->nodes[toLayout].position;
            position = Vec2((from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f);
        }
    }

    GraphPatchOp node = MakeOp(GraphSlotKind::Node, graph.id);
    node.id = id.Value();
    node.hasValue = true;
    node.node.id = id.Value();
    node.node.type = conversionType;
    node.node.typeSchemaVersion = type->descriptor.schemaVersion;
    GraphPatchOp nodeLayout = MakeOp(GraphSlotKind::NodeLayout, graph.id);
    nodeLayout.id = id.Value();
    nodeLayout.hasValue = true;
    nodeLayout.layout.node = id.Value();
    nodeLayout.layout.position = position;
    const PinDescriptor inputPin = *input;
    const PinDescriptor outputPin = *output;
    if (auto emitted = Emit(Move(node)); !emitted) {
        return emitted;
    }

    if (auto emitted = Emit(Move(nodeLayout)); !emitted) {
        return emitted;
    }

    // 원래 끝 핀 서술은 판정 때 값으로 잡아 둔 것을 다시 구한다(변환 노드를 거치는 두 간선은
    // schema를 다시 묻지 않는다 — 변환 노드 선택이 이미 schema 판정이다).
    const usize fromIndex = NodeIndex(graph, edge.fromNode);
    const usize toIndex = NodeIndex(graph, edge.toNode);
    ROCKS_ASSERT(fromIndex != IndexNone && toIndex != IndexNone);
    PinList fromPins(allocator_);
    PinList toPins(allocator_);
    auto fromKnown = PinsOf(graph, graph.nodes[fromIndex], fromPins);
    if (!fromKnown) {
        return Result<void>::Failure(fromKnown.Error());
    }

    auto toKnown = PinsOf(graph, graph.nodes[toIndex], toPins);
    if (!toKnown) {
        return Result<void>::Failure(toKnown.Error());
    }

    const PinDescriptor* fromPin = fromPins.Find(edge.fromPin);
    const PinDescriptor* toPin = toPins.Find(edge.toPin);
    ROCKS_ASSERT(fromPin != nullptr && toPin != nullptr);
    const PinDescriptor from = *fromPin;
    const PinDescriptor to = *toPin;

    GraphEdge first;
    first.fromNode = edge.fromNode;
    first.fromPin = edge.fromPin;
    first.toNode = id.Value();
    first.toPin = inputPin.key;
    if (auto connected = ConnectResolved(graph, first, from, inputPin, false); !connected) {
        return connected;
    }

    GraphEdge second;
    second.fromNode = id.Value();
    second.fromPin = outputPin.key;
    second.toNode = edge.toNode;
    second.toPin = edge.toPin;
    return ConnectResolved(graph, second, outputPin, to, false);
}

Result<void> GraphMutatorCore::Disconnect(const GraphCommand& command) noexcept {
    Graph* graph = FindGraph(command.graph);
    if (graph == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    bool exists = false;
    if (auto position = EdgeLowerBound(*graph, command.edge, exists); !position) {
        return Result<void>::Failure(position.Error());
    }

    if (!exists) {
        return Fail(GraphCommandReason::EdgeMissing, command.graph, command.edge.toNode,
                    command.edge.toPin);
    }

    if (auto erased = EraseEdge(command.graph, command.edge); !erased) {
        return erased;
    }

    if (auto normalized = NormalizeOutputOrder(*graph, command.edge.fromNode, command.edge.fromPin);
        !normalized) {
        return normalized;
    }

    return PruneRetainedEnds(*graph, command.edge, Guid{});
}

Result<void> GraphMutatorCore::ReorderEdges(const GraphCommand& command) noexcept {
    Graph* graph = FindGraph(command.graph);
    if (graph == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    const usize index = NodeIndex(*graph, command.node);
    if (index == IndexNone) {
        return Fail(GraphCommandReason::NodeMissing, command.graph, command.node);
    }

    auto known = PinsOf(*graph, graph->nodes[index], pinsA_);
    if (!known) {
        return Result<void>::Failure(known.Error());
    }

    if (!known.Value()) {
        return Fail(UnknownReason(graph->nodes[index]), command.graph, command.node);
    }

    const PinDescriptor* pin = pinsA_.Find(command.pin);
    if (pin == nullptr) {
        return Fail(GraphCommandReason::PinMissing, command.graph, command.node, command.pin);
    }

    if (pin->direction != PinDirection::Output) {
        return Fail(GraphCommandReason::PinDirection, command.graph, command.node, command.pin);
    }

    if (!pin->ordered) {
        return Fail(GraphCommandReason::NotOrdered, command.graph, command.node, command.pin);
    }

    edgesA_.Clear();
    if (auto collected = CollectOutgoing(*graph, command.node, command.pin, edgesA_); !collected) {
        return collected;
    }

    // 준 순서는 그 핀 간선 전체의 순열이어야 한다.
    if (command.edges.Size() != edgesA_.Size()) {
        return Fail(GraphCommandReason::OrderMismatch, command.graph, command.node, command.pin);
    }

    for (usize position = 0; position < command.edges.Size(); ++position) {
        const GraphEdge& wanted = command.edges[position];
        bool matched = false;
        for (const GraphEdge& current : edgesA_) {
            matched = matched || CommandSameEdgeKey(current, wanted);
        }

        for (usize earlier = 0; matched && earlier < position; ++earlier) {
            matched = !CommandSameEdgeKey(command.edges[earlier], wanted);
        }

        if (!matched) {
            return Fail(GraphCommandReason::OrderMismatch, command.graph, command.node,
                        command.pin);
        }
    }

    Array<GraphEdge> current(allocator_);
    if (auto copied = current.TryAppend(edgesA_.View()); !copied) {
        return copied;
    }

    for (usize position = 0; position < command.edges.Size(); ++position) {
        for (const GraphEdge& edge : current) {
            if (!CommandSameEdgeKey(edge, command.edges[position]) || edge.order == position) {
                continue;
            }

            GraphPatchOp op = MakeOp(GraphSlotKind::Edge, command.graph);
            op.edgeKey = edge;
            op.edge = edge;
            op.edge.order = static_cast<uint32>(position);
            op.hasValue = true;
            if (auto emitted = Emit(Move(op)); !emitted) {
                return emitted;
            }
        }
    }

    return Result<void>::Success();
}

// ---- 노드 값 ----

Result<void> GraphMutatorCore::SetNodeProperties(const GraphCommand& command) noexcept {
    Graph* graph = FindGraph(command.graph);
    if (graph == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    const usize index = NodeIndex(*graph, command.node);
    if (index == IndexNone) {
        return Fail(GraphCommandReason::NodeMissing, command.graph, command.node);
    }

    const GraphNode& current = graph->nodes[index];
    const RegisteredNodeType* type = registry_.FindNodeType(document_.domain, current.type);
    if (type == nullptr) {
        return Fail(GraphCommandReason::TypeUnknown, command.graph, command.node);
    }

    if (current.typeSchemaVersion != type->descriptor.schemaVersion) {
        return Fail(GraphCommandReason::TypeVersion, command.graph, command.node);
    }

    if (command.bytes.Size() > MaxPropertyRecordBytes) {
        return Fail(GraphCommandReason::ValueTooLarge, command.graph, command.node);
    }

    if (!CommandPropertiesMatch(type->descriptor, command.bytes.View())) {
        return Fail(GraphCommandReason::PropertiesSchema, command.graph, command.node);
    }

    // 같은 바이트면 바꿀 것이 없다(op도 통지도 없다).
    if (command.bytes.Size() == current.properties.bytes.Size()) {
        bool same = true;
        for (usize position = 0; same && position < command.bytes.Size(); ++position) {
            same = command.bytes[position] == current.properties.bytes[position];
        }

        if (same) {
            return Result<void>::Success();
        }
    }

    GraphNode changed(allocator_);
    if (auto cloned = CloneGraphNode(current, changed); !cloned) {
        return cloned;
    }

    if (auto copied = CloneBytes(command.bytes, changed.properties.bytes); !copied) {
        return copied;
    }

    // 옛 핀 목록은 고아 핀의 마지막 타입에 쓴다. 사라지는 핀은 고아 핀으로, 같은 키·방향으로 다시
    // 생긴 고아 핀은 재결합한다(10.1절 핀 재정리).
    PinList oldPins(allocator_);
    auto oldKnown = PinsOf(*graph, current, oldPins);
    if (!oldKnown) {
        return Result<void>::Failure(oldKnown.Error());
    }

    return ReconcileNode(*graph, Move(changed), Span<const PinKeyRename>(),
                         oldKnown.Value() ? &oldPins : nullptr, nullptr);
}

Result<void> GraphMutatorCore::SetInputDefault(const GraphCommand& command) noexcept {
    Graph* graph = FindGraph(command.graph);
    if (graph == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    const usize index = NodeIndex(*graph, command.node);
    if (index == IndexNone) {
        return Fail(GraphCommandReason::NodeMissing, command.graph, command.node);
    }

    auto known = PinsOf(*graph, graph->nodes[index], pinsA_);
    if (!known) {
        return Result<void>::Failure(known.Error());
    }

    if (!known.Value()) {
        return Fail(UnknownReason(graph->nodes[index]), command.graph, command.node);
    }

    const PinDescriptor* pin = pinsA_.Find(command.pin);
    if (pin == nullptr) {
        return Fail(GraphCommandReason::PinMissing, command.graph, command.node, command.pin);
    }

    if (pin->direction != PinDirection::Input) {
        return Fail(GraphCommandReason::PinDirection, command.graph, command.node, command.pin);
    }

    if (pin->valuePolicy == PinValuePolicy::Hidden) {
        return Fail(GraphCommandReason::ValueHidden, command.graph, command.node, command.pin);
    }

    if (command.bytes.Size() > MaxPropertyRecordBytes) {
        return Fail(GraphCommandReason::ValueTooLarge, command.graph, command.node, command.pin);
    }

    if (command.bytes.IsEmpty()) {
        return Fail(GraphCommandReason::ValueEmpty, command.graph, command.node, command.pin);
    }

    GraphNode changed(allocator_);
    if (auto cloned = CloneGraphNode(graph->nodes[index], changed); !cloned) {
        return cloned;
    }

    NamePool& names = registry_.Names();
    usize position = 0;
    bool replacing = false;
    for (; position < changed.inputDefaults.Size(); ++position) {
        auto compared = CompareNames(names, changed.inputDefaults[position].pin, command.pin);
        if (!compared) {
            return Result<void>::Failure(compared.Error());
        }

        if (compared.Value() >= 0) {
            replacing = compared.Value() == 0;
            break;
        }
    }

    if (replacing) {
        if (auto copied = CloneBytes(command.bytes, changed.inputDefaults[position].value.bytes);
            !copied) {
            return copied;
        }
    } else {
        PinValueEntry entry(allocator_);
        entry.pin = command.pin;
        if (auto copied = CloneBytes(command.bytes, entry.value.bytes); !copied) {
            return copied;
        }

        if (auto inserted = changed.inputDefaults.TryInsert(Move(entry), position); !inserted) {
            return inserted;
        }
    }

    return ReplaceNode(command.graph, Move(changed));
}

Result<void> GraphMutatorCore::ResetInputDefault(const GraphCommand& command) noexcept {
    Graph* graph = FindGraph(command.graph);
    if (graph == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    const usize index = NodeIndex(*graph, command.node);
    if (index == IndexNone) {
        return Fail(GraphCommandReason::NodeMissing, command.graph, command.node);
    }

    usize found = IndexNone;
    const GraphNode& current = graph->nodes[index];
    for (usize position = 0; position < current.inputDefaults.Size(); ++position) {
        if (current.inputDefaults[position].pin == command.pin) {
            found = position;
        }
    }

    if (found == IndexNone) {
        return Fail(GraphCommandReason::DefaultMissing, command.graph, command.node, command.pin);
    }

    GraphNode changed(allocator_);
    if (auto cloned = CloneGraphNode(current, changed); !cloned) {
        return cloned;
    }

    if (auto removed = changed.inputDefaults.TryRemoveAt(found); !removed) {
        return removed;
    }

    return ReplaceNode(command.graph, Move(changed));
}

Result<void> GraphMutatorCore::SetEnableState(const GraphCommand& command) noexcept {
    Graph* graph = FindGraph(command.graph);
    if (graph == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    const usize index = NodeIndex(*graph, command.node);
    if (index == IndexNone) {
        return Fail(GraphCommandReason::NodeMissing, command.graph, command.node);
    }

    if (command.enableState != NodeEnableState::Enabled &&
        command.enableState != NodeEnableState::Disabled) {
        return Fail(GraphCommandReason::EnableState, command.graph, command.node);
    }

    const GraphNode& current = graph->nodes[index];
    if (command.enableState == NodeEnableState::Disabled) {
        const RegisteredNodeType* type = registry_.FindNodeType(document_.domain, current.type);
        if (type == nullptr) {
            return Fail(GraphCommandReason::TypeUnknown, command.graph, command.node);
        }

        if (current.typeSchemaVersion != type->descriptor.schemaVersion) {
            return Fail(GraphCommandReason::TypeVersion, command.graph, command.node);
        }

        if (!HasCapability(type->descriptor.capabilities, NodeCapabilities::AllowDisable)) {
            return Fail(GraphCommandReason::EnableState, command.graph, command.node);
        }
    }

    if (current.enableState == command.enableState) {
        return Result<void>::Success();
    }

    GraphNode changed(allocator_);
    if (auto cloned = CloneGraphNode(current, changed); !cloned) {
        return cloned;
    }

    changed.enableState = command.enableState;
    return ReplaceNode(command.graph, Move(changed));
}

Result<void> GraphMutatorCore::RemoveRetainedPin(const GraphCommand& command) noexcept {
    Graph* graph = FindGraph(command.graph);
    if (graph == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    const usize index = NodeIndex(*graph, command.node);
    if (index == IndexNone) {
        return Fail(GraphCommandReason::NodeMissing, command.graph, command.node);
    }

    usize found = IndexNone;
    for (usize position = 0; position < graph->nodes[index].retainedPins.Size(); ++position) {
        if (graph->nodes[index].retainedPins[position].key == command.pin) {
            found = position;
        }
    }

    if (found == IndexNone) {
        return Fail(GraphCommandReason::RetainedMissing, command.graph, command.node, command.pin);
    }

    // 그 고아 핀의 간선을 함께 뺀다(방향에 맞는 쪽).
    edgesA_.Clear();
    const PinDirection direction = graph->nodes[index].retainedPins[found].direction;
    if (direction == PinDirection::Input) {
        if (auto collected = CollectIncoming(*graph, command.node, command.pin, edgesA_);
            !collected) {
            return collected;
        }
    } else if (auto collected = CollectOutgoing(*graph, command.node, command.pin, edgesA_);
               !collected) {
        return collected;
    }

    Array<GraphEdge> touched(allocator_);
    if (auto copied = touched.TryAppend(edgesA_.View()); !copied) {
        return copied;
    }

    for (const GraphEdge& edge : touched) {
        if (auto erased = EraseEdge(command.graph, edge); !erased) {
            return erased;
        }
    }

    for (const GraphEdge& edge : touched) {
        if (!(edge.fromNode == command.node)) {
            if (auto normalized = NormalizeOutputOrder(*graph, edge.fromNode, edge.fromPin);
                !normalized) {
                return normalized;
            }
        }
    }

    for (const GraphEdge& edge : touched) {
        if (auto pruned = PruneRetainedEnds(*graph, edge, command.node); !pruned) {
            return pruned;
        }
    }

    const usize nodeIndex = NodeIndex(*graph, command.node);
    GraphNode changed(allocator_);
    if (auto cloned = CloneGraphNode(graph->nodes[nodeIndex], changed); !cloned) {
        return cloned;
    }

    if (auto removed = changed.retainedPins.TryRemoveAt(found); !removed) {
        return removed;
    }

    return ReplaceNode(command.graph, Move(changed));
}

// ---- 배치 ----

Result<void> GraphMutatorCore::SetLayout(const GraphCommand& command) noexcept {
    GraphLayout* layout = FindLayoutGraph(command.graph);
    if (layout == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    if (LayoutNodeIndex(*layout, command.node) == IndexNone) {
        return Fail(GraphCommandReason::NodeMissing, command.graph, command.node);
    }

    GraphPatchOp op = MakeOp(GraphSlotKind::NodeLayout, command.graph);
    op.id = command.node;
    op.hasValue = true;
    if (auto cloned = CloneNodeLayout(command.layout, op.layout); !cloned) {
        return cloned;
    }

    op.layout.node = command.node;
    return Emit(Move(op));
}

Result<void> GraphMutatorCore::AddComment(const GraphCommand& command) noexcept {
    if (FindLayoutGraph(command.graph) == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    if (CommandIsZero(command.comment.id)) {
        return Fail(GraphCommandReason::IdZero, command.graph);
    }

    if (IdExists(command.comment.id)) {
        return Fail(GraphCommandReason::CommentExists, command.graph, command.comment.id);
    }

    GraphPatchOp op = MakeOp(GraphSlotKind::Comment, command.graph);
    op.id = command.comment.id;
    op.hasValue = true;
    if (auto cloned = CloneCommentBox(command.comment, op.comment); !cloned) {
        return cloned;
    }

    return Emit(Move(op));
}

Result<void> GraphMutatorCore::RemoveComment(const GraphCommand& command) noexcept {
    GraphLayout* layout = FindLayoutGraph(command.graph);
    if (layout == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    if (CommentIndex(*layout, command.node) == IndexNone) {
        return Fail(GraphCommandReason::CommentMissing, command.graph, command.node);
    }

    GraphPatchOp op = MakeOp(GraphSlotKind::Comment, command.graph);
    op.id = command.node;
    return Emit(Move(op));
}

Result<void> GraphMutatorCore::SetComment(const GraphCommand& command) noexcept {
    GraphLayout* layout = FindLayoutGraph(command.graph);
    if (layout == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    if (CommentIndex(*layout, command.comment.id) == IndexNone) {
        return Fail(GraphCommandReason::CommentMissing, command.graph, command.comment.id);
    }

    GraphPatchOp op = MakeOp(GraphSlotKind::Comment, command.graph);
    op.id = command.comment.id;
    op.hasValue = true;
    if (auto cloned = CloneCommentBox(command.comment, op.comment); !cloned) {
        return cloned;
    }

    return Emit(Move(op));
}

Result<void> GraphMutatorCore::SetBookmark(const GraphCommand& command) noexcept {
    GraphLayout* layout = FindLayoutGraph(command.graph);
    if (layout == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    const bool exists = BookmarkIndex(*layout, command.bookmark.number) != IndexNone;
    if (!command.hasBookmark && !exists) {
        return Result<void>::Success();
    }

    GraphPatchOp op = MakeOp(GraphSlotKind::Bookmark, command.graph);
    op.number = command.bookmark.number;
    op.hasValue = command.hasBookmark;
    op.bookmark = command.bookmark;
    return Emit(Move(op));
}

// ---- 그래프 ----

Result<void> GraphMutatorCore::AddGraph(const GraphCommand& command) noexcept {
    if (CommandIsZero(command.graph)) {
        return Fail(GraphCommandReason::IdZero, command.graph);
    }

    if (IdExists(command.graph)) {
        return Fail(GraphCommandReason::GraphExists, command.graph);
    }

    const bool roleAllowed =
        command.role == GraphRole::Collapsed || command.role == GraphRole::DomainDefined;
    if (!roleAllowed || (command.role != GraphRole::DomainDefined && command.domainRole != 0)) {
        return Fail(GraphCommandReason::GraphRole, command.graph);
    }

    GraphPatchOp op = MakeOp(GraphSlotKind::Graph, command.graph);
    op.hasValue = true;
    op.graphValue.id = command.graph;
    op.graphValue.role = command.role;
    op.graphValue.domainRole = command.domainRole;
    op.graphLayoutValue.graph = command.graph;
    if (auto copied = op.graphLayoutValue.title.TryAssign(command.title.View()); !copied) {
        return copied;
    }

    return Emit(Move(op));
}

Result<void> GraphMutatorCore::RemoveGraph(const GraphCommand& command) noexcept {
    Graph* graph = FindGraph(command.graph);
    GraphLayout* layout = FindLayoutGraph(command.graph);
    if (graph == nullptr || layout == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    if (command.graph == document_.mainGraph) {
        return Fail(GraphCommandReason::MainGraph, command.graph);
    }

    for (const GraphNode& node : graph->nodes) {
        const RegisteredNodeType* type = registry_.FindNodeType(document_.domain, node.type);
        if (type != nullptr &&
            !HasCapability(type->descriptor.capabilities, NodeCapabilities::CanDelete)) {
            return Fail(GraphCommandReason::CannotDelete, command.graph, node.id);
        }
    }

    // 내용을 먼저 뺀다: 간선 → 노드 배치·노드 → 주석 → 북마크 → 그래프.
    Array<GraphEdge> edges(allocator_);
    if (auto copied = edges.TryAppend(graph->edges.View()); !copied) {
        return copied;
    }

    for (const GraphEdge& edge : edges) {
        if (auto erased = EraseEdge(command.graph, edge); !erased) {
            return erased;
        }
    }

    Array<Guid> nodes(allocator_);
    for (const GraphNode& node : graph->nodes) {
        if (auto pushed = nodes.TryPushBack(node.id); !pushed) {
            return pushed;
        }
    }

    for (const Guid& id : nodes) {
        GraphPatchOp nodeLayout = MakeOp(GraphSlotKind::NodeLayout, command.graph);
        nodeLayout.id = id;
        if (auto emitted = Emit(Move(nodeLayout)); !emitted) {
            return emitted;
        }

        GraphPatchOp node = MakeOp(GraphSlotKind::Node, command.graph);
        node.id = id;
        if (auto emitted = Emit(Move(node)); !emitted) {
            return emitted;
        }
    }

    Array<Guid> comments(allocator_);
    Array<uint32> bookmarks(allocator_);
    GraphLayout* current = FindLayoutGraph(command.graph);
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
        GraphPatchOp op = MakeOp(GraphSlotKind::Comment, command.graph);
        op.id = id;
        if (auto emitted = Emit(Move(op)); !emitted) {
            return emitted;
        }
    }

    for (const uint32 number : bookmarks) {
        GraphPatchOp op = MakeOp(GraphSlotKind::Bookmark, command.graph);
        op.number = number;
        if (auto emitted = Emit(Move(op)); !emitted) {
            return emitted;
        }
    }

    GraphPatchOp op = MakeOp(GraphSlotKind::Graph, command.graph);
    return Emit(Move(op));
}

Result<void> GraphMutatorCore::RenameGraph(const GraphCommand& command) noexcept {
    if (FindLayoutGraph(command.graph) == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    GraphPatchOp op = MakeOp(GraphSlotKind::GraphTitle, command.graph);
    op.hasValue = true;
    if (auto copied = op.title.TryAssign(command.title.View()); !copied) {
        return copied;
    }

    return Emit(Move(op));
}

Result<void> GraphMutatorCore::MoveNodesToGraph(const GraphCommand& command) noexcept {
    Graph* source = FindGraph(command.graph);
    if (source == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.graph);
    }

    if (FindGraph(command.target) == nullptr) {
        return Fail(GraphCommandReason::GraphMissing, command.target);
    }

    if (command.target == command.graph) {
        return Fail(GraphCommandReason::SameGraph, command.graph);
    }

    auto duplicate = FindDuplicate(command.nodes.View());
    if (!duplicate) {
        return Result<void>::Failure(duplicate.Error());
    }

    if (!CommandIsZero(duplicate.Value())) {
        return Fail(GraphCommandReason::DuplicateNode, command.graph, duplicate.Value());
    }

    if (FindGraph(command.target)->nodes.Size() + command.nodes.Size() > MaxGraphNodes) {
        return Fail(GraphCommandReason::GraphFull, command.target);
    }

    const auto moving = [&command](Guid id) noexcept {
        for (const Guid& candidate : command.nodes) {
            if (candidate == id) {
                return true;
            }
        }

        return false;
    };

    // 노드가 원본에 있고, 옮기는 노드와 나머지 사이 간선이 없어야 한다. 그 사이 간선은 함께 옮긴다.
    Array<GraphEdge> internal(allocator_);
    for (const Guid& id : command.nodes) {
        if (NodeIndex(*source, id) == IndexNone) {
            return Fail(GraphCommandReason::NodeMissing, command.graph, id);
        }

        edgesA_.Clear();
        if (auto collected = CollectIncoming(*source, id, Name(), edgesA_); !collected) {
            return collected;
        }

        for (const GraphEdge& edge : edgesA_) {
            if (!moving(edge.fromNode)) {
                return Fail(GraphCommandReason::CrossingEdge, command.graph, id, edge.toPin);
            }

            if (auto pushed = internal.TryPushBack(edge); !pushed) {
                return pushed;
            }
        }

        edgesA_.Clear();
        if (auto collected = CollectOutgoing(*source, id, Name(), edgesA_); !collected) {
            return collected;
        }

        for (const GraphEdge& edge : edgesA_) {
            if (!moving(edge.toNode)) {
                return Fail(GraphCommandReason::CrossingEdge, command.graph, id, edge.fromPin);
            }
        }
    }

    for (const GraphEdge& edge : internal) {
        if (auto erased = EraseEdge(command.graph, edge); !erased) {
            return erased;
        }
    }

    for (const Guid& id : command.nodes) {
        // 사본을 대상에 넣을 수 있게 먼저 복사한 뒤 원본에서 뺀다(같은 id가 문서에 둘이 되지 않게
        // 빼기가 먼저다).
        Graph* from = FindGraph(command.graph);
        const usize index = NodeIndex(*from, id);
        GraphPatchOp insertNode = MakeOp(GraphSlotKind::Node, command.target);
        insertNode.id = id;
        insertNode.hasValue = true;
        if (auto cloned = CloneGraphNode(from->nodes[index], insertNode.node); !cloned) {
            return cloned;
        }

        GraphPatchOp insertLayout = MakeOp(GraphSlotKind::NodeLayout, command.target);
        insertLayout.id = id;
        insertLayout.hasValue = true;
        GraphLayout* fromLayout = FindLayoutGraph(command.graph);
        const usize layoutIndex = LayoutNodeIndex(*fromLayout, id);
        if (layoutIndex != IndexNone) {
            if (auto cloned = CloneNodeLayout(fromLayout->nodes[layoutIndex], insertLayout.layout);
                !cloned) {
                return cloned;
            }
        }

        insertLayout.layout.node = id;
        GraphPatchOp eraseLayout = MakeOp(GraphSlotKind::NodeLayout, command.graph);
        eraseLayout.id = id;
        GraphPatchOp eraseNode = MakeOp(GraphSlotKind::Node, command.graph);
        eraseNode.id = id;
        if (auto emitted = Emit(Move(eraseLayout)); !emitted) {
            return emitted;
        }

        if (auto emitted = Emit(Move(eraseNode)); !emitted) {
            return emitted;
        }

        if (auto emitted = Emit(Move(insertNode)); !emitted) {
            return emitted;
        }

        if (auto emitted = Emit(Move(insertLayout)); !emitted) {
            return emitted;
        }
    }

    for (const GraphEdge& edge : internal) {
        GraphPatchOp op = MakeOp(GraphSlotKind::Edge, command.target);
        op.edgeKey = edge;
        op.edgeKey.order = 0;
        op.edge = edge;
        op.hasValue = true;
        if (auto emitted = Emit(Move(op)); !emitted) {
            return emitted;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::nodegraph::detail
