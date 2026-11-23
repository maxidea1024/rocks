#include <Core/Algorithms.h>
#include <NodeGraph/GraphNameOrder.h>
#include <NodeGraph/GraphReadView.h>
#include <NodeGraph/NodeTypeRegistry.h>

// 문서 읽기 view와 순환 검사 도우미다(Docs/Design/NodeGraph.ko.md 7·8.6절).
namespace rocks::nodegraph {

namespace {

// (출력 노드, 입력 노드) 간선 쌍이다(순환 검사 인접 표).
struct ReadViewLink {
    Guid from;
    Guid to;
};

} // namespace

const Graph* GraphReadView::FindGraph(Guid id) const noexcept {
    usize low = 0;
    usize high = document_.graphs.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const int32 compared = CompareGuid(document_.graphs[middle].id, id);
        if (compared == 0) {
            return &document_.graphs[middle];
        }

        if (compared < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return nullptr;
}

const GraphNode* GraphReadView::FindNode(const Graph& graph, Guid id) const noexcept {
    usize low = 0;
    usize high = graph.nodes.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const int32 compared = CompareGuid(graph.nodes[middle].id, id);
        if (compared == 0) {
            return &graph.nodes[middle];
        }

        if (compared < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return nullptr;
}

Result<void> GraphReadView::CollectPins(const Graph& graph, const GraphNode& node,
                                        PinList& out) const noexcept {
    return registry_.CollectPins(document_, graph, node, out);
}

Span<const GraphEdge> GraphReadView::EdgesInto(const Graph& graph, Guid node) const noexcept {
    const Span<const GraphEdge> edges = graph.edges.View();
    usize low = 0;
    usize high = edges.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (CompareGuid(edges[middle].toNode, node) < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    usize end = low;
    while (end < edges.Size() && edges[end].toNode == node) {
        ++end;
    }

    return Span<const GraphEdge>(edges.Data() + low, end - low);
}

Result<void> GraphReadView::CollectEdgesFrom(const Graph& graph, Guid node, Name pin,
                                             Array<GraphEdge>& out) const noexcept {
    out.Clear();
    for (const GraphEdge& edge : graph.edges) {
        if (edge.fromNode == node && (pin.IsNone() || edge.fromPin == pin)) {
            if (auto pushed = out.TryPushBack(edge); !pushed) {
                return pushed;
            }
        }
    }

    algo::Sort(out.View(), [](const GraphEdge& left, const GraphEdge& right) noexcept {
        return left.order < right.order;
    });

    return Result<void>::Success();
}

Result<bool> WouldCreateCycle(const Graph& graph, PinRef from, PinRef to,
                              Allocator& scratch) noexcept {
    if (from.node == to.node) {
        return Result<bool>::Success(true);
    }

    // 출력 노드 순 인접 표를 만들고 to에서 너비 우선으로 따라간다.
    Array<ReadViewLink> links(scratch);
    if (auto reserved = links.TryReserve(graph.edges.Size()); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    for (const GraphEdge& edge : graph.edges) {
        if (auto pushed = links.TryPushBack(ReadViewLink{edge.fromNode, edge.toNode}); !pushed) {
            return Result<bool>::Failure(pushed.Error());
        }
    }

    algo::Sort(links.View(), [](const ReadViewLink& left, const ReadViewLink& right) noexcept {
        return CompareGuid(left.from, right.from) < 0;
    });

    Array<Guid> frontier(scratch);
    Array<Guid> visited(scratch);
    if (auto pushed = frontier.TryPushBack(to.node); !pushed) {
        return Result<bool>::Failure(pushed.Error());
    }

    while (!frontier.IsEmpty()) {
        const Guid current = frontier[frontier.Size() - 1];
        static_cast<void>(frontier.TryRemoveAt(frontier.Size() - 1));
        if (current == from.node) {
            return Result<bool>::Success(true);
        }

        usize position = 0;
        usize end = visited.Size();
        while (position < end) {
            const usize middle = position + (end - position) / 2;
            if (CompareGuid(visited[middle], current) < 0) {
                position = middle + 1;
            } else {
                end = middle;
            }
        }

        if (position < visited.Size() && visited[position] == current) {
            continue;
        }

        if (auto inserted = visited.TryInsert(current, position); !inserted) {
            return Result<bool>::Failure(inserted.Error());
        }

        usize low = 0;
        usize high = links.Size();
        while (low < high) {
            const usize middle = low + (high - low) / 2;
            if (CompareGuid(links[middle].from, current) < 0) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }

        for (usize index = low; index < links.Size() && links[index].from == current; ++index) {
            if (auto pushed = frontier.TryPushBack(links[index].to); !pushed) {
                return Result<bool>::Failure(pushed.Error());
            }
        }
    }

    return Result<bool>::Success(false);
}

} // namespace rocks::nodegraph
