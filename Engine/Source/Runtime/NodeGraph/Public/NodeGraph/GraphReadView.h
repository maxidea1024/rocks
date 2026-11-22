#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <NodeGraph/GraphDocument.h>
#include <NodeGraph/NodeTypeDescriptor.h>

// 문서와 등록표의 읽기 전용 view다(Docs/Design/NodeGraph.ko.md 7·8.6절). schema
// 판정(CanConnect·Validate)과 화면이 문서를 바꾸지 않고 조회할 때 쓴다. 문서는 정규 순서여야
// 한다(GraphMutator가 지킨다).
//
// 사용 예:
//   const Graph* graph = view.FindGraph(graphId);
//   const GraphNode* node = graph ? view.FindNode(*graph, nodeId) : nullptr;
//   PinList pins(allocator);
//   view.CollectPins(*graph, *node, pins);
namespace rocks::nodegraph {

class NodeTypeRegistry;

// 핀 하나의 위치다.
struct PinRef {
    Guid node;
    Name pin;
};

class ROCKS_NODEGRAPH_API GraphReadView {
public:
    GraphReadView(const GraphDocument& document, const NodeTypeRegistry& registry) noexcept
        : document_(document), registry_(registry) {
    }

    const GraphDocument& Document() const noexcept {
        return document_;
    }

    const NodeTypeRegistry& Registry() const noexcept {
        return registry_;
    }

    // id인 그래프다(이진 탐색). 없으면 nullptr이다.
    const Graph* FindGraph(Guid id) const noexcept;

    // graph에서 id인 노드다(이진 탐색). 없으면 nullptr이다.
    const GraphNode* FindNode(const Graph& graph, Guid id) const noexcept;

    // node의 현재 핀 목록이다(NodeTypeRegistry::CollectPins).
    Result<void> CollectPins(const Graph& graph, const GraphNode& node,
                             PinList& out) const noexcept;

    // node로 들어오는 간선들이다(간선 정규 순서의 연속 구간).
    Span<const GraphEdge> EdgesInto(const Graph& graph, Guid node) const noexcept;

    // node의 출력 핀 pin(None이면 모든 출력 핀)에서 나가는 간선들을 out에 채운다(먼저 비운다, 간선
    // 순서).
    Result<void> CollectEdgesFrom(const Graph& graph, Guid node, Name pin,
                                  Array<GraphEdge>& out) const noexcept;

private:
    const GraphDocument& document_;
    const NodeTypeRegistry& registry_;
};

// graph에 from(출력 핀) → to(입력 핀) 간선을 더하면 순환이 생기는가: to 노드에서 나가는 간선을 따라
// from 노드에 닿는지 본다(재경유 포함, 7절 순환 검사 도우미). from과 to가 같은 노드면 참이다.
// 비용은 도달 범위에 비례하고, 인접 표를 만드는 데 간선 수에 비례하는 할당을 한다.
ROCKS_NODEGRAPH_API Result<bool> WouldCreateCycle(const Graph& graph, PinRef from, PinRef to,
                                                  Allocator& scratch) noexcept;

} // namespace rocks::nodegraph
