#include "GraphNameRanks.h"

#include <Base/NumericLimits.h>
#include <Core/Algorithms.h>
#include <NodeGraph/GraphNameOrder.h>
#include <NodeGraph/GraphSemantic.h>
#include <Serialization/Archive.h>

// 의미 바이트와 semantic hash다(Docs/Design/NodeGraph.ko.md 8.4절).
namespace rocks::nodegraph {

namespace {

// 의미 바이트 머리의 magic이다.
constexpr uint8 SemanticMagic[4] = {'R', 'N', 'G', 'S'};

// 의미 바이트 writer의 예산이다. 바이트 상한은 MaxSemanticBytes, 값 하나(속성 Record)는
// MaxPropertyRecordBytes, 연산·작업 수는 노드 그래프 상한(그래프마다 노드 65,536·간선 262,144)의 큰
// 문서도 받도록 넉넉히 둔다.
constexpr uint64 SemanticMaxOperations = 64ull * 1024ull * 1024ull;
constexpr uint64 SemanticMaxWork = 4ull * 1024ull * 1024ull * 1024ull;

constexpr uint32 SemanticNoNode = NumericLimits<uint32>::Max();

// 접은 간선 하나다.
struct SemanticEdge {
    Guid toNode;
    Name toPin;
    Guid fromNode;
    Name fromPin;
    uint32 order = 0;
};

// 그래프 안 노드 id → 노드 색인 표 항목이다.
struct SemanticNodeKey {
    Guid id;
    uint32 index = 0;
};

uint32 SemanticFindNode(Span<const SemanticNodeKey> keys, const Guid& id) noexcept {
    usize low = 0;
    usize high = keys.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const int32 compared = CompareGuid(keys[middle].id, id);
        if (compared == 0) {
            return keys[middle].index;
        }

        if (compared < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return SemanticNoNode;
}

} // namespace

namespace detail {

class SemanticEncoder {
public:
    SemanticEncoder(const GraphDocument& document, const NodeTypeRegistry& registry,
                    Allocator& scratch) noexcept
        : document_(document), registry_(registry),
          writer_(scratch, ArchiveLimits{MaxSemanticBytes, MaxPropertyRecordBytes,
                                         SemanticMaxOperations, SemanticMaxWork}),
          ranks_(scratch, registry.Names()), lowered_(scratch), keys_(scratch), upstream_(scratch),
          edges_(scratch), order_(scratch) {
    }

    Result<void> Run() noexcept {
        if (auto added = ranks_.AddDocument(document_); !added) {
            return added;
        }

        if (auto built = ranks_.Build(); !built) {
            return built;
        }

        if (auto written =
                writer_.WriteRaw(Span<const uint8>(SemanticMagic, sizeof(SemanticMagic)));
            !written) {
            return written;
        }

        if (auto written = writer_.WriteU32(SemanticBytesFormatVersion); !written) {
            return written;
        }

        if (auto written = WriteName(document_.domain); !written) {
            return written;
        }

        if (auto written = writer_.WriteGuid(document_.mainGraph); !written) {
            return written;
        }

        if (auto written = writer_.WriteU32(static_cast<uint32>(document_.graphs.Size()));
            !written) {
            return written;
        }

        // 그래프는 GraphId 순으로 쓴다(입력 배열 순서와 무관하다).
        Array<uint32> graphOrder(order_.GetAllocator());
        if (auto filled = FillOrder(graphOrder, document_.graphs.Size()); !filled) {
            return filled;
        }

        algo::Sort(graphOrder.View(), [this](uint32 left, uint32 right) noexcept {
            return CompareGuid(document_.graphs[left].id, document_.graphs[right].id) < 0;
        });

        const GraphSchema* schema = registry_.FindSchema(document_.domain);
        for (uint32 graph : graphOrder) {
            if (auto written = WriteGraph(document_.graphs[graph], schema); !written) {
                return written;
            }
        }

        return Result<void>::Success();
    }

    Array<uint8> TakeBytes() noexcept {
        return writer_.TakeBytes();
    }

    // SemanticNodeWriter::WriteNode가 부른다. 실패하면 첫 오류를 기억한다(schema가 오류를 무시해도
    // 쓰기가 성공으로 끝나지 않게 — WriteGraph가 확인한다).
    Result<void> WriteNodeBody(const GraphNode& node, Span<const uint8> properties) noexcept {
        auto written = WriteNodeFields(node, properties);
        if (!written && failure_ == ErrorCode::None) {
            failure_ = written.Error();
        }

        return written;
    }

private:
    // [0, count) 색인을 order에 채운다.
    static Result<void> FillOrder(Array<uint32>& order, usize count) noexcept {
        order.Clear();
        if (auto reserved = order.TryReserve(count); !reserved) {
            return reserved;
        }

        for (usize index = 0; index < count; ++index) {
            if (auto pushed = order.TryPushBack(static_cast<uint32>(index)); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    }

    Result<void> WriteNodeFields(const GraphNode& node, Span<const uint8> properties) noexcept {
        if (auto written = writer_.WriteGuid(node.id); !written) {
            return written;
        }

        if (auto written = WriteName(node.type); !written) {
            return written;
        }

        if (auto written = writer_.WriteU32(node.typeSchemaVersion); !written) {
            return written;
        }

        if (auto written = writer_.WriteBytes(properties); !written) {
            return written;
        }

        if (auto written = writer_.WriteU32(static_cast<uint32>(node.inputDefaults.Size()));
            !written) {
            return written;
        }

        // 입력 기본값·고아 핀은 PinKey 원문 순으로 쓴다(입력 배열 순서와 무관하다).
        if (auto filled = FillOrder(order_, node.inputDefaults.Size()); !filled) {
            return filled;
        }

        algo::Sort(order_.View(), [this, &node](uint32 left, uint32 right) noexcept {
            return ranks_.Rank(node.inputDefaults[left].pin) <
                   ranks_.Rank(node.inputDefaults[right].pin);
        });

        for (uint32 index : order_) {
            const PinValueEntry& entry = node.inputDefaults[index];
            if (auto written = WriteName(entry.pin); !written) {
                return written;
            }

            if (auto written = writer_.WriteBytes(entry.value.bytes.View()); !written) {
                return written;
            }
        }

        if (auto written = writer_.WriteU8(static_cast<uint8>(node.enableState)); !written) {
            return written;
        }

        if (auto written = writer_.WriteU32(static_cast<uint32>(node.retainedPins.Size()));
            !written) {
            return written;
        }

        if (auto filled = FillOrder(order_, node.retainedPins.Size()); !filled) {
            return filled;
        }

        algo::Sort(order_.View(), [this, &node](uint32 left, uint32 right) noexcept {
            return ranks_.Rank(node.retainedPins[left].key) <
                   ranks_.Rank(node.retainedPins[right].key);
        });

        for (uint32 index : order_) {
            const RetainedPin& pin = node.retainedPins[index];
            if (auto written = WriteName(pin.key); !written) {
                return written;
            }

            if (auto written = writer_.WriteU8(static_cast<uint8>(pin.direction)); !written) {
                return written;
            }

            if (auto written = writer_.WriteU32(pin.lastType); !written) {
                return written;
            }

            if (auto written = writer_.WriteBytes(pin.value.bytes.View()); !written) {
                return written;
            }
        }

        return Result<void>::Success();
    }

    // Name을 정규 원문(소문자 base + 번호)으로 쓴다.
    Result<void> WriteName(Name name) noexcept {
        const uint32 rank = ranks_.Rank(name);
        if (rank == UnrankedName) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto number = CanonicalNameParts(ranks_.Text(name), lowered_);
        if (!number) {
            return Result<void>::Failure(number.Error());
        }

        if (auto written = writer_.WriteString(StringView(lowered_.Data(), lowered_.Size()));
            !written) {
            return written;
        }

        return writer_.WriteU32(number.Value());
    }

    Result<void> BuildNodeKeys(const Graph& graph) noexcept {
        keys_.Clear();
        upstream_.Clear();
        if (auto reserved = keys_.TryReserve(graph.nodes.Size()); !reserved) {
            return reserved;
        }

        if (auto grown = upstream_.TryResize(graph.nodes.Size()); !grown) {
            return grown;
        }

        for (usize index = 0; index < graph.nodes.Size(); ++index) {
            SemanticNodeKey key;
            key.id = graph.nodes[index].id;
            key.index = static_cast<uint32>(index);
            if (auto pushed = keys_.TryPushBack(key); !pushed) {
                return pushed;
            }

            upstream_[index] = SemanticNoNode;
        }

        algo::Sort(keys_.View(),
                   [](const SemanticNodeKey& left, const SemanticNodeKey& right) noexcept {
                       return CompareGuid(left.id, right.id) < 0;
                   });

        // 재경유마다 입력 간선(재경유로 들어오는 간선)을 기억한다. upstream_[재경유] = 그 간선의
        // 색인.
        for (usize index = 0; index < graph.edges.Size(); ++index) {
            const GraphEdge& edge = graph.edges[index];
            const uint32 to = SemanticFindNode(keys_.View(), edge.toNode);
            if (to == SemanticNoNode || !registry_.IsReroute(graph.nodes[to].type)) {
                continue;
            }

            if (upstream_[to] != SemanticNoNode) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            upstream_[to] = static_cast<uint32>(index);
        }

        return Result<void>::Success();
    }

    // edge의 출력 쪽이 재경유면 사슬을 거슬러 원천을 찾아 folded에 채운다. 원천이 없으면 false를
    // 돌려준다.
    Result<bool> Fold(const Graph& graph, const GraphEdge& edge, SemanticEdge& folded) noexcept {
        folded.toNode = edge.toNode;
        folded.toPin = edge.toPin;
        folded.fromNode = edge.fromNode;
        folded.fromPin = edge.fromPin;
        folded.order = edge.order;
        uint32 current = SemanticFindNode(keys_.View(), edge.fromNode);
        usize steps = 0;
        while (current != SemanticNoNode && registry_.IsReroute(graph.nodes[current].type)) {
            if (steps > graph.nodes.Size()) {
                return Result<bool>::Failure(ErrorCode::CycleDetected);
            }

            ++steps;
            const uint32 incoming = upstream_[current];
            if (incoming == SemanticNoNode) {
                return Result<bool>::Success(false);
            }

            const GraphEdge& source = graph.edges[incoming];
            folded.fromNode = source.fromNode;
            folded.fromPin = source.fromPin;
            folded.order = source.order;
            current = SemanticFindNode(keys_.View(), source.fromNode);
        }

        return Result<bool>::Success(true);
    }

    Result<void> WriteGraph(const Graph& graph, const GraphSchema* schema) noexcept {
        if (auto built = BuildNodeKeys(graph); !built) {
            return built;
        }

        if (auto written = writer_.WriteGuid(graph.id); !written) {
            return written;
        }

        if (auto written = writer_.WriteU8(static_cast<uint8>(graph.role)); !written) {
            return written;
        }

        if (auto written = writer_.WriteU32(graph.domainRole); !written) {
            return written;
        }

        uint32 nodeCount = 0;
        for (const GraphNode& node : graph.nodes) {
            if (!registry_.IsReroute(node.type)) {
                ++nodeCount;
            }
        }

        if (auto written = writer_.WriteU32(nodeCount); !written) {
            return written;
        }

        // 노드는 NodeId 순(keys_ — BuildNodeKeys가 정렬했다)으로 쓴다.
        for (const SemanticNodeKey& key : keys_) {
            const GraphNode& node = graph.nodes[key.index];
            if (registry_.IsReroute(node.type)) {
                continue;
            }

            SemanticNodeWriter nodeWriter(*this, node);
            auto normalized = schema != nullptr ? schema->NormalizeSemanticNode(node, nodeWriter)
                                                : nodeWriter.WriteNode(node);
            if (!normalized) {
                return normalized;
            }

            if (failure_ != ErrorCode::None) {
                return Result<void>::Failure(failure_);
            }

            if (!nodeWriter.written_) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        edges_.Clear();
        for (const GraphEdge& edge : graph.edges) {
            const uint32 to = SemanticFindNode(keys_.View(), edge.toNode);
            if (to != SemanticNoNode && registry_.IsReroute(graph.nodes[to].type)) {
                continue;
            }

            SemanticEdge folded;
            auto kept = Fold(graph, edge, folded);
            if (!kept) {
                return Result<void>::Failure(kept.Error());
            }

            if (!kept.Value()) {
                continue;
            }

            if (auto pushed = edges_.TryPushBack(folded); !pushed) {
                return pushed;
            }
        }

        algo::Sort(edges_.View(), [this](const SemanticEdge& left,
                                         const SemanticEdge& right) noexcept {
            if (const int32 compared = CompareGuid(left.toNode, right.toNode); compared != 0) {
                return compared < 0;
            }

            if (ranks_.Rank(left.toPin) != ranks_.Rank(right.toPin)) {
                return ranks_.Rank(left.toPin) < ranks_.Rank(right.toPin);
            }

            if (const int32 compared = CompareGuid(left.fromNode, right.fromNode); compared != 0) {
                return compared < 0;
            }

            if (ranks_.Rank(left.fromPin) != ranks_.Rank(right.fromPin)) {
                return ranks_.Rank(left.fromPin) < ranks_.Rank(right.fromPin);
            }

            return left.order < right.order;
        });

        if (auto written = writer_.WriteU32(static_cast<uint32>(edges_.Size())); !written) {
            return written;
        }

        for (const SemanticEdge& edge : edges_) {
            if (auto written = writer_.WriteGuid(edge.toNode); !written) {
                return written;
            }

            if (auto written = WriteName(edge.toPin); !written) {
                return written;
            }

            if (auto written = writer_.WriteGuid(edge.fromNode); !written) {
                return written;
            }

            if (auto written = WriteName(edge.fromPin); !written) {
                return written;
            }

            if (auto written = writer_.WriteU32(edge.order); !written) {
                return written;
            }
        }

        return Result<void>::Success();
    }

    const GraphDocument& document_;
    const NodeTypeRegistry& registry_;
    ArchiveWriter writer_;
    GraphNameRanks ranks_;
    Array<char> lowered_;
    Array<SemanticNodeKey> keys_;
    Array<uint32> upstream_;
    Array<SemanticEdge> edges_;
    Array<uint32> order_;
    ErrorCode failure_ = ErrorCode::None;
};

} // namespace detail

Result<void> SemanticNodeWriter::WriteNode(const GraphNode& node) noexcept {
    return WriteNode(node, node.properties.bytes.View());
}

Result<void> SemanticNodeWriter::WriteNode(const GraphNode& node,
                                           Span<const uint8> properties) noexcept {
    if (&node != &expected_ || written_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    written_ = true;
    return encoder_.WriteNodeBody(node, properties);
}

Result<void> WriteSemanticBytes(const GraphDocument& document, const NodeTypeRegistry& registry,
                                Allocator& scratch, Array<uint8>& out) noexcept {
    out.Clear();
    detail::SemanticEncoder encoder(document, registry, scratch);
    if (auto encoded = encoder.Run(); !encoded) {
        return encoded;
    }

    Array<uint8> bytes = encoder.TakeBytes();
    if (auto reserved = out.TryReserve(bytes.Size()); !reserved) {
        return reserved;
    }

    return out.TryAppend(bytes.View());
}

Result<Sha256Digest> ComputeSemanticHash(const GraphDocument& document,
                                         const NodeTypeRegistry& registry,
                                         Allocator& scratch) noexcept {
    detail::SemanticEncoder encoder(document, registry, scratch);
    if (auto encoded = encoder.Run(); !encoded) {
        return Result<Sha256Digest>::Failure(encoded.Error());
    }

    const Array<uint8> bytes = encoder.TakeBytes();
    return Result<Sha256Digest>::Success(Sha256::Hash(bytes.View()));
}

} // namespace rocks::nodegraph
