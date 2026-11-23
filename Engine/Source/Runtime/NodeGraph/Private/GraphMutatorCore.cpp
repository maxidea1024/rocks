#include "GraphMutatorCore.h"

#include <Base/NumericLimits.h>
#include <Core/Algorithms.h>
#include <Core/Assert.h>
#include <Core/Hash.h>
#include <Core/Text/NamePool.h>
#include <NodeGraph/GraphMutator.h>
#include <NodeGraph/GraphNameOrder.h>
#include <NodeGraph/GraphValidation.h>

// GraphMutator의 색인·op 적용·명령
// 실행·undo·redo·bracket·통지다(Docs/Design/NodeGraph 8.2·8.6절).
namespace rocks::nodegraph {

namespace detail {

namespace {

// 같은 pool 안에서 Name 같음과 일치하는 토큰 순서다(메모리 색인 전용).
int32 CoreCompareTokens(Name left, Name right) noexcept {
    const uint8 leftNormal = left.kind == NameKind::Normal ? 1 : 0;
    const uint8 rightNormal = right.kind == NameKind::Normal ? 1 : 0;
    if (leftNormal != rightNormal) {
        return leftNormal < rightNormal ? -1 : 1;
    }

    const uint64 leftId = leftNormal != 0 ? left.id : 0;
    const uint64 rightId = rightNormal != 0 ? right.id : 0;
    if (leftId != rightId) {
        return leftId < rightId ? -1 : 1;
    }

    if (left.number != right.number) {
        return left.number < right.number ? -1 : 1;
    }

    return 0;
}

int32 CoreCompareOutgoing(const OutgoingEdge& left, const OutgoingEdge& right) noexcept {
    if (const int32 compared = CompareGuid(left.graph, right.graph); compared != 0) {
        return compared;
    }

    if (const int32 compared = CompareGuid(left.from, right.from); compared != 0) {
        return compared;
    }

    if (const int32 compared = CoreCompareTokens(left.fromPin, right.fromPin); compared != 0) {
        return compared;
    }

    if (const int32 compared = CompareGuid(left.to, right.to); compared != 0) {
        return compared;
    }

    return CoreCompareTokens(left.toPin, right.toPin);
}

// 간선 정규 순서 비교다(입력 NodeId, 입력 PinKey, 출력 NodeId, 출력 PinKey — 6.3절).
Result<int32> CoreCompareEdges(NamePool& names, const GraphEdge& left,
                               const GraphEdge& right) noexcept {
    if (const int32 compared = CompareGuid(left.toNode, right.toNode); compared != 0) {
        return Result<int32>::Success(compared);
    }

    auto toPin = CompareNames(names, left.toPin, right.toPin);
    if (!toPin || toPin.Value() != 0) {
        return toPin;
    }

    if (const int32 compared = CompareGuid(left.fromNode, right.fromNode); compared != 0) {
        return Result<int32>::Success(compared);
    }

    return CompareNames(names, left.fromPin, right.fromPin);
}

// id 순 배열에서 id의 하한 위치다.
template <class T, class GetId>
usize CoreLowerBound(const Array<T>& items, Guid id, GetId&& getId) noexcept {
    usize low = 0;
    usize high = items.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (CompareGuid(getId(items[middle]), id) < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return low;
}

template <class T, class GetId>
usize CoreFind(const Array<T>& items, Guid id, GetId&& getId) noexcept {
    const usize index = CoreLowerBound(items, id, getId);
    return index < items.Size() && getId(items[index]) == id ? index : IndexNone;
}

template <class T>
void CoreSwap(T& left, T& right) noexcept {
    T temporary(Move(left));
    left = Move(right);
    right = Move(temporary);
}

bool CoreSameBytes(const Array<uint8>& left, const Array<uint8>& right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

// op 키의 hash다(합치기 표). 충돌하면 합치지 않으므로 정확성에 영향이 없다.
uint64 CoreMix(uint64 hash, uint64 value) noexcept {
    return HashCombine(hash, value);
}

uint64 CoreNameHash(Name name) noexcept {
    return name.kind == NameKind::Normal ? CoreMix(name.id, name.number) : name.number;
}

uint64 CoreOpKeyHash(const GraphPatchOp& op) noexcept {
    uint64 hash = static_cast<uint64>(op.kind) + 1;
    hash = CoreMix(hash, op.graph.high);
    hash = CoreMix(hash, op.graph.low);
    hash = CoreMix(hash, op.id.high);
    hash = CoreMix(hash, op.id.low);
    hash = CoreMix(hash, op.number);
    if (op.kind == GraphSlotKind::Edge) {
        hash = CoreMix(hash, op.edgeKey.fromNode.low);
        hash = CoreMix(hash, op.edgeKey.toNode.low);
        hash = CoreMix(hash, CoreNameHash(op.edgeKey.fromPin));
        hash = CoreMix(hash, CoreNameHash(op.edgeKey.toPin));
    }

    return hash;
}

bool CoreSameOpKey(const GraphPatchOp& left, const GraphPatchOp& right) noexcept {
    if (left.kind != right.kind || !(left.graph == right.graph) || !(left.id == right.id) ||
        left.number != right.number) {
        return false;
    }

    if (left.kind != GraphSlotKind::Edge) {
        return true;
    }

    return left.edgeKey.fromNode == right.edgeKey.fromNode &&
           left.edgeKey.fromPin == right.edgeKey.fromPin &&
           left.edgeKey.toNode == right.edgeKey.toNode && left.edgeKey.toPin == right.edgeKey.toPin;
}

bool CoreSemanticSlot(GraphSlotKind kind) noexcept {
    return kind == GraphSlotKind::Graph || kind == GraphSlotKind::Node ||
           kind == GraphSlotKind::Edge;
}

} // namespace

ConnectVerdict DefaultConnectVerdict(const ConnectQuery& query) noexcept {
    ConnectVerdict verdict;
    if (query.fromPin == nullptr || query.toPin == nullptr ||
        query.fromPin->kind != query.toPin->kind) {
        return verdict;
    }

    const bool typesMatch = query.fromPin->type == query.toPin->type ||
                            query.fromPin->type == UnresolvedPinType ||
                            query.toPin->type == UnresolvedPinType;
    if (!typesMatch) {
        return verdict;
    }

    verdict.action = query.toEdgeCount >= query.toPin->maxEdges ? ConnectAction::MakeReplacingInput
                                                                : ConnectAction::Make;
    return verdict;
}

GraphMutatorCore::GraphMutatorCore(GraphDocument& document, GraphDocumentLayout& layout,
                                   const NodeTypeRegistry& registry, GraphIdSource& ids,
                                   Allocator& allocator) noexcept
    : document_(document), layout_(layout), registry_(registry), ids_(ids), allocator_(allocator),
      idIndex_(allocator), outgoing_(allocator), listeners_(allocator), pending_(allocator),
      work_(allocator), interactionPatch_(allocator), interactionKeys_(allocator),
      pinsA_(allocator), pinsB_(allocator), edgesA_(allocator), edgesB_(allocator) {
}

// ---- 초기화·조회 ----

Result<void> GraphMutatorCore::Initialize() noexcept {
    if (initialized_) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    GraphViolationList violations(allocator_);
    if (auto valid = ValidateGraphDocument(document_, &layout_, registry_, allocator_, violations);
        !valid) {
        return valid;
    }

    idIndex_.Clear();
    outgoing_.Clear();
    for (const Graph& graph : document_.graphs) {
        if (auto pushed = idIndex_.TryPushBack(graph.id); !pushed) {
            return pushed;
        }

        for (const GraphNode& node : graph.nodes) {
            if (auto pushed = idIndex_.TryPushBack(node.id); !pushed) {
                return pushed;
            }
        }

        for (const GraphEdge& edge : graph.edges) {
            const OutgoingEdge entry{graph.id, edge.fromNode, edge.fromPin, edge.toNode,
                                     edge.toPin};
            if (auto pushed = outgoing_.TryPushBack(entry); !pushed) {
                return pushed;
            }
        }
    }

    for (const GraphLayout& graph : layout_.graphs) {
        for (const CommentBox& comment : graph.comments) {
            if (auto pushed = idIndex_.TryPushBack(comment.id); !pushed) {
                return pushed;
            }
        }
    }

    algo::Sort(idIndex_.View(), [](const Guid& left, const Guid& right) noexcept {
        return CompareGuid(left, right) < 0;
    });
    algo::Sort(outgoing_.View(), [](const OutgoingEdge& left, const OutgoingEdge& right) noexcept {
        return CoreCompareOutgoing(left, right) < 0;
    });

    interactionPatch_.owner_ = this;
    revision_ = 0;
    initialized_ = true;
    return Result<void>::Success();
}

usize GraphMutatorCore::GraphIndex(Guid id) const noexcept {
    return CoreFind(document_.graphs, id, [](const Graph& graph) noexcept { return graph.id; });
}

Graph* GraphMutatorCore::FindGraph(Guid id) noexcept {
    const usize index = GraphIndex(id);
    return index == IndexNone ? nullptr : &document_.graphs[index];
}

GraphLayout* GraphMutatorCore::FindLayoutGraph(Guid id) noexcept {
    const usize index =
        CoreFind(layout_.graphs, id, [](const GraphLayout& graph) noexcept { return graph.graph; });
    return index == IndexNone ? nullptr : &layout_.graphs[index];
}

usize GraphMutatorCore::NodeIndex(const Graph& graph, Guid id) noexcept {
    return CoreFind(graph.nodes, id, [](const GraphNode& node) noexcept { return node.id; });
}

usize GraphMutatorCore::LayoutNodeIndex(const GraphLayout& layout, Guid id) noexcept {
    return CoreFind(layout.nodes, id, [](const NodeLayout& node) noexcept { return node.node; });
}

usize GraphMutatorCore::CommentIndex(const GraphLayout& layout, Guid id) noexcept {
    return CoreFind(layout.comments, id,
                    [](const CommentBox& comment) noexcept { return comment.id; });
}

usize GraphMutatorCore::BookmarkIndex(const GraphLayout& layout, uint32 number) noexcept {
    for (usize index = 0; index < layout.bookmarks.Size(); ++index) {
        if (layout.bookmarks[index].number == number) {
            return index;
        }
    }

    return IndexNone;
}

bool GraphMutatorCore::IdExists(Guid id) const noexcept {
    return CoreFind(idIndex_, id, [](const Guid& value) noexcept { return value; }) != IndexNone;
}

Result<usize> GraphMutatorCore::EdgeLowerBound(const Graph& graph, const GraphEdge& key,
                                               bool& found) const noexcept {
    NamePool& names = registry_.Names();
    usize low = 0;
    usize high = graph.edges.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        auto compared = CoreCompareEdges(names, graph.edges[middle], key);
        if (!compared) {
            return Result<usize>::Failure(compared.Error());
        }

        if (compared.Value() < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    found = false;
    if (low < graph.edges.Size()) {
        auto compared = CoreCompareEdges(names, graph.edges[low], key);
        if (!compared) {
            return Result<usize>::Failure(compared.Error());
        }

        found = compared.Value() == 0;
    }

    return Result<usize>::Success(low);
}

Result<void> GraphMutatorCore::CollectIncoming(const Graph& graph, Guid node, Name pin,
                                               Array<GraphEdge>& out) const noexcept {
    usize index = CoreLowerBound(graph.edges, node,
                                 [](const GraphEdge& edge) noexcept { return edge.toNode; });
    for (; index < graph.edges.Size() && graph.edges[index].toNode == node; ++index) {
        if (pin.IsNone() || graph.edges[index].toPin == pin) {
            if (auto pushed = out.TryPushBack(graph.edges[index]); !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> GraphMutatorCore::CollectOutgoing(const Graph& graph, Guid node, Name pin,
                                               Array<GraphEdge>& out) noexcept {
    // 색인에서 (graph, node) 범위를 찾고, 간선 값(순번)은 그래프에서 읽는다.
    usize low = 0;
    usize high = outgoing_.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const OutgoingEdge& entry = outgoing_[middle];
        int32 compared = CompareGuid(entry.graph, graph.id);
        if (compared == 0) {
            compared = CompareGuid(entry.from, node);
        }

        if (compared < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    for (usize index = low; index < outgoing_.Size(); ++index) {
        const OutgoingEdge& entry = outgoing_[index];
        if (!(entry.graph == graph.id) || !(entry.from == node)) {
            break;
        }

        if (!pin.IsNone() && !(entry.fromPin == pin)) {
            continue;
        }

        GraphEdge key;
        key.fromNode = entry.from;
        key.fromPin = entry.fromPin;
        key.toNode = entry.to;
        key.toPin = entry.toPin;
        bool found = false;
        auto position = EdgeLowerBound(graph, key, found);
        if (!position) {
            return Result<void>::Failure(position.Error());
        }

        ROCKS_ASSERT(found);
        if (auto pushed = out.TryPushBack(graph.edges[position.Value()]); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

Result<bool> GraphMutatorCore::PinsOf(const Graph& graph, const GraphNode& node,
                                      PinList& out) const noexcept {
    auto collected = registry_.CollectPins(document_, graph, node, out);
    if (!collected) {
        if (collected.Error() == ErrorCode::NotFound) {
            return Result<bool>::Success(false);
        }

        return Result<bool>::Failure(collected.Error());
    }

    return Result<bool>::Success(true);
}

// ---- op 적용 ----

GraphPatchOp GraphMutatorCore::MakeOp(GraphSlotKind kind, Guid graph) noexcept {
    GraphPatchOp op(allocator_);
    op.kind = kind;
    op.graph = graph;
    return op;
}

Result<void> GraphMutatorCore::Record(GraphChangeKind kind, Guid graph, Guid node, Guid other,
                                      Name pin, Name otherPin, uint32 number) noexcept {
    GraphChange change;
    change.kind = kind;
    change.graph = graph;
    change.node = node;
    change.other = other;
    change.pin = pin;
    change.otherPin = otherPin;
    change.number = number;
    // EdgeOrderChanged는 출력 핀 단위다(8.3절): 같은 묶음에 같은 핀이 이미 있으면 다시 넣지 않는다.
    if (kind == GraphChangeKind::EdgeOrderChanged) {
        for (const GraphChange& existing : pending_.changes) {
            if (existing.kind == kind && existing.graph == graph && existing.node == node &&
                existing.pin == pin) {
                return Result<void>::Success();
            }
        }
    }

    return pending_.changes.TryPushBack(change);
}

Result<void> GraphMutatorCore::IndexId(Guid id) noexcept {
    const usize position =
        CoreLowerBound(idIndex_, id, [](const Guid& value) noexcept { return value; });
    return idIndex_.TryInsert(id, position);
}

void GraphMutatorCore::UnindexId(Guid id) noexcept {
    const usize position = CoreFind(idIndex_, id, [](const Guid& value) noexcept { return value; });
    if (position != IndexNone) {
        static_cast<void>(idIndex_.TryRemoveAt(position));
    }
}

Result<usize> GraphMutatorCore::OutgoingSlot(const OutgoingEdge& key, bool& found) const noexcept {
    usize low = 0;
    usize high = outgoing_.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (CoreCompareOutgoing(outgoing_[middle], key) < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    found = low < outgoing_.Size() && CoreCompareOutgoing(outgoing_[low], key) == 0;
    return Result<usize>::Success(low);
}

Result<void> GraphMutatorCore::ApplyGraphOp(GraphPatchOp& op, bool record) noexcept {
    const usize index = GraphIndex(op.graph);
    const usize layoutIndex = CoreFind(
        layout_.graphs, op.graph, [](const GraphLayout& graph) noexcept { return graph.graph; });
    if (op.hasValue && index == IndexNone) {
        if (auto reserved = document_.graphs.TryEnsureCapacity(document_.graphs.Size() + 1);
            !reserved) {
            return reserved;
        }

        if (auto reserved = layout_.graphs.TryEnsureCapacity(layout_.graphs.Size() + 1);
            !reserved) {
            return reserved;
        }

        if (auto reserved = idIndex_.TryEnsureCapacity(idIndex_.Size() + 1); !reserved) {
            return reserved;
        }

        const usize position = CoreLowerBound(document_.graphs, op.graph,
                                              [](const Graph& graph) noexcept { return graph.id; });
        const usize layoutPosition =
            CoreLowerBound(layout_.graphs, op.graph,
                           [](const GraphLayout& graph) noexcept { return graph.graph; });
        ROCKS_ASSERT(document_.graphs.TryInsert(Move(op.graphValue), position));
        ROCKS_ASSERT(layout_.graphs.TryInsert(Move(op.graphLayoutValue), layoutPosition));
        ROCKS_ASSERT(IndexId(op.graph));
        op.hasValue = false;
        op.replaced = false;
        if (record) {
            if (auto recorded = Record(GraphChangeKind::GraphAdded, op.graph, Guid{}, Guid{},
                                       Name(), Name(), 0);
                !recorded) {
                ROCKS_ASSERT(ApplyGraphOp(op, false));
                return recorded;
            }
        }

        return Result<void>::Success();
    }

    if (!op.hasValue && index != IndexNone && layoutIndex != IndexNone) {
        op.graphValue = Move(document_.graphs[index]);
        op.graphLayoutValue = Move(layout_.graphs[layoutIndex]);
        ROCKS_ASSERT(document_.graphs.TryRemoveAt(index));
        ROCKS_ASSERT(layout_.graphs.TryRemoveAt(layoutIndex));
        UnindexId(op.graph);
        op.hasValue = true;
        op.replaced = false;
        if (record) {
            if (auto recorded = Record(GraphChangeKind::GraphRemoved, op.graph, Guid{}, Guid{},
                                       Name(), Name(), 0);
                !recorded) {
                ROCKS_ASSERT(ApplyGraphOp(op, false));
                return recorded;
            }
        }

        return Result<void>::Success();
    }

    return Result<void>::Failure(ErrorCode::StaleHandle);
}

Result<void> GraphMutatorCore::ApplyTitleOp(GraphPatchOp& op, bool record) noexcept {
    GraphLayout* layout = FindLayoutGraph(op.graph);
    if (layout == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    CoreSwap(layout->title, op.title);
    op.replaced = true;
    if (record) {
        if (auto recorded =
                Record(GraphChangeKind::GraphRenamed, op.graph, Guid{}, Guid{}, Name(), Name(), 0);
            !recorded) {
            CoreSwap(layout->title, op.title);
            return recorded;
        }
    }

    return Result<void>::Success();
}

Result<void> GraphMutatorCore::RecordNodeChanges(Guid graph, const GraphNode& before,
                                                 const GraphNode& after) noexcept {
    if (!CoreSameBytes(before.properties.bytes, after.properties.bytes)) {
        if (auto recorded = Record(GraphChangeKind::NodePropertiesChanged, graph, after.id, Guid{},
                                   Name(), Name(), 0);
            !recorded) {
            return recorded;
        }

        // 동적 핀이 있는 종류면 핀 목록이 달라졌는지 본다.
        const RegisteredNodeType* type = registry_.FindNodeType(document_.domain, after.type);
        const Graph* owner = FindGraph(graph);
        if (type != nullptr && type->descriptor.dynamicPins != nullptr && owner != nullptr) {
            PinList oldPins(allocator_);
            PinList newPins(allocator_);
            auto oldCollected = registry_.CollectPins(document_, *owner, before, oldPins);
            auto newCollected = registry_.CollectPins(document_, *owner, after, newPins);
            bool differ = !oldCollected || !newCollected || oldPins.Size() != newPins.Size();
            for (usize index = 0; !differ && index < oldPins.Size(); ++index) {
                const PinDescriptor& left = oldPins.View()[index];
                const PinDescriptor& right = newPins.View()[index];
                differ = !(left.key == right.key) || left.direction != right.direction ||
                         left.type != right.type || left.kind != right.kind ||
                         left.maxEdges != right.maxEdges || left.ordered != right.ordered;
            }

            if (differ) {
                if (auto recorded = Record(GraphChangeKind::NodePinsChanged, graph, after.id,
                                           Guid{}, Name(), Name(), 0);
                    !recorded) {
                    return recorded;
                }
            }
        }
    }

    // 입력 기본값: 한쪽에만 있거나 값이 다른 핀마다.
    for (const PinValueEntry& entry : before.inputDefaults) {
        const PinValueEntry* match = nullptr;
        for (const PinValueEntry& candidate : after.inputDefaults) {
            if (candidate.pin == entry.pin) {
                match = &candidate;
                break;
            }
        }

        if (match == nullptr || !CoreSameBytes(match->value.bytes, entry.value.bytes)) {
            if (auto recorded = Record(GraphChangeKind::InputDefaultChanged, graph, after.id,
                                       Guid{}, entry.pin, Name(), 0);
                !recorded) {
                return recorded;
            }
        }
    }

    for (const PinValueEntry& entry : after.inputDefaults) {
        bool existed = false;
        for (const PinValueEntry& candidate : before.inputDefaults) {
            existed = existed || candidate.pin == entry.pin;
        }

        if (!existed) {
            if (auto recorded = Record(GraphChangeKind::InputDefaultChanged, graph, after.id,
                                       Guid{}, entry.pin, Name(), 0);
                !recorded) {
                return recorded;
            }
        }
    }

    if (before.enableState != after.enableState) {
        if (auto recorded = Record(GraphChangeKind::NodeEnableStateChanged, graph, after.id, Guid{},
                                   Name(), Name(), 0);
            !recorded) {
            return recorded;
        }
    }

    // 고아 핀: 한쪽에만 있거나 내용이 다른 키마다.
    for (const RetainedPin& pin : before.retainedPins) {
        const RetainedPin* match = nullptr;
        for (const RetainedPin& candidate : after.retainedPins) {
            if (candidate.key == pin.key) {
                match = &candidate;
                break;
            }
        }

        if (match == nullptr || match->direction != pin.direction ||
            match->lastType != pin.lastType ||
            !CoreSameBytes(match->value.bytes, pin.value.bytes)) {
            if (auto recorded = Record(GraphChangeKind::RetainedPinChanged, graph, after.id, Guid{},
                                       pin.key, Name(), 0);
                !recorded) {
                return recorded;
            }
        }
    }

    for (const RetainedPin& pin : after.retainedPins) {
        bool existed = false;
        for (const RetainedPin& candidate : before.retainedPins) {
            existed = existed || candidate.key == pin.key;
        }

        if (!existed) {
            if (auto recorded = Record(GraphChangeKind::RetainedPinChanged, graph, after.id, Guid{},
                                       pin.key, Name(), 0);
                !recorded) {
                return recorded;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> GraphMutatorCore::ApplyNodeOp(GraphPatchOp& op, bool record) noexcept {
    Graph* graph = FindGraph(op.graph);
    if (graph == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    const usize index = NodeIndex(*graph, op.id);
    if (op.hasValue && index == IndexNone) {
        if (auto reserved = graph->nodes.TryEnsureCapacity(graph->nodes.Size() + 1); !reserved) {
            return reserved;
        }

        if (auto reserved = idIndex_.TryEnsureCapacity(idIndex_.Size() + 1); !reserved) {
            return reserved;
        }

        const usize position = CoreLowerBound(
            graph->nodes, op.id, [](const GraphNode& node) noexcept { return node.id; });
        ROCKS_ASSERT(graph->nodes.TryInsert(Move(op.node), position));
        ROCKS_ASSERT(IndexId(op.id));
        op.hasValue = false;
        op.replaced = false;
        if (record) {
            if (auto recorded =
                    Record(GraphChangeKind::NodeAdded, op.graph, op.id, Guid{}, Name(), Name(), 0);
                !recorded) {
                ROCKS_ASSERT(ApplyNodeOp(op, false));
                return recorded;
            }
        }

        return Result<void>::Success();
    }

    if (!op.hasValue && index != IndexNone) {
        op.node = Move(graph->nodes[index]);
        ROCKS_ASSERT(graph->nodes.TryRemoveAt(index));
        UnindexId(op.id);
        op.hasValue = true;
        op.replaced = false;
        if (record) {
            if (auto recorded = Record(GraphChangeKind::NodeRemoved, op.graph, op.id, Guid{},
                                       Name(), Name(), 0);
                !recorded) {
                ROCKS_ASSERT(ApplyNodeOp(op, false));
                return recorded;
            }
        }

        return Result<void>::Success();
    }

    if (op.hasValue && index != IndexNone) {
        CoreSwap(graph->nodes[index], op.node);
        op.replaced = true;
        if (record) {
            const usize before = pending_.changes.Size();
            if (auto recorded = RecordNodeChanges(op.graph, op.node, graph->nodes[index]);
                !recorded) {
                static_cast<void>(pending_.changes.TryResize(before));
                CoreSwap(graph->nodes[index], op.node);
                return recorded;
            }
        }

        return Result<void>::Success();
    }

    return Result<void>::Failure(ErrorCode::StaleHandle);
}

Result<void> GraphMutatorCore::ApplyLayoutOp(GraphPatchOp& op, bool record) noexcept {
    GraphLayout* layout = FindLayoutGraph(op.graph);
    if (layout == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    const usize index = LayoutNodeIndex(*layout, op.id);
    if (op.hasValue && index == IndexNone) {
        if (auto reserved = layout->nodes.TryEnsureCapacity(layout->nodes.Size() + 1); !reserved) {
            return reserved;
        }

        const usize position = CoreLowerBound(
            layout->nodes, op.id, [](const NodeLayout& node) noexcept { return node.node; });
        ROCKS_ASSERT(layout->nodes.TryInsert(Move(op.layout), position));
        op.hasValue = false;
        op.replaced = false;
        return Result<void>::Success();
    }

    if (!op.hasValue && index != IndexNone) {
        op.layout = Move(layout->nodes[index]);
        ROCKS_ASSERT(layout->nodes.TryRemoveAt(index));
        op.hasValue = true;
        op.replaced = false;
        return Result<void>::Success();
    }

    if (op.hasValue && index != IndexNone) {
        CoreSwap(layout->nodes[index], op.layout);
        op.replaced = true;
        if (record) {
            if (auto recorded = Record(GraphChangeKind::NodeLayoutChanged, op.graph, op.id, Guid{},
                                       Name(), Name(), 0);
                !recorded) {
                CoreSwap(layout->nodes[index], op.layout);
                return recorded;
            }
        }

        return Result<void>::Success();
    }

    return Result<void>::Failure(ErrorCode::StaleHandle);
}

Result<void> GraphMutatorCore::ApplyEdgeOp(GraphPatchOp& op, bool record) noexcept {
    Graph* graph = FindGraph(op.graph);
    if (graph == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    bool found = false;
    auto position = EdgeLowerBound(*graph, op.edgeKey, found);
    if (!position) {
        return Result<void>::Failure(position.Error());
    }

    const OutgoingEdge key{op.graph, op.edgeKey.fromNode, op.edgeKey.fromPin, op.edgeKey.toNode,
                           op.edgeKey.toPin};
    bool indexed = false;
    auto slot = OutgoingSlot(key, indexed);
    if (!slot) {
        return Result<void>::Failure(slot.Error());
    }

    if (op.hasValue && !found) {
        if (auto reserved = graph->edges.TryEnsureCapacity(graph->edges.Size() + 1); !reserved) {
            return reserved;
        }

        if (auto reserved = outgoing_.TryEnsureCapacity(outgoing_.Size() + 1); !reserved) {
            return reserved;
        }

        ROCKS_ASSERT(!indexed);
        ROCKS_ASSERT(graph->edges.TryInsert(op.edge, position.Value()));
        ROCKS_ASSERT(outgoing_.TryInsert(key, slot.Value()));
        op.hasValue = false;
        op.replaced = false;
        if (record) {
            if (auto recorded =
                    Record(GraphChangeKind::EdgeAdded, op.graph, op.edgeKey.toNode,
                           op.edgeKey.fromNode, op.edgeKey.toPin, op.edgeKey.fromPin, 0);
                !recorded) {
                ROCKS_ASSERT(ApplyEdgeOp(op, false));
                return recorded;
            }
        }

        return Result<void>::Success();
    }

    if (!op.hasValue && found) {
        ROCKS_ASSERT(indexed);
        op.edge = graph->edges[position.Value()];
        ROCKS_ASSERT(graph->edges.TryRemoveAt(position.Value()));
        ROCKS_ASSERT(outgoing_.TryRemoveAt(slot.Value()));
        op.hasValue = true;
        op.replaced = false;
        if (record) {
            if (auto recorded =
                    Record(GraphChangeKind::EdgeRemoved, op.graph, op.edgeKey.toNode,
                           op.edgeKey.fromNode, op.edgeKey.toPin, op.edgeKey.fromPin, 0);
                !recorded) {
                ROCKS_ASSERT(ApplyEdgeOp(op, false));
                return recorded;
            }
        }

        return Result<void>::Success();
    }

    if (op.hasValue && found) {
        CoreSwap(graph->edges[position.Value()], op.edge);
        op.replaced = true;
        if (record) {
            if (auto recorded = Record(GraphChangeKind::EdgeOrderChanged, op.graph,
                                       op.edgeKey.fromNode, Guid{}, op.edgeKey.fromPin, Name(), 0);
                !recorded) {
                CoreSwap(graph->edges[position.Value()], op.edge);
                return recorded;
            }
        }

        return Result<void>::Success();
    }

    return Result<void>::Failure(ErrorCode::StaleHandle);
}

Result<void> GraphMutatorCore::ApplyCommentOp(GraphPatchOp& op, bool record) noexcept {
    GraphLayout* layout = FindLayoutGraph(op.graph);
    if (layout == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    const usize index = CommentIndex(*layout, op.id);
    if (op.hasValue && index == IndexNone) {
        if (auto reserved = layout->comments.TryEnsureCapacity(layout->comments.Size() + 1);
            !reserved) {
            return reserved;
        }

        if (auto reserved = idIndex_.TryEnsureCapacity(idIndex_.Size() + 1); !reserved) {
            return reserved;
        }

        const usize position = CoreLowerBound(
            layout->comments, op.id, [](const CommentBox& comment) noexcept { return comment.id; });
        ROCKS_ASSERT(layout->comments.TryInsert(Move(op.comment), position));
        ROCKS_ASSERT(IndexId(op.id));
        op.hasValue = false;
        op.replaced = false;
    } else if (!op.hasValue && index != IndexNone) {
        op.comment = Move(layout->comments[index]);
        ROCKS_ASSERT(layout->comments.TryRemoveAt(index));
        UnindexId(op.id);
        op.hasValue = true;
        op.replaced = false;
    } else if (op.hasValue && index != IndexNone) {
        CoreSwap(layout->comments[index], op.comment);
        op.replaced = true;
    } else {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (record) {
        if (auto recorded =
                Record(GraphChangeKind::CommentChanged, op.graph, op.id, Guid{}, Name(), Name(), 0);
            !recorded) {
            ROCKS_ASSERT(ApplyCommentOp(op, false));
            return recorded;
        }
    }

    return Result<void>::Success();
}

Result<void> GraphMutatorCore::ApplyBookmarkOp(GraphPatchOp& op, bool record) noexcept {
    GraphLayout* layout = FindLayoutGraph(op.graph);
    if (layout == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    const usize index = BookmarkIndex(*layout, op.number);
    if (op.hasValue && index == IndexNone) {
        if (auto reserved = layout->bookmarks.TryEnsureCapacity(layout->bookmarks.Size() + 1);
            !reserved) {
            return reserved;
        }

        usize position = 0;
        while (position < layout->bookmarks.Size() &&
               layout->bookmarks[position].number < op.number) {
            ++position;
        }

        ROCKS_ASSERT(layout->bookmarks.TryInsert(op.bookmark, position));
        op.hasValue = false;
        op.replaced = false;
    } else if (!op.hasValue && index != IndexNone) {
        op.bookmark = layout->bookmarks[index];
        ROCKS_ASSERT(layout->bookmarks.TryRemoveAt(index));
        op.hasValue = true;
        op.replaced = false;
    } else if (op.hasValue && index != IndexNone) {
        CoreSwap(layout->bookmarks[index], op.bookmark);
        op.replaced = true;
    } else {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (record) {
        if (auto recorded = Record(GraphChangeKind::BookmarkChanged, op.graph, Guid{}, Guid{},
                                   Name(), Name(), op.number);
            !recorded) {
            ROCKS_ASSERT(ApplyBookmarkOp(op, false));
            return recorded;
        }
    }

    return Result<void>::Success();
}

Result<void> GraphMutatorCore::ApplyOp(GraphPatchOp& op, bool record) noexcept {
    Result<void> applied = Result<void>::Success();
    switch (op.kind) {
    case GraphSlotKind::Graph:
        applied = ApplyGraphOp(op, record);
        break;
    case GraphSlotKind::GraphTitle:
        applied = ApplyTitleOp(op, record);
        break;
    case GraphSlotKind::Node:
        applied = ApplyNodeOp(op, record);
        break;
    case GraphSlotKind::NodeLayout:
        applied = ApplyLayoutOp(op, record);
        break;
    case GraphSlotKind::Edge:
        applied = ApplyEdgeOp(op, record);
        break;
    case GraphSlotKind::Comment:
        applied = ApplyCommentOp(op, record);
        break;
    case GraphSlotKind::Bookmark:
        applied = ApplyBookmarkOp(op, record);
        break;
    }

    if (applied && record && CoreSemanticSlot(op.kind)) {
        pending_.semanticTouched = true;
    }

    return applied;
}

void GraphMutatorCore::Revert(Array<GraphPatchOp>& ops, usize count) noexcept {
    // 되돌리기는 LIFO이고 배열은 줄지 않으므로 할당하지 않는다(8.6절). 실패는 불변식 결함이다.
    for (usize index = count; index > 0; --index) {
        ROCKS_ASSERT(ApplyOp(ops[index - 1], false));
    }
}

Result<void> GraphMutatorCore::Emit(GraphPatchOp&& op) noexcept {
    if (auto reserved = work_.TryEnsureCapacity(work_.Size() + 1); !reserved) {
        return reserved;
    }

    ROCKS_ASSERT(work_.TryPushBack(Move(op)));
    if (auto applied = ApplyOp(work_[work_.Size() - 1], true); !applied) {
        ROCKS_ASSERT(work_.TryRemoveAt(work_.Size() - 1));
        return applied;
    }

    return Result<void>::Success();
}

Result<void> GraphMutatorCore::MergeInto(GraphPatch& target,
                                         HashMap<uint64, uint32>& keys) noexcept {
    if (auto reserved = target.ops_.TryEnsureCapacity(target.ops_.Size() + work_.Size());
        !reserved) {
        return reserved;
    }

    for (GraphPatchOp& op : work_) {
        const uint64 hash = CoreOpKeyHash(op);
        if (op.replaced) {
            if (const uint32* latest = keys.Find(hash); latest != nullptr) {
                const GraphPatchOp& previous = target.ops_[*latest];
                if (previous.replaced && CoreSameOpKey(previous, op)) {
                    // 앞 op가 원래 값을 담고 있으므로 이 op(중간 값)는 버린다.
                    continue;
                }
            }
        }

        ROCKS_ASSERT(target.ops_.TryPushBack(Move(op)));
        const uint32 position = static_cast<uint32>(target.ops_.Size() - 1);
        if (!keys.InsertOrAssign(hash, position)) {
            // 표를 갱신하지 못하면 낡은 항목이 틀린 합치기를 낳지 않게 지운다(합치기는 최적화다).
            static_cast<void>(keys.Erase(hash));
        }
    }

    work_.Clear();
    return Result<void>::Success();
}

void GraphMutatorCore::Notify(GraphChangeCause cause, bool interactive) noexcept {
    pending_.revision = revision_;
    pending_.cause = cause;
    pending_.interactive = interactive;
    // 콜백이 RemoveListener를 부르면 자리만 비운다(반복 중 배열이 줄지 않게). 끝에 빈 자리를
    // 지운다.
    notifying_ = true;
    for (usize index = 0; index < listeners_.Size(); ++index) {
        if (listeners_[index] != nullptr) {
            listeners_[index]->OnGraphChanged(pending_);
        }
    }

    notifying_ = false;
    for (usize index = listeners_.Size(); index > 0; --index) {
        if (listeners_[index - 1] == nullptr) {
            static_cast<void>(listeners_.TryRemoveAt(index - 1));
        }
    }

    pending_.changes.Clear();
    pending_.semanticTouched = false;
}

Result<void> GraphMutatorCore::CheckFullValidation() noexcept {
    if (!fullValidation_) {
        return Result<void>::Success();
    }

    GraphViolationList violations(allocator_);
    return ValidateGraphDocument(document_, &layout_, registry_, allocator_, violations);
}

// ---- 실행·undo·redo·bracket ----

Result<GraphPatch> GraphMutatorCore::Execute(uint64 source, uint64 expectedRevision,
                                             const GraphCommandList& commands,
                                             GraphCommandFailure* failure) noexcept {
    if (!initialized_) {
        return Result<GraphPatch>::Failure(ErrorCode::Unsupported);
    }

    if (interaction_ && source != interactionSource_) {
        return Result<GraphPatch>::Failure(ErrorCode::Busy);
    }

    if (expectedRevision != revision_) {
        return Result<GraphPatch>::Failure(ErrorCode::StaleHandle);
    }

    GraphCommandFailure ignored;
    failure_ = failure != nullptr ? failure : &ignored;
    *failure_ = GraphCommandFailure{};
    work_.Clear();
    pending_.changes.Clear();
    pending_.semanticTouched = false;
    const Span<const GraphCommand> list = commands.View();
    for (usize index = 0; index < list.Size(); ++index) {
        commandIndex_ = static_cast<uint32>(index);
        if (auto executed = ExecuteCommand(list[index]); !executed) {
            if (executed.Error() != ErrorCode::InvalidArgument) {
                *failure_ = GraphCommandFailure{};
                failure_->command = commandIndex_;
            }

            Revert(work_, work_.Size());
            work_.Clear();
            pending_.changes.Clear();
            return Result<GraphPatch>::Failure(executed.Error());
        }
    }

    if (auto retained = CheckInsertedRetainedPins(list); !retained) {
        Revert(work_, work_.Size());
        work_.Clear();
        pending_.changes.Clear();
        return Result<GraphPatch>::Failure(retained.Error());
    }

    if (auto checked = CheckFullValidation(); !checked) {
        Revert(work_, work_.Size());
        work_.Clear();
        pending_.changes.Clear();
        *failure_ = GraphCommandFailure{};
        failure_->reason = GraphCommandReason::InvariantBroken;
        return Result<GraphPatch>::Failure(checked.Error());
    }

    GraphPatch patch(allocator_);
    patch.owner_ = this;
    patch.stateBefore_ = state_;
    HashMap<uint64, uint32> keys(allocator_);
    GraphPatch& target = interaction_ ? interactionPatch_ : patch;
    HashMap<uint64, uint32>& targetKeys = interaction_ ? interactionKeys_ : keys;
    const bool semantic = pending_.semanticTouched;
    const bool changed = !work_.IsEmpty();
    if (auto merged = MergeInto(target, targetKeys); !merged) {
        Revert(work_, work_.Size());
        work_.Clear();
        pending_.changes.Clear();
        return Result<GraphPatch>::Failure(merged.Error());
    }

    // 문서를 바꾼 적용만 새 상태 토큰을 받는다(빈 patch는 전후가 같다).
    if (changed) {
        state_ = nextState_;
        ++nextState_;
    }

    patch.stateAfter_ = state_;
    target.semantic_ = target.semantic_ || semantic;
    if (interaction_) {
        interactionSemantic_ = interactionSemantic_ || semantic;
    }

    ++revision_;
    Notify(GraphChangeCause::Edit, interaction_);
    return Result<GraphPatch>::Success(Move(patch));
}

Result<void> GraphMutatorCore::ApplyPatch(GraphPatch& patch, bool reverse,
                                          GraphChangeCause cause) noexcept {
    pending_.changes.Clear();
    pending_.semanticTouched = false;
    const usize count = patch.ops_.Size();
    for (usize step = 0; step < count; ++step) {
        const usize index = reverse ? count - 1 - step : step;
        if (auto applied = ApplyOp(patch.ops_[index], true); !applied) {
            // 이미 적용한 op를 반대 순서로 되돌린다(무할당 — 8.6절).
            for (usize back = step; back > 0; --back) {
                const usize undoIndex = reverse ? count - back : back - 1;
                ROCKS_ASSERT(ApplyOp(patch.ops_[undoIndex], false));
            }

            pending_.changes.Clear();
            return applied;
        }
    }

    if (auto checked = CheckFullValidation(); !checked) {
        for (usize step = count; step > 0; --step) {
            const usize index = reverse ? count - step : step - 1;
            ROCKS_ASSERT(ApplyOp(patch.ops_[index], false));
        }

        pending_.changes.Clear();
        return checked;
    }

    ++revision_;
    Notify(cause, false);
    return Result<void>::Success();
}

Result<void> GraphMutatorCore::Undo(uint64 source, GraphPatch& patch) noexcept {
    static_cast<void>(source);
    if (!initialized_) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (interaction_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (patch.owner_ != this || !patch.applied_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // LIFO: 문서가 이 patch를 적용한 직후 상태여야 한다.
    if (state_ != patch.stateAfter_) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (auto applied = ApplyPatch(patch, true, GraphChangeCause::Undo); !applied) {
        return applied;
    }

    patch.applied_ = false;
    state_ = patch.stateBefore_;
    return Result<void>::Success();
}

Result<void> GraphMutatorCore::Redo(uint64 source, GraphPatch& patch) noexcept {
    static_cast<void>(source);
    if (!initialized_) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (interaction_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (patch.owner_ != this || patch.applied_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (state_ != patch.stateBefore_) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (auto applied = ApplyPatch(patch, false, GraphChangeCause::Redo); !applied) {
        return applied;
    }

    patch.applied_ = true;
    state_ = patch.stateAfter_;
    return Result<void>::Success();
}

Result<void> GraphMutatorCore::BeginInteraction(uint64 source) noexcept {
    if (!initialized_) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (interaction_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    interactionPatch_ = GraphPatch(allocator_);
    interactionPatch_.owner_ = this;
    interactionPatch_.stateBefore_ = state_;
    interactionKeys_.Clear();
    interactionSource_ = source;
    interactionSemantic_ = false;
    interaction_ = true;
    return Result<void>::Success();
}

Result<GraphPatch> GraphMutatorCore::EndInteraction(uint64 source) noexcept {
    if (!interaction_) {
        return Result<GraphPatch>::Failure(ErrorCode::NotReady);
    }

    if (source != interactionSource_) {
        return Result<GraphPatch>::Failure(ErrorCode::Busy);
    }

    GraphPatch patch = Move(interactionPatch_);
    patch.stateAfter_ = state_;
    interactionPatch_ = GraphPatch(allocator_);
    interactionPatch_.owner_ = this;
    interactionKeys_.Clear();
    interaction_ = false;
    pending_.changes.Clear();
    pending_.semanticTouched = interactionSemantic_;
    Notify(GraphChangeCause::Edit, false);
    return Result<GraphPatch>::Success(Move(patch));
}

Result<void> GraphMutatorCore::CancelInteraction(uint64 source) noexcept {
    if (!interaction_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    if (source != interactionSource_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (!interactionPatch_.ops_.IsEmpty()) {
        // bracket patch는 지금 적용된 상태다. 역순으로 되돌린다. 통지 기록이 할당에 실패하면
        // ApplyPatch가 문서를 bracket 상태로 되돌려 두므로 bracket을 열어 둔 채 오류를
        // 돌려준다(다시 취소하거나 확정할 수 있다).
        if (auto reverted = ApplyPatch(interactionPatch_, true, GraphChangeCause::Cancel);
            !reverted) {
            return reverted;
        }
    }

    state_ = interactionPatch_.stateBefore_;
    interaction_ = false;
    interactionPatch_ = GraphPatch(allocator_);
    interactionPatch_.owner_ = this;
    interactionKeys_.Clear();
    return Result<void>::Success();
}

Result<void> GraphMutatorCore::AddListener(GraphChangeListener& listener) noexcept {
    return listeners_.TryPushBack(&listener);
}

void GraphMutatorCore::RemoveListener(GraphChangeListener& listener) noexcept {
    for (usize index = 0; index < listeners_.Size(); ++index) {
        if (listeners_[index] == &listener) {
            if (notifying_) {
                listeners_[index] = nullptr;
            } else {
                static_cast<void>(listeners_.TryRemoveAt(index));
            }

            return;
        }
    }
}

} // namespace detail

// ---- GraphPatch·GraphMutator ----

usize GraphPatch::ApproximateBytes() const noexcept {
    usize bytes = sizeof(GraphPatch);
    for (const detail::GraphPatchOp& op : ops_) {
        bytes += sizeof(detail::GraphPatchOp) + op.node.properties.bytes.Size() +
                 op.title.View().ByteCount() + op.layout.bubble.View().ByteCount() +
                 op.comment.text.View().ByteCount();
        for (const PinValueEntry& entry : op.node.inputDefaults) {
            bytes += sizeof(PinValueEntry) + entry.value.bytes.Size();
        }

        for (const RetainedPin& pin : op.node.retainedPins) {
            bytes += sizeof(RetainedPin) + pin.value.bytes.Size();
        }
    }

    return bytes;
}

GraphMutator::GraphMutator(GraphDocument& document, GraphDocumentLayout& layout,
                           const NodeTypeRegistry& registry, GraphIdSource& ids) noexcept
    : document_(document), layout_(layout), registry_(registry), ids_(ids) {
}

GraphMutator::~GraphMutator() noexcept = default;

Result<void> GraphMutator::Initialize() noexcept {
    if (core_.Get() != nullptr) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    Allocator& allocator = document_.graphs.GetAllocator();
    auto made = UniquePtr<detail::GraphMutatorCore>::TryMake(allocator, document_, layout_,
                                                             registry_, ids_, allocator);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    if (auto initialized = made.Value()->Initialize(); !initialized) {
        return initialized;
    }

    core_ = Move(made.Value());
    return Result<void>::Success();
}

uint64 GraphMutator::Revision() const noexcept {
    return core_.Get() != nullptr ? core_->Revision() : 0;
}

Result<GraphPatch> GraphMutator::Execute(uint64 source, uint64 expectedRevision,
                                         const GraphCommandList& commands,
                                         GraphCommandFailure* failure) noexcept {
    if (core_.Get() == nullptr) {
        return Result<GraphPatch>::Failure(ErrorCode::Unsupported);
    }

    return core_->Execute(source, expectedRevision, commands, failure);
}

Result<void> GraphMutator::Undo(uint64 source, GraphPatch& patch) noexcept {
    return core_.Get() != nullptr ? core_->Undo(source, patch)
                                  : Result<void>::Failure(ErrorCode::Unsupported);
}

Result<void> GraphMutator::Redo(uint64 source, GraphPatch& patch) noexcept {
    return core_.Get() != nullptr ? core_->Redo(source, patch)
                                  : Result<void>::Failure(ErrorCode::Unsupported);
}

Result<void> GraphMutator::BeginInteraction(uint64 source) noexcept {
    return core_.Get() != nullptr ? core_->BeginInteraction(source)
                                  : Result<void>::Failure(ErrorCode::Unsupported);
}

Result<GraphPatch> GraphMutator::EndInteraction(uint64 source) noexcept {
    if (core_.Get() == nullptr) {
        return Result<GraphPatch>::Failure(ErrorCode::Unsupported);
    }

    return core_->EndInteraction(source);
}

Result<void> GraphMutator::CancelInteraction(uint64 source) noexcept {
    return core_.Get() != nullptr ? core_->CancelInteraction(source)
                                  : Result<void>::Failure(ErrorCode::Unsupported);
}

bool GraphMutator::InteractionOpen() const noexcept {
    return core_.Get() != nullptr && core_->InteractionOpen();
}

Result<void> GraphMutator::AddListener(GraphChangeListener& listener) noexcept {
    return core_.Get() != nullptr ? core_->AddListener(listener)
                                  : Result<void>::Failure(ErrorCode::Unsupported);
}

void GraphMutator::RemoveListener(GraphChangeListener& listener) noexcept {
    if (core_.Get() != nullptr) {
        core_->RemoveListener(listener);
    }
}

void GraphMutator::SetFullValidation(bool enabled) noexcept {
    if (core_.Get() != nullptr) {
        core_->SetFullValidation(enabled);
    }
}

GraphReadView GraphMutator::View() const noexcept {
    return GraphReadView(document_, registry_);
}

} // namespace rocks::nodegraph
