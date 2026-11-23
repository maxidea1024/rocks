#include <Core/Algorithms.h>
#include <NodeGraph/GraphClone.h>
#include <NodeGraph/GraphNameOrder.h>
#include <NodeGraph/GraphReadView.h>
#include <NodeGraph/GraphSchema.h>
#include <NodeGraphTools/GraphPaste.h>
#include <Serialization/Record.h>

// 복사·붙여넣기다(Docs/Design/NodeGraph.ko.md 9.4절 "복사"·"붙여넣기"). 붙여넣기는 노드 판정 →
// InsertNode(새 id·위치 이동) → 조각 간선 InsertEdge(원래 순번 순) → 바깥 간선 Connect(같은
// 문서이고 schema가 허락하며 판정이 Make일 때만 — 실행 때도 Make만 받는다) → AddComment 순서로 명령
// 목록 하나를 만든다. 노드·간선 판정은 InsertNode·InsertEdge 규칙을 미리 본다 — 걸리는 것은 진단과
// 함께 빼서 목록 전체가 거부되지 않게 한다.
namespace rocks::nodegraph {
namespace {

bool PasteIdZero(Guid id) noexcept {
    return id.high == 0 && id.low == 0;
}

bool PasteNameNone(Name name) noexcept {
    return name == Name();
}

const Graph* PasteFindGraph(const GraphDocument& document, Guid id) noexcept {
    for (const Graph& graph : document.graphs) {
        if (graph.id == id) {
            return &graph;
        }
    }

    return nullptr;
}

const GraphLayout* PasteFindGraphLayout(const GraphDocumentLayout* layout, Guid id) noexcept {
    if (layout == nullptr) {
        return nullptr;
    }

    for (const GraphLayout& graph : layout->graphs) {
        if (graph.graph == id) {
            return &graph;
        }
    }

    return nullptr;
}

// 정렬된 id 배열에서 찾는다.
bool PasteContains(const Array<Guid>& sorted, Guid id) noexcept {
    usize low = 0;
    usize high = sorted.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const int32 compared = CompareGuid(sorted[middle], id);
        if (compared == 0) {
            return true;
        }

        if (compared < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return false;
}

// 정렬된 배열에 id를 넣는다. 이미 있으면 거짓이다.
Result<bool> PasteInsertSorted(Array<Guid>& sorted, Guid id) noexcept {
    usize low = 0;
    usize high = sorted.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const int32 compared = CompareGuid(sorted[middle], id);
        if (compared == 0) {
            return Result<bool>::Success(false);
        }

        if (compared < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    if (auto inserted = sorted.TryInsert(id, low); !inserted) {
        return Result<bool>::Failure(inserted.Error());
    }

    return Result<bool>::Success(true);
}

// 속성 Record schema가 서술의 속성 struct와 맞는가(둘 다 없음 포함 — mutator 규칙과 같다).
bool PropertiesMatch(const NodeTypeDescriptor& descriptor,
                     const PropertyRecord& properties) noexcept {
    if (PasteIdZero(descriptor.propertiesStruct)) {
        return properties.bytes.IsEmpty();
    }

    auto header = ReadRecordHeader(properties.bytes.View());
    return header && header.Value().schema == descriptor.propertiesStruct;
}

const GraphIdMapping* FindMapping(const Array<GraphIdMapping>& mappings, Guid from) noexcept {
    for (const GraphIdMapping& mapping : mappings) {
        if (mapping.from == from) {
            return &mapping;
        }
    }

    return nullptr;
}

// 조각 간선 하나와 그 조각 안 위치다(같은 순번의 안정 정렬용).
struct OrderedEdge {
    GraphEdge edge;
    usize index = 0;
};

// 붙여넣기 문맥이다(대상 문서는 읽기만 한다).
class PasteBuilder {
public:
    PasteBuilder(const GraphFragment& fragment, const GraphDocument& document,
                 const NodeTypeRegistry& registry, const GraphPasteOptions& options,
                 GraphIdSource& ids, GraphPasteResult& out) noexcept
        : fragment_(fragment), document_(document), registry_(registry), options_(options),
          ids_(ids), out_(out), view_(document, registry),
          schema_(registry.FindSchema(document.domain)) {
    }

    Result<void> Run() noexcept {
        if (!(fragment_.domain == document_.domain)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        graph_ = PasteFindGraph(document_, options_.graph);
        if (graph_ == nullptr) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        if (auto chosen = ChooseNodes(); !chosen) {
            return chosen;
        }

        if (auto edges = ChooseEdges(); !edges) {
            return edges;
        }

        if (auto nodes = EmitNodes(); !nodes) {
            return nodes;
        }

        if (auto edges = EmitEdges(); !edges) {
            return edges;
        }

        return EmitComments();
    }

private:
    Result<void> Warn(GraphPasteCode code, Guid node, StringView message) noexcept {
        return out_.diagnostics.Add(GraphDiagnosticSeverity::Warning, static_cast<uint32>(code),
                                    options_.targetAsset, options_.graph, node, Name(), message);
    }

    // 노드마다 붙일지 판정하고 새 id를 받는다(조각 순서).
    Result<void> ChooseNodes() noexcept {
        for (const GraphFragmentNode& entry : fragment_.nodes) {
            const GraphNode& node = entry.node;
            if (!entry.unresolvedType.IsEmpty()) {
                if (auto warned = Warn(GraphPasteCode::UnresolvedProperties, node.id,
                                       "node properties use an unregistered type");
                    !warned) {
                    return warned;
                }

                continue;
            }

            auto accepted = Accept(node);
            if (!accepted) {
                return Result<void>::Failure(accepted.Error());
            }

            if (!accepted.Value()) {
                continue;
            }

            auto id = ids_.Next();
            if (!id) {
                return Result<void>::Failure(id.Error());
            }

            if (auto added = out_.nodes.TryPushBack(GraphIdMapping{node.id, id.Value()}); !added) {
                return added;
            }

            if (auto added = kept_.TryPushBack(&entry); !added) {
                return added;
            }
        }

        return Result<void>::Success();
    }

    // 노드 데이터가 InsertNode 규칙(9.4절)을 지키는지 미리 본다. 거부하면 진단 하나와 거짓이다.
    Result<bool> Accept(const GraphNode& node) noexcept {
        GraphPasteCode code = GraphPasteCode::NodeData;
        StringView message;
        const RegisteredNodeType* type = registry_.FindNodeType(document_.domain, node.type);
        auto data = CheckNodeData(node, type, message);
        if (!data) {
            return Result<bool>::Failure(data.Error());
        }

        if (data.Value()) {
            if (type == nullptr) {
                return Result<bool>::Success(true);
            }

            const NodeTypeDescriptor& descriptor = type->descriptor;
            if (node.typeSchemaVersion != descriptor.schemaVersion) {
                code = GraphPasteCode::TypeVersion;
                message = "node type version differs from the registered type";
            } else if (!PropertiesMatch(descriptor, node.properties)) {
                code = GraphPasteCode::PropertiesSchema;
                message = "node properties do not match the registered type";
            } else if (!HasCapability(descriptor.capabilities, NodeCapabilities::CanDuplicate)) {
                code = GraphPasteCode::CannotDuplicate;
                message = "node type cannot be duplicated";
            } else if (HasCapability(descriptor.capabilities,
                                     NodeCapabilities::SinglePerDocument) &&
                       HasSingle(node.type)) {
                code = GraphPasteCode::SinglePerDocument;
                message = "node type allows one node per document";
            } else if (node.enableState == NodeEnableState::Disabled &&
                       !HasCapability(descriptor.capabilities, NodeCapabilities::AllowDisable)) {
                message = "node type cannot be disabled";
            } else if (schema_ != nullptr &&
                       schema_->CanPasteNode(view_, node) == PasteDecision::Deny) {
                code = GraphPasteCode::PasteDenied;
                message = "domain schema denied pasting the node";
            } else {
                auto pins = CheckPins(node, message);
                if (!pins) {
                    return Result<bool>::Failure(pins.Error());
                }

                if (pins.Value()) {
                    return Result<bool>::Success(true);
                }
            }
        }

        if (auto warned = Warn(code, node.id, message); !warned) {
            return Result<bool>::Failure(warned.Error());
        }

        return Result<bool>::Success(false);
    }

    // 종류와 무관한 노드 데이터 규칙이다: 키가 None이 아님·겹치지 않음, 고아 핀 방향, 재경유의 고아
    // 핀, 활성 상태 값.
    Result<bool> CheckNodeData(const GraphNode& node, const RegisteredNodeType* type,
                               StringView& message) noexcept {
        if (node.enableState != NodeEnableState::Enabled &&
            node.enableState != NodeEnableState::Disabled) {
            message = "node enable state is not defined";
            return Result<bool>::Success(false);
        }

        for (usize index = 0; index < node.inputDefaults.Size(); ++index) {
            const Name pin = node.inputDefaults[index].pin;
            for (usize other = 0; other < index; ++other) {
                if (node.inputDefaults[other].pin == pin) {
                    message = "node input default pins repeat";
                    return Result<bool>::Success(false);
                }
            }

            if (PasteNameNone(pin)) {
                message = "node input default pin is None";
                return Result<bool>::Success(false);
            }
        }

        for (usize index = 0; index < node.retainedPins.Size(); ++index) {
            const RetainedPin& pin = node.retainedPins[index];
            for (usize other = 0; other < index; ++other) {
                if (node.retainedPins[other].key == pin.key) {
                    message = "node retained pin keys repeat";
                    return Result<bool>::Success(false);
                }
            }

            if (PasteNameNone(pin.key) ||
                (pin.direction != PinDirection::Input && pin.direction != PinDirection::Output)) {
                message = "node retained pin is not valid";
                return Result<bool>::Success(false);
            }
        }

        if (type != nullptr && node.type == registry_.RerouteType() &&
            !node.retainedPins.IsEmpty()) {
            message = "reroute nodes cannot have retained pins";
            return Result<bool>::Success(false);
        }

        return Result<bool>::Success(true);
    }

    // 알려진 종류의 핀 규칙이다: 대상 그래프에서 핀을 계산할 수 있음, 입력 기본값은 Hidden이 아닌
    // 현재 입력 핀이고 값이 있음(GI14), 고아 핀이 현재 핀을 가리지 않음(GI8).
    Result<bool> CheckPins(const GraphNode& node, StringView& message) noexcept {
        PinList pins(out_.nodes.GetAllocator());
        if (auto collected = registry_.CollectPins(document_, *graph_, node, pins); !collected) {
            if (collected.Error() == ErrorCode::OutOfMemory) {
                return Result<bool>::Failure(collected.Error());
            }

            message = "node pins cannot be computed in the target graph";
            return Result<bool>::Success(false);
        }

        for (const PinValueEntry& entry : node.inputDefaults) {
            const PinDescriptor* pin = pins.Find(entry.pin);
            if (pin == nullptr || pin->direction != PinDirection::Input ||
                pin->valuePolicy == PinValuePolicy::Hidden || entry.value.bytes.IsEmpty()) {
                message = "node input default does not fit a current input pin";
                return Result<bool>::Success(false);
            }
        }

        for (const RetainedPin& retained : node.retainedPins) {
            if (pins.Find(retained.key) != nullptr) {
                message = "node retained pin shadows a current pin";
                return Result<bool>::Success(false);
            }
        }

        return Result<bool>::Success(true);
    }

    // 문서나 앞서 붙이기로 한 조각 노드에 같은 종류가 있는가.
    bool HasSingle(Name type) const noexcept {
        for (const Graph& graph : document_.graphs) {
            for (const GraphNode& existing : graph.nodes) {
                if (existing.type == type) {
                    return true;
                }
            }
        }

        for (const GraphFragmentNode* entry : kept_) {
            if (entry->node.type == type) {
                return true;
            }
        }

        return false;
    }

    Guid NewId(Guid old) const noexcept {
        const GraphIdMapping* mapping = FindMapping(out_.nodes, old);
        return mapping != nullptr ? mapping->to : Guid{};
    }

    const GraphNode* KeptNode(Guid old) const noexcept {
        for (const GraphFragmentNode* entry : kept_) {
            if (entry->node.id == old) {
                return &entry->node;
            }
        }

        return nullptr;
    }

    // 조각 노드 끝 하나가 InsertEdge 규칙(현재 핀이나 그 노드의 고아 핀, 방향 일치 — 알 수 없는
    // 종류는 None이 아닌 핀)을 지키는가.
    Result<bool> EdgeEndFits(const GraphNode& node, Name key, PinDirection direction) noexcept {
        if (PasteNameNone(key)) {
            return Result<bool>::Success(false);
        }

        if (registry_.FindNodeType(document_.domain, node.type) == nullptr) {
            return Result<bool>::Success(true);
        }

        PinList pins(out_.nodes.GetAllocator());
        if (auto collected = registry_.CollectPins(document_, *graph_, node, pins); !collected) {
            return collected.Error() == ErrorCode::OutOfMemory
                       ? Result<bool>::Failure(collected.Error())
                       : Result<bool>::Success(false);
        }

        if (const PinDescriptor* pin = pins.Find(key); pin != nullptr) {
            return Result<bool>::Success(pin->direction == direction);
        }

        for (const RetainedPin& retained : node.retainedPins) {
            if (retained.key == key) {
                return Result<bool>::Success(retained.direction == direction);
            }
        }

        return Result<bool>::Success(false);
    }

    // 조각 간선은 두 끝이 남은 노드이고 끝 핀이 맞을 때 새 id로 옮기고, 바깥 간선은 다시 이을 수
    // 있을 때만 남긴다. 진단의 노드 id는 조각 쪽 옛 id다(조각 간선은 빠진 끝).
    Result<void> ChooseEdges() noexcept {
        Array<OrderedEdge> ordered(out_.nodes.GetAllocator());
        for (usize index = 0; index < fragment_.edges.Size(); ++index) {
            const GraphEdge& edge = fragment_.edges[index];
            const GraphNode* from = KeptNode(edge.fromNode);
            const GraphNode* to = KeptNode(edge.toNode);
            bool fits = from != nullptr && to != nullptr;
            if (fits) {
                auto fromFits = EdgeEndFits(*from, edge.fromPin, PinDirection::Output);
                auto toFits = EdgeEndFits(*to, edge.toPin, PinDirection::Input);
                if (!fromFits || !toFits) {
                    return Result<void>::Failure(!fromFits ? fromFits.Error() : toFits.Error());
                }

                fits = fromFits.Value() && toFits.Value();
            }

            if (!fits) {
                const Guid missing = from == nullptr ? edge.fromNode : edge.toNode;
                if (auto warned = Warn(GraphPasteCode::EdgeDropped, missing,
                                       "edge end node or pin was not pasted");
                    !warned) {
                    return warned;
                }

                continue;
            }

            OrderedEdge moved;
            moved.edge = edge;
            moved.edge.fromNode = NewId(edge.fromNode);
            moved.edge.toNode = NewId(edge.toNode);
            moved.index = index;
            if (auto added = ordered.TryPushBack(moved); !added) {
                return added;
            }
        }

        // 원래 순번 순으로 넣어야 ordered 출력 핀의 끝에 붙이기가 같은 순서를 만든다(같은 순번은
        // 조각 순서 — 안정).
        algo::Sort(ordered.View(), [](const OrderedEdge& left, const OrderedEdge& right) noexcept {
            return left.edge.order != right.edge.order ? left.edge.order < right.edge.order
                                                       : left.index < right.index;
        });
        if (auto reserved = internal_.TryReserve(ordered.Size()); !reserved) {
            return reserved;
        }

        for (const OrderedEdge& edge : ordered) {
            if (auto added = internal_.TryPushBack(edge.edge); !added) {
                return added;
            }
        }

        const bool sameDocument =
            !PasteIdZero(options_.targetAsset) && options_.targetAsset == fragment_.sourceAsset;
        const bool reconnect =
            sameDocument && schema_ != nullptr && schema_->AllowExternalEdgeReconnect();
        for (const GraphEdge& edge : fragment_.externalEdges) {
            auto kept = reconnect ? Reconnect(edge) : Result<bool>::Success(false);
            if (!kept) {
                return Result<void>::Failure(kept.Error());
            }

            if (!kept.Value()) {
                const Guid inside =
                    KeptNode(edge.fromNode) != nullptr ? edge.fromNode : edge.toNode;
                if (auto warned = Warn(GraphPasteCode::ExternalEdgeDropped, inside,
                                       "external edge was not reconnected");
                    !warned) {
                    return warned;
                }
            }
        }

        return Result<void>::Success();
    }

    // 바깥 간선 하나를 다시 이을지 본다: 밖 끝 노드가 대상 그래프에 있고, 두 끝 핀이 지금 핀 목록에
    // 있으며, schema 판정이 Make여야 한다(바꾸기·변환은 기존 간선을 바꾸므로 하지 않는다 — 실행
    // 때도 Connect가 Make만 받는다).
    Result<bool> Reconnect(const GraphEdge& edge) noexcept {
        const Guid newFrom = NewId(edge.fromNode);
        const Guid newTo = NewId(edge.toNode);
        if (PasteIdZero(newFrom) == PasteIdZero(newTo)) {
            return Result<bool>::Success(false);
        }

        GraphEdge moved = edge;
        moved.order = 0;
        if (!PasteIdZero(newFrom)) {
            moved.fromNode = newFrom;
        } else {
            moved.toNode = newTo;
        }

        const GraphNode* fromNode = PastedOrExisting(edge.fromNode, newFrom);
        const GraphNode* toNode = PastedOrExisting(edge.toNode, newTo);
        if (fromNode == nullptr || toNode == nullptr) {
            return Result<bool>::Success(false);
        }

        PinList fromPins(out_.nodes.GetAllocator());
        PinList toPins(out_.nodes.GetAllocator());
        if (auto collected = registry_.CollectPins(document_, *graph_, *fromNode, fromPins);
            !collected) {
            return collected.Error() == ErrorCode::OutOfMemory
                       ? Result<bool>::Failure(collected.Error())
                       : Result<bool>::Success(false);
        }

        if (auto collected = registry_.CollectPins(document_, *graph_, *toNode, toPins);
            !collected) {
            return collected.Error() == ErrorCode::OutOfMemory
                       ? Result<bool>::Failure(collected.Error())
                       : Result<bool>::Success(false);
        }

        const PinDescriptor* fromPin = fromPins.Find(edge.fromPin);
        const PinDescriptor* toPin = toPins.Find(edge.toPin);
        if (fromPin == nullptr || toPin == nullptr || fromPin->direction != PinDirection::Output ||
            toPin->direction != PinDirection::Input) {
            return Result<bool>::Success(false);
        }

        ConnectQuery query;
        query.graph = graph_;
        query.from = PinRef{moved.fromNode, moved.fromPin};
        query.to = PinRef{moved.toNode, moved.toPin};
        query.fromPin = fromPin;
        query.toPin = toPin;
        query.toEdgeCount = CountEdges(moved.toNode, moved.toPin, false);
        query.fromEdgeCount = CountEdges(moved.fromNode, moved.fromPin, true);
        if (schema_->CanConnect(view_, query).action != ConnectAction::Make) {
            return Result<bool>::Success(false);
        }

        if (auto added = external_.TryPushBack(moved); !added) {
            return Result<bool>::Failure(added.Error());
        }

        return Result<bool>::Success(true);
    }

    // 붙일 조각 노드(새 id가 있으면)나 대상 그래프의 기존 노드다.
    const GraphNode* PastedOrExisting(Guid old, Guid pasted) const noexcept {
        if (!PasteIdZero(pasted)) {
            return KeptNode(old);
        }

        for (const GraphNode& node : graph_->nodes) {
            if (node.id == old) {
                return &node;
            }
        }

        return nullptr;
    }

    // 붙여넣기 뒤 그 핀의 간선 수다(대상 그래프의 기존 간선 + 조각 간선 + 앞서 고른 바깥 간선).
    uint32 CountEdges(Guid node, Name pin, bool output) const noexcept {
        uint32 count = 0;
        const auto matches = [&](const GraphEdge& edge) noexcept {
            return output ? edge.fromNode == node && edge.fromPin == pin
                          : edge.toNode == node && edge.toPin == pin;
        };

        for (const GraphEdge& edge : graph_->edges) {
            count += matches(edge) ? 1u : 0u;
        }

        for (const GraphEdge& edge : internal_) {
            count += matches(edge) ? 1u : 0u;
        }

        for (const GraphEdge& edge : external_) {
            count += matches(edge) ? 1u : 0u;
        }

        return count;
    }

    // 붙일 간선에 닿는가(값 없는 고아 핀을 남길지 — GI7).
    bool Touched(Guid node, Name key) const noexcept {
        const auto touches = [&](const GraphEdge& edge) noexcept {
            return (edge.fromNode == node && edge.fromPin == key) ||
                   (edge.toNode == node && edge.toPin == key);
        };

        for (const GraphEdge& edge : internal_) {
            if (touches(edge)) {
                return true;
            }
        }

        for (const GraphEdge& edge : external_) {
            if (touches(edge)) {
                return true;
            }
        }

        return false;
    }

    // 조각 노드·주석의 왼쪽 위다(없으면 붙이는 위치 그대로).
    Vec2 Origin() const noexcept {
        bool any = false;
        Vec2 origin = options_.position;
        const auto include = [&](Vec2 position) noexcept {
            if (!any) {
                origin = position;
                any = true;
                return;
            }

            origin.x = position.x < origin.x ? position.x : origin.x;
            origin.y = position.y < origin.y ? position.y : origin.y;
        };

        for (const GraphFragmentNode* entry : kept_) {
            include(entry->layout.position);
        }

        for (const CommentBox& comment : fragment_.comments) {
            include(comment.position);
        }

        return origin;
    }

    Result<void> EmitNodes() noexcept {
        const Vec2 origin = Origin();
        offset_ = Vec2(options_.position.x - origin.x, options_.position.y - origin.y);
        for (const GraphFragmentNode* entry : kept_) {
            const Guid id = NewId(entry->node.id);
            GraphNode node(out_.nodes.GetAllocator());
            if (auto cloned = CloneGraphNode(entry->node, node); !cloned) {
                return cloned;
            }

            node.id = id;
            // 값 없는 고아 핀은 붙는 간선이 남지 않으면 뺀다(GI7).
            for (usize pin = node.retainedPins.Size(); pin > 0; --pin) {
                const RetainedPin& retained = node.retainedPins[pin - 1];
                if (retained.value.bytes.IsEmpty() && !Touched(id, retained.key)) {
                    if (auto removed = node.retainedPins.TryRemoveAt(pin - 1); !removed) {
                        return removed;
                    }
                }
            }

            NodeLayout layout(out_.nodes.GetAllocator());
            if (auto cloned = CloneNodeLayout(entry->layout, layout); !cloned) {
                return cloned;
            }

            layout.node = id;
            layout.position = Vec2(layout.position.x + offset_.x, layout.position.y + offset_.y);
            if (auto inserted = out_.commands.InsertNode(options_.graph, node, layout); !inserted) {
                return inserted;
            }
        }

        return Result<void>::Success();
    }

    Result<void> EmitEdges() noexcept {
        for (const GraphEdge& edge : internal_) {
            if (auto inserted = out_.commands.InsertEdge(options_.graph, edge); !inserted) {
                return inserted;
            }
        }

        for (const GraphEdge& edge : external_) {
            if (auto connected = out_.commands.Connect(options_.graph, edge, true); !connected) {
                return connected;
            }
        }

        return Result<void>::Success();
    }

    Result<void> EmitComments() noexcept {
        for (const CommentBox& source : fragment_.comments) {
            auto id = ids_.Next();
            if (!id) {
                return Result<void>::Failure(id.Error());
            }

            CommentBox comment(out_.nodes.GetAllocator());
            if (auto cloned = CloneCommentBox(source, comment); !cloned) {
                return cloned;
            }

            comment.id = id.Value();
            comment.position = Vec2(source.position.x + offset_.x, source.position.y + offset_.y);
            if (auto added = out_.commands.AddComment(options_.graph, comment); !added) {
                return added;
            }

            if (auto mapped = out_.comments.TryPushBack(GraphIdMapping{source.id, id.Value()});
                !mapped) {
                return mapped;
            }
        }

        return Result<void>::Success();
    }

    const GraphFragment& fragment_;
    const GraphDocument& document_;
    const NodeTypeRegistry& registry_;
    const GraphPasteOptions& options_;
    GraphIdSource& ids_;
    GraphPasteResult& out_;
    GraphReadView view_;
    const GraphSchema* schema_;
    const Graph* graph_ = nullptr;
    // 붙일 조각 노드다(조각을 가리킨다).
    Array<const GraphFragmentNode*> kept_{out_.nodes.GetAllocator()};
    Array<GraphEdge> internal_{out_.nodes.GetAllocator()};
    Array<GraphEdge> external_{out_.nodes.GetAllocator()};
    Vec2 offset_;
};

} // namespace

Result<void> CopyGraphFragment(const GraphDocument& document, const GraphDocumentLayout* layout,
                               const NodeTypeRegistry& registry, Guid graph, Span<const Guid> nodes,
                               Span<const Guid> comments, Guid sourceAsset,
                               GraphFragment& out) noexcept {
    out.nodes.Clear();
    out.edges.Clear();
    out.externalEdges.Clear();
    out.comments.Clear();
    const Graph* source = PasteFindGraph(document, graph);
    if (source == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    out.domain = document.domain;
    out.sourceAsset = sourceAsset;
    out.sourceGraph = graph;
    const GraphLayout* graphLayout = PasteFindGraphLayout(layout, graph);
    Allocator& allocator = out.nodes.GetAllocator();

    // 고른 id를 NodeId 순으로 모은다(고른 순서와 무관하게 같은 조각 — 새 id 배정이 재현된다).
    Array<Guid> requested(allocator);
    for (const Guid id : nodes) {
        auto inserted = PasteInsertSorted(requested, id);
        if (!inserted) {
            return Result<void>::Failure(inserted.Error());
        }

        if (!inserted.Value()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    Array<Guid> selected(allocator);
    for (const Guid id : requested) {
        const GraphNode* found = nullptr;
        for (const GraphNode& node : source->nodes) {
            if (node.id == id) {
                found = &node;
                break;
            }
        }

        if (found == nullptr) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        // 복제할 수 없는 종류는 빼고, 그 노드와의 간선은 바깥 간선이 된다.
        if (const RegisteredNodeType* type = registry.FindNodeType(document.domain, found->type);
            type != nullptr &&
            !HasCapability(type->descriptor.capabilities, NodeCapabilities::CanDuplicate)) {
            continue;
        }

        auto entry = out.nodes.TryEmplaceBack(allocator);
        if (!entry) {
            return Result<void>::Failure(entry.Error());
        }

        if (auto cloned = CloneGraphNode(*found, entry.Value()->node); !cloned) {
            return cloned;
        }

        entry.Value()->layout.node = id;
        if (graphLayout != nullptr) {
            for (const NodeLayout& candidate : graphLayout->nodes) {
                if (candidate.node == id) {
                    if (auto cloned = CloneNodeLayout(candidate, entry.Value()->layout); !cloned) {
                        return cloned;
                    }

                    break;
                }
            }
        }

        if (auto added = selected.TryPushBack(id); !added) {
            return added;
        }
    }

    for (const GraphEdge& edge : source->edges) {
        const bool from = PasteContains(selected, edge.fromNode);
        const bool to = PasteContains(selected, edge.toNode);
        if (from && to) {
            if (auto added = out.edges.TryPushBack(edge); !added) {
                return added;
            }
        } else if (from || to) {
            if (auto added = out.externalEdges.TryPushBack(edge); !added) {
                return added;
            }
        }
    }

    Array<Guid> commentIds(allocator);
    for (const Guid id : comments) {
        auto inserted = PasteInsertSorted(commentIds, id);
        if (!inserted) {
            return Result<void>::Failure(inserted.Error());
        }

        if (!inserted.Value()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    for (const Guid id : commentIds) {
        const CommentBox* found = nullptr;
        if (graphLayout != nullptr) {
            for (const CommentBox& comment : graphLayout->comments) {
                if (comment.id == id) {
                    found = &comment;
                    break;
                }
            }
        }

        if (found == nullptr) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        CommentBox comment(allocator);
        if (auto cloned = CloneCommentBox(*found, comment); !cloned) {
            return cloned;
        }

        if (auto added = out.comments.TryPushBack(Move(comment)); !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

Result<void> BuildPasteCommands(const GraphFragment& fragment, const GraphDocument& document,
                                const NodeTypeRegistry& registry, const GraphPasteOptions& options,
                                GraphIdSource& ids, GraphPasteResult& out) noexcept {
    out.commands.Clear();
    out.diagnostics.items.Clear();
    out.nodes.Clear();
    out.comments.Clear();
    PasteBuilder builder(fragment, document, registry, options, ids, out);
    return builder.Run();
}

} // namespace rocks::nodegraph
