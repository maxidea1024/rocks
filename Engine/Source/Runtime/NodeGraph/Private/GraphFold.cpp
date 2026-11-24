#include <NodeGraph/GraphFold.h>

#include <Core/Text/NamePool.h>
#include <NodeGraph/NodeTypeRegistry.h>

// 접기·펼치기 명령 목록 도우미다(Docs/Design/NodeGraph 10.1절 NG3c).

namespace rocks::nodegraph {

namespace {

// 경계 핀 키 "In<n>"·"Out<n>"이다.
constexpr usize FoldKeyPrefixMaxBytes = 8;

Result<Name> FoldKey(NamePool& names, StringView prefix, usize number) noexcept {
    constexpr usize MaxDigits = 20;
    char digits[MaxDigits];
    usize count = 0;
    do {
        digits[count++] = static_cast<char>('0' + number % 10);
        number /= 10;
    } while (number != 0 && count < MaxDigits);

    char text[FoldKeyPrefixMaxBytes + MaxDigits];
    usize length = 0;
    for (usize index = 0; index < prefix.ByteCount() && length < FoldKeyPrefixMaxBytes; ++index) {
        text[length++] = prefix.Data()[index];
    }

    while (count > 0) {
        text[length++] = digits[--count];
    }

    return names.TryIntern(StringView(text, length));
}

GraphEdge FoldEdge(Guid from, Name fromPin, Guid to, Name toPin) noexcept {
    GraphEdge edge;
    edge.fromNode = from;
    edge.fromPin = fromPin;
    edge.toNode = to;
    edge.toPin = toPin;
    return edge;
}

bool FoldContains(Span<const Guid> nodes, Guid id) noexcept {
    for (const Guid& candidate : nodes) {
        if (candidate == id) {
            return true;
        }
    }

    return false;
}

// 안쪽 핀 하나(경계 핀 하나에 대응)와 그 핀을 건너는 간선들이다.
struct FoldGroup {
    explicit FoldGroup(Allocator& allocator) noexcept : edges(allocator) {
    }

    Guid node;
    Name pin;
    GraphBoundaryPin boundary;
    Array<GraphEdge> edges;
};

const NodeLayout* FoldFindLayout(const GraphDocumentLayout* layout, Guid graph,
                                 Guid node) noexcept {
    if (layout == nullptr) {
        return nullptr;
    }

    for (const GraphLayout& entry : layout->graphs) {
        if (entry.graph != graph) {
            continue;
        }

        for (const NodeLayout& position : entry.nodes) {
            if (position.node == node) {
                return &position;
            }
        }
    }

    return nullptr;
}

// 안쪽 핀의 서술로 경계 핀을 채운다(종류·타입 이름·상한).
Result<void> FoldDescribe(const GraphReadView& view, const Graph& graph, Guid node, Name pin,
                          Name key, bool input, Allocator& allocator,
                          GraphBoundaryPin& out) noexcept {
    const GraphNode* found = view.FindNode(graph, node);
    if (found == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    PinList pins(allocator);
    if (auto collected = view.CollectPins(graph, *found, pins); !collected) {
        return Result<void>::Failure(
            collected.Error() == ErrorCode::NotFound ? ErrorCode::Unsupported : collected.Error());
    }

    const PinDescriptor* descriptor = pins.Find(pin);
    if (descriptor == nullptr) {
        // 고아 핀에 닿은 간선은 접을 수 없다(경계 핀 서술이 없다).
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    out.key = key;
    // 구조 핀(상태 기계 전이 등)은 경계 핀 종류가 아니다(GI15).
    if (descriptor->kind == PinKind::Structural) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    out.kind = descriptor->kind;
    out.maxEdges = input && descriptor->maxEdges != 0 ? descriptor->maxEdges : 1;
    out.type = Name();
    const GraphSchema* schema = view.Registry().FindSchema(view.Document().domain);
    if (schema != nullptr && descriptor->type < schema->PinTypes().Count()) {
        auto type =
            view.Registry().Names().TryIntern(schema->PinTypes().types[descriptor->type].name);
        if (!type) {
            return Result<void>::Failure(type.Error());
        }

        out.type = type.Value();
    }

    return Result<void>::Success();
}

} // namespace

Result<void> BuildCollapseCommands(const GraphReadView& view, const GraphDocumentLayout* layout,
                                   Guid graphId, Span<const Guid> nodes, GraphIdSource& ids,
                                   GraphCommandList& out) noexcept {
    const NodeTypeRegistry& registry = view.Registry();
    NamePool& names = registry.Names();
    Allocator& allocator = registry.GetAllocator();
    const Graph* graph = view.FindGraph(graphId);
    if (graph == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (nodes.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < nodes.Size(); ++index) {
        const GraphNode* node = view.FindNode(*graph, nodes[index]);
        if (node == nullptr) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        if (registry.IsBoundary(node->type) || FoldContains(nodes.Slice(0, index), nodes[index])) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    // 건너는 간선을 정규 순서로 안쪽 핀마다 모은다(입력: 바깥 → 안, 출력: 안 → 바깥).
    Array<FoldGroup> inputs(allocator);
    Array<FoldGroup> outputs(allocator);
    Array<GraphEdge> crossing(allocator);
    for (const GraphEdge& edge : graph->edges) {
        const bool fromInside = FoldContains(nodes, edge.fromNode);
        const bool toInside = FoldContains(nodes, edge.toNode);
        if (fromInside == toInside) {
            continue;
        }

        if (auto pushed = crossing.TryPushBack(edge); !pushed) {
            return pushed;
        }

        Array<FoldGroup>& groups = toInside ? inputs : outputs;
        const Guid inner = toInside ? edge.toNode : edge.fromNode;
        const Name pin = toInside ? edge.toPin : edge.fromPin;
        FoldGroup* group = nullptr;
        for (FoldGroup& candidate : groups) {
            if (candidate.node == inner && candidate.pin == pin) {
                group = &candidate;
                break;
            }
        }

        if (group == nullptr) {
            auto added = groups.TryEmplaceBack(allocator);
            if (!added) {
                return Result<void>::Failure(added.Error());
            }

            group = added.Value();
            group->node = inner;
            group->pin = pin;
            auto key =
                FoldKey(names, toInside ? StringView("In") : StringView("Out"), groups.Size() - 1);
            if (!key) {
                return Result<void>::Failure(key.Error());
            }

            if (auto described = FoldDescribe(view, *graph, inner, pin, key.Value(), toInside,
                                              allocator, group->boundary);
                !described) {
                return described;
            }
        }

        if (auto pushed = group->edges.TryPushBack(edge); !pushed) {
            return pushed;
        }
    }

    // 새 id: 하위 그래프·Graph.Collapsed·경계 노드 둘.
    Guid fresh[4];
    for (Guid& id : fresh) {
        auto next = ids.Next();
        if (!next) {
            return Result<void>::Failure(next.Error());
        }

        id = next.Value();
    }

    const Guid subgraph = fresh[0];
    const Guid collapsed = fresh[1];
    const Guid inputNode = fresh[2];
    const Guid outputNode = fresh[3];

    // 배치: 옮긴 노드 위치의 경계 상자와 중심.
    float minX = 0.0f;
    float maxX = 0.0f;
    float sumX = 0.0f;
    float sumY = 0.0f;
    usize placed = 0;
    for (const Guid& id : nodes) {
        if (const NodeLayout* position = FoldFindLayout(layout, graphId, id); position != nullptr) {
            minX = placed == 0 || position->position.x < minX ? position->position.x : minX;
            maxX = placed == 0 || position->position.x > maxX ? position->position.x : maxX;
            sumX += position->position.x;
            sumY += position->position.y;
            ++placed;
        }
    }

    const float centerX = placed == 0 ? 0.0f : sumX / static_cast<float>(placed);
    const float centerY = placed == 0 ? 0.0f : sumY / static_cast<float>(placed);
    const auto makeLayout = [&allocator](Guid node, float x, float y) noexcept {
        NodeLayout position(allocator);
        position.node = node;
        position.position = Vec2(x, y);
        return position;
    };

    const auto makeNode = [&allocator](Guid id, Name type) noexcept {
        GraphNode node(allocator);
        node.id = id;
        node.type = type;
        node.typeSchemaVersion = BoundarySchemaVersion;
        return node;
    };

    Array<GraphBoundaryPin> boundaryInputs(allocator);
    Array<GraphBoundaryPin> boundaryOutputs(allocator);
    for (const FoldGroup& group : inputs) {
        if (auto pushed = boundaryInputs.TryPushBack(group.boundary); !pushed) {
            return pushed;
        }
    }

    for (const FoldGroup& group : outputs) {
        if (auto pushed = boundaryOutputs.TryPushBack(group.boundary); !pushed) {
            return pushed;
        }
    }

    // 순서: 건너는 간선 끊기 → 하위 그래프·경계 → 경계 노드 → 옮기기 → Graph.Collapsed → 다시 잇기.
    for (const GraphEdge& edge : crossing) {
        if (auto added = out.Disconnect(graphId, edge); !added) {
            return added;
        }
    }

    if (auto added = out.AddGraph(subgraph, GraphRole::Collapsed, 0, "Collapsed", collapsed);
        !added) {
        return added;
    }

    if (auto added = out.SetGraphBoundary(subgraph, boundaryInputs.View(), boundaryOutputs.View());
        !added) {
        return added;
    }

    if (auto added = out.AddNode(subgraph, makeNode(inputNode, registry.GraphInputType()),
                                 makeLayout(inputNode, minX - FoldBoundaryOffset, centerY));
        !added) {
        return added;
    }

    if (auto added = out.AddNode(subgraph, makeNode(outputNode, registry.GraphOutputType()),
                                 makeLayout(outputNode, maxX + FoldBoundaryOffset, centerY));
        !added) {
        return added;
    }

    if (auto added = out.MoveNodesToGraph(graphId, nodes, subgraph); !added) {
        return added;
    }

    if (auto added = out.AddNode(graphId, makeNode(collapsed, registry.CollapsedType()),
                                 makeLayout(collapsed, centerX, centerY));
        !added) {
        return added;
    }

    for (const FoldGroup& group : inputs) {
        const GraphEdge inside = FoldEdge(inputNode, group.boundary.key, group.node, group.pin);
        if (auto added = out.Connect(subgraph, inside, true); !added) {
            return added;
        }

        for (const GraphEdge& edge : group.edges) {
            const GraphEdge outside =
                FoldEdge(edge.fromNode, edge.fromPin, collapsed, group.boundary.key);
            if (auto added = out.Connect(graphId, outside, true); !added) {
                return added;
            }
        }
    }

    for (const FoldGroup& group : outputs) {
        const GraphEdge inside = FoldEdge(group.node, group.pin, outputNode, group.boundary.key);
        if (auto added = out.Connect(subgraph, inside, true); !added) {
            return added;
        }

        for (const GraphEdge& edge : group.edges) {
            const GraphEdge outside =
                FoldEdge(collapsed, group.boundary.key, edge.toNode, edge.toPin);
            if (auto added = out.Connect(graphId, outside, true); !added) {
                return added;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> BuildExpandCommands(const GraphReadView& view, Guid graphId, Guid collapsedNode,
                                 GraphCommandList& out, GraphFoldReport* report) noexcept {
    const NodeTypeRegistry& registry = view.Registry();
    Allocator& allocator = registry.GetAllocator();
    const Graph* graph = view.FindGraph(graphId);
    const GraphNode* node = graph != nullptr ? view.FindNode(*graph, collapsedNode) : nullptr;
    if (node == nullptr || !registry.IsCollapsed(node->type)) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    const Graph* subgraph = nullptr;
    for (const Graph& candidate : view.Document().graphs) {
        if (candidate.role == GraphRole::Collapsed && candidate.ownerNode == collapsedNode) {
            subgraph = &candidate;
            break;
        }
    }

    if (subgraph == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    Guid inputNode;
    Guid outputNode;
    Array<Guid> inner(allocator);
    for (const GraphNode& child : subgraph->nodes) {
        if (registry.IsGraphInput(child.type)) {
            inputNode = child.id;
        } else if (registry.IsGraphOutput(child.type)) {
            outputNode = child.id;
        } else if (auto pushed = inner.TryPushBack(child.id); !pushed) {
            return pushed;
        }
    }

    // 고아 핀(경계에서 사라진 핀)의 간선·값은 이을 곳이 없으므로 펼치지 않는다(조용히 버리지
    // 않는다).
    if (!node->retainedPins.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    for (const GraphNode& child : subgraph->nodes) {
        if (registry.IsBoundary(child.type) && !child.retainedPins.IsEmpty()) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }
    }

    // 끊을 간선: 부모 쪽 Graph.Collapsed 간선과 하위 그래프 쪽 경계 노드 간선.
    Array<GraphEdge> outsideEdges(allocator);
    Array<GraphEdge> insideEdges(allocator);
    for (const GraphEdge& edge : graph->edges) {
        if (edge.fromNode == collapsedNode || edge.toNode == collapsedNode) {
            if (auto pushed = outsideEdges.TryPushBack(edge); !pushed) {
                return pushed;
            }
        }
    }

    for (const GraphEdge& edge : subgraph->edges) {
        if (edge.fromNode == inputNode || edge.toNode == outputNode) {
            if (auto pushed = insideEdges.TryPushBack(edge); !pushed) {
                return pushed;
            }
        }
    }

    for (const GraphEdge& edge : outsideEdges) {
        if (auto added = out.Disconnect(graphId, edge); !added) {
            return added;
        }
    }

    for (const GraphEdge& edge : insideEdges) {
        if (auto added = out.Disconnect(subgraph->id, edge); !added) {
            return added;
        }
    }

    if (!inner.IsEmpty()) {
        if (auto added = out.MoveNodesToGraph(subgraph->id, inner.View(), graphId); !added) {
            return added;
        }
    }

    // 하위 그래프(이제 경계 노드만)는 Graph.Collapsed를 지울 때 연쇄로 지워진다.
    if (auto added = out.RemoveNodes(graphId, Span<const Guid>(&collapsedNode, 1)); !added) {
        return added;
    }

    // 경계 핀마다 바깥 끝 × 안쪽 끝을 다시 잇는다. Graph.Input에서 Graph.Output으로 바로 가는 간선
    // (그대로 지나는 경계)은 바깥 시작 × 그 출력 키의 바깥 끝으로 잇는다.
    const auto connectThrough = [&](Guid from, Name fromPin,
                                    Name outputKey) noexcept -> Result<void> {
        for (const GraphEdge& outside : outsideEdges) {
            if (outside.fromNode == collapsedNode && outside.fromPin == outputKey) {
                if (auto added = out.Connect(
                        graphId, FoldEdge(from, fromPin, outside.toNode, outside.toPin), true);
                    !added) {
                    return added;
                }
            }
        }

        return Result<void>::Success();
    };

    for (const GraphBoundaryPin& boundary : subgraph->inputs) {
        bool linked = false;
        for (const GraphEdge& outside : outsideEdges) {
            if (!(outside.toNode == collapsedNode) || !(outside.toPin == boundary.key)) {
                continue;
            }

            linked = true;
            for (const GraphEdge& inside : insideEdges) {
                if (!(inside.fromNode == inputNode) || !(inside.fromPin == boundary.key)) {
                    continue;
                }

                const Result<void> added =
                    inside.toNode == outputNode
                        ? connectThrough(outside.fromNode, outside.fromPin, inside.toPin)
                        : out.Connect(graphId,
                                      FoldEdge(outside.fromNode, outside.fromPin, inside.toNode,
                                               inside.toPin),
                                      true);
                if (!added) {
                    return Result<void>::Failure(added.Error());
                }
            }
        }

        // 바깥 간선이 없던 입력의 기본값은 안쪽 받는 핀으로 옮긴다. 받는 핀이 없거나 Hidden이거나
        // 그대로 지나는 경계면 버리고 센다.
        const PinValueEntry* value = nullptr;
        for (const PinValueEntry& entry : node->inputDefaults) {
            if (entry.pin == boundary.key) {
                value = &entry;
            }
        }

        if (linked || value == nullptr) {
            continue;
        }

        bool moved = false;
        for (const GraphEdge& inside : insideEdges) {
            if (!(inside.fromNode == inputNode) || !(inside.fromPin == boundary.key) ||
                inside.toNode == outputNode) {
                continue;
            }

            const GraphNode* target = view.FindNode(*subgraph, inside.toNode);
            PinList pins(allocator);
            const PinDescriptor* pin = nullptr;
            if (target != nullptr && view.CollectPins(*subgraph, *target, pins)) {
                pin = pins.Find(inside.toPin);
            }

            if (pin == nullptr || pin->valuePolicy == PinValuePolicy::Hidden) {
                continue;
            }

            if (auto added = out.SetInputDefault(graphId, inside.toNode, inside.toPin,
                                                 value->value.bytes.View());
                !added) {
                return added;
            }

            moved = true;
        }

        if (!moved && report != nullptr) {
            ++report->droppedDefaults;
        }
    }

    for (const GraphBoundaryPin& boundary : subgraph->outputs) {
        for (const GraphEdge& inside : insideEdges) {
            // 그대로 지나는 간선은 입력 쪽에서 이었다.
            if (!(inside.toNode == outputNode) || !(inside.toPin == boundary.key) ||
                inside.fromNode == inputNode) {
                continue;
            }

            if (auto added = connectThrough(inside.fromNode, inside.fromPin, boundary.key);
                !added) {
                return added;
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::nodegraph
