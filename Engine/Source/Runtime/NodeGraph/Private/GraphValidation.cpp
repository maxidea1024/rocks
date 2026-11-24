#include "GraphNameRanks.h"

#include <Base/NumericLimits.h>
#include <Core/Algorithms.h>
#include <NodeGraph/GraphNameOrder.h>
#include <NodeGraph/GraphValidation.h>
#include <Serialization/Record.h>

// 문서 정규화와 전체 불변식 검사(GI1–GI14)다(Docs/Design/NodeGraph 6.3절).
namespace rocks::nodegraph {

bool GraphViolationList::Has(GraphInvariant rule) const noexcept {
    for (const GraphViolation& violation : items) {
        if (violation.rule == rule) {
            return true;
        }
    }

    return false;
}

bool GraphViolationList::Has(GraphInvariant rule, GraphSubject subject) const noexcept {
    for (const GraphViolation& violation : items) {
        if (violation.rule == rule && violation.subject == subject) {
            return true;
        }
    }

    return false;
}

namespace {

using detail::GraphNameRanks;

// 위반 등록기다. 할당 실패는 첫 오류를 기억하고 이후 등록을 무시한다(검사 끝에 돌려준다).
class ValidationReporter {
public:
    explicit ValidationReporter(GraphViolationList& out) noexcept : out_(out) {
    }

    void Add(GraphInvariant rule, GraphSubject subject, const Guid& graph, const Guid& node,
             const Guid& related, Name pin, usize index) noexcept {
        if (failure_ != ErrorCode::None) {
            return;
        }

        if (out_.items.Size() >= MaxReportedGraphViolations) {
            if (out_.dropped < NumericLimits<uint32>::Max()) {
                ++out_.dropped;
            }

            return;
        }

        GraphViolation violation;
        violation.rule = rule;
        violation.subject = subject;
        violation.graph = graph;
        violation.node = node;
        violation.related = related;
        violation.pin = pin;
        violation.index = static_cast<uint32>(index);
        if (auto pushed = out_.items.TryPushBack(violation); !pushed) {
            failure_ = pushed.Error();
        }
    }

    // 검사 결과다: 할당 실패가 있었으면 그 오류, 위반이 있으면 InvalidArgument, 아니면 성공.
    Result<void> Finish() const noexcept {
        if (failure_ != ErrorCode::None) {
            return Result<void>::Failure(failure_);
        }

        if (!out_.items.IsEmpty() || out_.dropped != 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<void>::Success();
    }

private:
    GraphViolationList& out_;
    ErrorCode failure_ = ErrorCode::None;
};

constexpr Guid ValidationNoId{};
constexpr uint32 ValidationNoInfo = NumericLimits<uint32>::Max();

bool ValidationIsZero(const Guid& id) noexcept {
    return id.high == 0 && id.low == 0;
}

bool ValidationIsNoneName(Name name) noexcept {
    return !name.IsValid() || name.IsNone();
}

// 노드 하나의 검사 정보다. 핀 목록은 pinPool의 [pinBegin, pinBegin + pinCount)다.
struct ValidationNodeInfo {
    const GraphNode* node = nullptr;
    const RegisteredNodeType* type = nullptr;
    // 현재 핀 목록을 안다(알려진 종류이고 핀 목록 계산이 성공했다).
    bool pinsKnown = false;
    bool reroute = false;
    uint32 pinBegin = 0;
    uint32 pinCount = 0;
    // 고아 핀 사용 표지 배열(retainedUsed)에서 이 노드의 시작이다.
    uint32 retainedBegin = 0;
    // 재경유면 입력 간선의 출력 노드 정보 색인이다.
    uint32 upstream = ValidationNoInfo;
    // GI10 순회 상태(0 미방문, 1 경로 위, 2 끝남)다.
    uint8 walk = 0;
};

struct ValidationNodeKey {
    Guid id;
    uint32 info = 0;
};

// id 순으로 정렬된 keys에서 id의 info를 찾는다. 없으면 ValidationNoInfo다.
uint32 ValidationFindKey(Span<const ValidationNodeKey> keys, const Guid& id) noexcept {
    usize low = 0;
    usize high = keys.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const int32 compared = CompareGuid(keys[middle].id, id);
        if (compared == 0) {
            return keys[middle].info;
        }

        if (compared < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return ValidationNoInfo;
}

// 간선 끝 하나의 해석 결과다.
enum class ValidationEnd : uint8 {
    // 핀 목록을 모른다(알 수 없는 종류 등) — 검사하지 않는다.
    Unknown,
    Current,
    Retained,
    Missing
};

struct ValidationEdgeEnd {
    ValidationEnd kind = ValidationEnd::Unknown;
    const PinDescriptor* pin = nullptr;
    uint32 retained = 0;
};

// GI3 계수용 (입력 노드, 입력 핀) 항목이다.
struct ValidationInputUse {
    uint32 info = 0;
    uint32 pinRank = 0;
    uint32 edge = 0;
    uint16 maxEdges = 0;
};

// GI5 순번 검사용 (출력 노드, 출력 핀) 항목이다.
struct ValidationOutputUse {
    uint32 info = 0;
    uint32 pinRank = 0;
    uint32 order = 0;
    uint32 edge = 0;
    bool ordered = false;
};

// GI9 검사용 id 항목이다.
struct ValidationIdUse {
    Guid id;
    Guid graph;
    GraphSubject subject = GraphSubject::Graph;
    uint32 index = 0;
};

int32 ValidationCompareEdges(const GraphEdge& left, const GraphEdge& right,
                             const GraphNameRanks& ranks) noexcept {
    if (const int32 compared = CompareGuid(left.toNode, right.toNode); compared != 0) {
        return compared;
    }

    const uint32 leftTo = ranks.Rank(left.toPin);
    const uint32 rightTo = ranks.Rank(right.toPin);
    if (leftTo != rightTo) {
        return leftTo < rightTo ? -1 : 1;
    }

    if (const int32 compared = CompareGuid(left.fromNode, right.fromNode); compared != 0) {
        return compared;
    }

    const uint32 leftFrom = ranks.Rank(left.fromPin);
    const uint32 rightFrom = ranks.Rank(right.fromPin);
    if (leftFrom != rightFrom) {
        return leftFrom < rightFrom ? -1 : 1;
    }

    return 0;
}

class DocumentValidator {
public:
    DocumentValidator(const GraphDocument& document, const GraphDocumentLayout* layout,
                      const NodeTypeRegistry& registry, Allocator& scratch,
                      GraphViolationList& out) noexcept
        : document_(document), layout_(layout), registry_(registry), scratch_(scratch),
          reporter_(out), ranks_(scratch, registry.Names(), true), infos_(scratch), keys_(scratch),
          pinPool_(scratch), retainedUsed_(scratch), inputUses_(scratch), outputUses_(scratch),
          path_(scratch), pins_(scratch) {
    }

    Result<void> Run() noexcept {
        if (auto added = ranks_.AddDocument(document_); !added) {
            return added;
        }

        if (auto built = ranks_.Build(); !built) {
            return built;
        }

        CheckHeader();
        for (usize index = 0; index < document_.graphs.Size(); ++index) {
            if (auto checked = CheckGraph(document_.graphs[index]); !checked) {
                return checked;
            }
        }

        if (auto checked = CheckIds(); !checked) {
            return checked;
        }

        CheckSubgraphs();
        if (layout_ != nullptr) {
            if (auto checked = CheckLayout(); !checked) {
                return checked;
            }
        }

        return reporter_.Finish();
    }

private:
    void Report(GraphInvariant rule, GraphSubject subject, const Guid& graph, const Guid& node,
                const Guid& related, Name pin, usize index) noexcept {
        reporter_.Add(rule, subject, graph, node, related, pin, index);
    }

    // GI11(도메인)·GI12(형식 버전·주 그래프·역할 번호)·그래프 순서(GI5).
    void CheckHeader() noexcept {
        if (document_.documentFormatVersion == 0 ||
            document_.documentFormatVersion > GraphDocumentFormatVersion) {
            Report(GraphInvariant::DocumentHeader, GraphSubject::Document, ValidationNoId,
                   ValidationNoId, ValidationNoId, Name(), 0);
        }

        if (ValidationIsNoneName(document_.domain)) {
            Report(GraphInvariant::EmptyIdentity, GraphSubject::Document, ValidationNoId,
                   ValidationNoId, ValidationNoId, document_.domain, 0);
        }

        bool mainFound = false;
        for (usize index = 0; index < document_.graphs.Size(); ++index) {
            const Graph& graph = document_.graphs[index];
            if (ValidationIsZero(graph.id)) {
                Report(GraphInvariant::EmptyIdentity, GraphSubject::Graph, graph.id, ValidationNoId,
                       ValidationNoId, Name(), index);
            }

            if (index > 0 && CompareGuid(document_.graphs[index - 1].id, graph.id) > 0) {
                Report(GraphInvariant::CanonicalOrder, GraphSubject::Graph, graph.id,
                       ValidationNoId, document_.graphs[index - 1].id, Name(), index);
            }

            const bool roleKnown = graph.role == GraphRole::Main ||
                                   graph.role == GraphRole::Collapsed ||
                                   graph.role == GraphRole::DomainDefined;
            const bool isMain = !ValidationIsZero(graph.id) && graph.id == document_.mainGraph;
            if (isMain) {
                mainFound = true;
            }

            if (!roleKnown || (isMain != (graph.role == GraphRole::Main)) ||
                (graph.role != GraphRole::DomainDefined && graph.domainRole != 0)) {
                Report(GraphInvariant::DocumentHeader, GraphSubject::Graph, graph.id,
                       ValidationNoId, document_.mainGraph, Name(), index);
            }
        }

        if (!mainFound) {
            Report(GraphInvariant::DocumentHeader, GraphSubject::Document, ValidationNoId,
                   ValidationNoId, document_.mainGraph, Name(), 0);
        }
    }

    // GI15·GI16: 하위 그래프 인터페이스·경계 노드와 부모 Graph.Collapsed의 1:1, 가리킴
    // 순환(10.1절).
    void CheckSubgraphs() noexcept {
        const Span<const Graph> graphs = document_.graphs.View();
        for (usize index = 0; index < graphs.Size(); ++index) {
            const Graph& graph = graphs[index];
            const bool collapsed = graph.role == GraphRole::Collapsed;
            if (!collapsed && (!ValidationIsZero(graph.ownerNode) || !graph.inputs.IsEmpty() ||
                               !graph.outputs.IsEmpty())) {
                Report(GraphInvariant::BoundaryInterface, GraphSubject::Graph, graph.id,
                       ValidationNoId, graph.ownerNode, Name(), index);
            }

            if (collapsed) {
                CheckBoundaryList(graph, index);
            }

            usize inputs = 0;
            usize outputs = 0;
            for (usize position = 0; position < graph.nodes.Size(); ++position) {
                const GraphNode& node = graph.nodes[position];
                const bool input = registry_.IsGraphInput(node.type);
                const bool output = registry_.IsGraphOutput(node.type);
                inputs += input ? 1 : 0;
                outputs += output ? 1 : 0;
                if (!collapsed && (input || output)) {
                    Report(GraphInvariant::BoundaryInterface, GraphSubject::Node, graph.id, node.id,
                           ValidationNoId, node.type, position);
                }

                if (registry_.IsCollapsed(node.type)) {
                    CheckCollapsedNode(graph, node, position);
                }
            }

            if (collapsed && (inputs != 1 || outputs != 1)) {
                Report(GraphInvariant::BoundaryInterface, GraphSubject::Graph, graph.id,
                       ValidationNoId, ValidationNoId, Name(), index);
            }

            if (collapsed) {
                CheckOwner(graph, index);
            }
        }
    }

    // 경계 목록: 키는 None이 아니고 두 목록을 합쳐 겹치지 않으며, 합계·kind·maxEdges 규칙(GI15).
    void CheckBoundaryList(const Graph& graph, usize graphIndex) noexcept {
        const usize total = graph.inputs.Size() + graph.outputs.Size();
        if (total > MaxPinsPerNode) {
            Report(GraphInvariant::BoundaryInterface, GraphSubject::Graph, graph.id, ValidationNoId,
                   ValidationNoId, Name(), graphIndex);
            return;
        }

        const auto at = [&graph](usize position) noexcept -> const GraphBoundaryPin& {
            return position < graph.inputs.Size() ? graph.inputs[position]
                                                  : graph.outputs[position - graph.inputs.Size()];
        };

        for (usize position = 0; position < total; ++position) {
            const GraphBoundaryPin& pin = at(position);
            bool valid = !ValidationIsNoneName(pin.key) && pin.type.IsValid() &&
                         pin.maxEdges >= 1 &&
                         (pin.kind == PinKind::Data || pin.kind == PinKind::Flow);
            for (usize other = 0; valid && other < position; ++other) {
                valid = !(at(other).key == pin.key);
            }

            if (!valid) {
                Report(GraphInvariant::BoundaryInterface, GraphSubject::Graph, graph.id,
                       ValidationNoId, ValidationNoId, pin.key, position);
            }
        }
    }

    // Collapsed 그래프의 ownerNode가 다른 그래프의 Graph.Collapsed이고, 부모를 따라 올라가도 자기로
    // 돌아오지 않는다(GI16).
    void CheckOwner(const Graph& graph, usize graphIndex) noexcept {
        usize parent = OwnerGraph(graph);
        if (parent == IndexNone || parent == graphIndex) {
            Report(GraphInvariant::CollapsedOwner, GraphSubject::Graph, graph.id, ValidationNoId,
                   graph.ownerNode, Name(), graphIndex);
            return;
        }

        // 그래프 수만큼 올라가면 끝나야 한다(나무의 깊이 ≤ 그래프 수).
        const Span<const Graph> graphs = document_.graphs.View();
        for (usize step = 0; step < graphs.Size(); ++step) {
            if (parent == graphIndex) {
                Report(GraphInvariant::CollapsedOwner, GraphSubject::Graph, graph.id,
                       ValidationNoId, graph.ownerNode, Name(), graphIndex);
                return;
            }

            if (graphs[parent].role != GraphRole::Collapsed) {
                return;
            }

            parent = OwnerGraph(graphs[parent]);
            if (parent == IndexNone) {
                return;
            }
        }

        Report(GraphInvariant::CollapsedOwner, GraphSubject::Graph, graph.id, ValidationNoId,
               graph.ownerNode, Name(), graphIndex);
    }

    // Graph.Collapsed 노드마다 그것을 ownerNode로 갖는 Collapsed 그래프가 정확히 하나다(GI16).
    void CheckCollapsedNode(const Graph& graph, const GraphNode& node, usize position) noexcept {
        usize owned = 0;
        for (const Graph& child : document_.graphs) {
            owned += child.role == GraphRole::Collapsed && child.ownerNode == node.id ? 1 : 0;
        }

        if (owned != 1) {
            Report(GraphInvariant::CollapsedOwner, GraphSubject::Node, graph.id, node.id,
                   ValidationNoId, node.type, position);
        }
    }

    // ownerNode가 Graph.Collapsed로 있는 그래프의 색인이다(없거나 종류가 다르면 IndexNone).
    usize OwnerGraph(const Graph& graph) const noexcept {
        if (ValidationIsZero(graph.ownerNode)) {
            return IndexNone;
        }

        const Span<const Graph> graphs = document_.graphs.View();
        for (usize index = 0; index < graphs.Size(); ++index) {
            for (const GraphNode& node : graphs[index].nodes) {
                if (node.id == graph.ownerNode) {
                    return registry_.IsCollapsed(node.type) ? index : IndexNone;
                }
            }
        }

        return IndexNone;
    }

    uint32 FindInfo(const Guid& id) const noexcept {
        return ValidationFindKey(keys_.View(), id);
    }

    const PinDescriptor* FindPin(const ValidationNodeInfo& info, Name key) const noexcept {
        for (uint32 index = 0; index < info.pinCount; ++index) {
            const PinDescriptor& pin = pinPool_[info.pinBegin + index];
            if (pin.key == key) {
                return &pin;
            }
        }

        return nullptr;
    }

    ValidationEdgeEnd ResolveEnd(const ValidationNodeInfo& info, Name key) const noexcept {
        ValidationEdgeEnd end;
        if (info.pinsKnown) {
            if (const PinDescriptor* pin = FindPin(info, key); pin != nullptr) {
                end.kind = ValidationEnd::Current;
                end.pin = pin;
                return end;
            }
        }

        const Array<RetainedPin>& retained = info.node->retainedPins;
        for (usize index = 0; index < retained.Size(); ++index) {
            if (retained[index].key == key) {
                end.kind = ValidationEnd::Retained;
                end.retained = static_cast<uint32>(index);
                return end;
            }
        }

        end.kind = info.pinsKnown ? ValidationEnd::Missing : ValidationEnd::Unknown;
        return end;
    }

    // 노드 정보를 만들고 노드 단위 검사(GI5 노드 순서·GI11·GI13·GI14·GI5 입력 기본값·고아 핀
    // 순서·GI8)를 한다.
    Result<void> CollectNodes(const Graph& graph) noexcept {
        infos_.Clear();
        keys_.Clear();
        pinPool_.Clear();
        retainedUsed_.Clear();
        if (auto reserved = infos_.TryReserve(graph.nodes.Size()); !reserved) {
            return reserved;
        }

        if (auto reserved = keys_.TryReserve(graph.nodes.Size()); !reserved) {
            return reserved;
        }

        for (usize index = 0; index < graph.nodes.Size(); ++index) {
            const GraphNode& node = graph.nodes[index];
            ValidationNodeInfo info;
            info.node = &node;
            info.retainedBegin = static_cast<uint32>(retainedUsed_.Size());
            if (auto grown =
                    retainedUsed_.TryResize(retainedUsed_.Size() + node.retainedPins.Size());
                !grown) {
                return grown;
            }

            for (usize flag = info.retainedBegin; flag < retainedUsed_.Size(); ++flag) {
                retainedUsed_[flag] = 0;
            }

            if (ValidationIsZero(node.id)) {
                Report(GraphInvariant::EmptyIdentity, GraphSubject::Node, graph.id, node.id,
                       ValidationNoId, Name(), index);
            }

            if (index > 0 && CompareGuid(graph.nodes[index - 1].id, node.id) > 0) {
                Report(GraphInvariant::CanonicalOrder, GraphSubject::Node, graph.id, node.id,
                       graph.nodes[index - 1].id, Name(), index);
            }

            if (node.enableState != NodeEnableState::Enabled &&
                node.enableState != NodeEnableState::Disabled) {
                Report(GraphInvariant::NodeTypeData, GraphSubject::Node, graph.id, node.id,
                       ValidationNoId, Name(), index);
            }

            if (ValidationIsNoneName(node.type)) {
                Report(GraphInvariant::EmptyIdentity, GraphSubject::Node, graph.id, node.id,
                       ValidationNoId, node.type, index);
            } else {
                info.type = registry_.FindNodeType(document_.domain, node.type);
                info.reroute = registry_.IsReroute(node.type);
            }

            if (info.type != nullptr) {
                if (auto checked = CheckKnownNode(graph, node, index, info); !checked) {
                    return checked;
                }
            }

            CheckNodePinLists(graph, node, info);
            const uint32 infoIndex = static_cast<uint32>(infos_.Size());
            if (auto pushed = infos_.TryPushBack(info); !pushed) {
                return pushed;
            }

            ValidationNodeKey key;
            key.id = node.id;
            key.info = infoIndex;
            if (auto pushed = keys_.TryPushBack(key); !pushed) {
                return pushed;
            }
        }

        algo::Sort(keys_.View(),
                   [](const ValidationNodeKey& left, const ValidationNodeKey& right) noexcept {
                       return CompareGuid(left.id, right.id) < 0;
                   });

        return Result<void>::Success();
    }

    // 알려진 종류 노드의 GI13과 핀 목록 계산이다.
    Result<void> CheckKnownNode(const Graph& graph, const GraphNode& node, usize index,
                                ValidationNodeInfo& info) noexcept {
        const NodeTypeDescriptor& descriptor = info.type->descriptor;
        bool dataValid =
            node.typeSchemaVersion != 0 && node.typeSchemaVersion <= descriptor.schemaVersion;
        if (dataValid && node.typeSchemaVersion < descriptor.schemaVersion) {
            // migration 전 노드: 속성·활성·핀은 옛 서술 기준이라 알 수 없는 종류처럼
            // 검사한다(10.1절).
            return Result<void>::Success();
        }

        const Span<const uint8> properties = node.properties.bytes.View();
        if (ValidationIsZero(descriptor.propertiesStruct)) {
            dataValid = dataValid && properties.Size() == 0;
        } else {
            auto header = ReadRecordHeader(properties);
            dataValid = dataValid && header && header.Value().schema == descriptor.propertiesStruct;
        }

        if (node.enableState == NodeEnableState::Disabled &&
            !HasCapability(descriptor.capabilities, NodeCapabilities::AllowDisable)) {
            dataValid = false;
        }

        // 재경유는 고아 핀을 두지 않는다(핀이 고정이고, 접으면 사라진다 — 6.1절).
        if (info.reroute && !node.retainedPins.IsEmpty()) {
            dataValid = false;
        }

        if (!dataValid) {
            // 속성이 틀린 노드는 동적 핀을 유도하지 않고 핀 목록 모름으로 둔다(알 수 없는 종류처럼
            // 검사).
            Report(GraphInvariant::NodeTypeData, GraphSubject::Node, graph.id, node.id,
                   ValidationNoId, node.type, index);
            return Result<void>::Success();
        }

        auto collected = registry_.CollectPins(document_, graph, node, pins_);
        if (!collected) {
            if (collected.Error() != ErrorCode::OutOfMemory) {
                // 동적 핀을 유도할 수 없다: 서술 데이터 위반으로 보고하고 핀 목록 모름으로 둔다.
                Report(GraphInvariant::NodeTypeData, GraphSubject::Node, graph.id, node.id,
                       ValidationNoId, node.type, index);
                return Result<void>::Success();
            }

            return collected;
        }

        info.pinBegin = static_cast<uint32>(pinPool_.Size());
        info.pinCount = static_cast<uint32>(pins_.Size());
        if (auto appended = pinPool_.TryAppend(pins_.View()); !appended) {
            return appended;
        }

        info.pinsKnown = true;
        return Result<void>::Success();
    }

    // 입력 기본값·고아 핀 목록의 GI11·GI5·GI14·GI8·GI13(방향 값)이다.
    void CheckNodePinLists(const Graph& graph, const GraphNode& node,
                           const ValidationNodeInfo& info) noexcept {
        for (usize entryIndex = 0; entryIndex < node.inputDefaults.Size(); ++entryIndex) {
            const PinValueEntry& entry = node.inputDefaults[entryIndex];
            if (ValidationIsNoneName(entry.pin)) {
                Report(GraphInvariant::EmptyIdentity, GraphSubject::InputDefault, graph.id, node.id,
                       ValidationNoId, entry.pin, entryIndex);
                continue;
            }

            if (entryIndex > 0 &&
                ranks_.Rank(node.inputDefaults[entryIndex - 1].pin) >= ranks_.Rank(entry.pin)) {
                Report(GraphInvariant::CanonicalOrder, GraphSubject::InputDefault, graph.id,
                       node.id, ValidationNoId, entry.pin, entryIndex);
            }

            if (info.pinsKnown) {
                const PinDescriptor* pin = FindPin(info, entry.pin);
                if (pin == nullptr || pin->direction != PinDirection::Input ||
                    pin->valuePolicy == PinValuePolicy::Hidden || entry.value.bytes.IsEmpty()) {
                    Report(GraphInvariant::InputDefaultPin, GraphSubject::InputDefault, graph.id,
                           node.id, ValidationNoId, entry.pin, entryIndex);
                }
            }
        }

        for (usize retainedIndex = 0; retainedIndex < node.retainedPins.Size(); ++retainedIndex) {
            const RetainedPin& retained = node.retainedPins[retainedIndex];
            if (ValidationIsNoneName(retained.key)) {
                Report(GraphInvariant::EmptyIdentity, GraphSubject::RetainedPin, graph.id, node.id,
                       ValidationNoId, retained.key, retainedIndex);
                continue;
            }

            if (retainedIndex > 0 && ranks_.Rank(node.retainedPins[retainedIndex - 1].key) >=
                                         ranks_.Rank(retained.key)) {
                Report(GraphInvariant::CanonicalOrder, GraphSubject::RetainedPin, graph.id, node.id,
                       ValidationNoId, retained.key, retainedIndex);
            }

            if (retained.direction != PinDirection::Input &&
                retained.direction != PinDirection::Output) {
                Report(GraphInvariant::NodeTypeData, GraphSubject::RetainedPin, graph.id, node.id,
                       ValidationNoId, retained.key, retainedIndex);
            }

            if (info.pinsKnown && FindPin(info, retained.key) != nullptr) {
                Report(GraphInvariant::RetainedPinShadowed, GraphSubject::RetainedPin, graph.id,
                       node.id, ValidationNoId, retained.key, retainedIndex);
            }
        }
    }

    // 간선 단위 검사(GI11·GI4·GI5 순서·GI1·GI2)와 GI3·GI5 순번·GI7·GI10 자료 수집이다.
    Result<void> CheckEdges(const Graph& graph) noexcept {
        inputUses_.Clear();
        outputUses_.Clear();
        for (usize index = 0; index < graph.edges.Size(); ++index) {
            const GraphEdge& edge = graph.edges[index];
            if (index > 0) {
                const int32 compared = ValidationCompareEdges(graph.edges[index - 1], edge, ranks_);
                if (compared == 0) {
                    Report(GraphInvariant::DuplicateEdge, GraphSubject::Edge, graph.id, edge.toNode,
                           edge.fromNode, edge.toPin, index);
                } else if (compared > 0) {
                    Report(GraphInvariant::CanonicalOrder, GraphSubject::Edge, graph.id,
                           edge.toNode, edge.fromNode, edge.toPin, index);
                }
            }

            if (ValidationIsZero(edge.fromNode) || ValidationIsZero(edge.toNode) ||
                ValidationIsNoneName(edge.fromPin) || ValidationIsNoneName(edge.toPin)) {
                Report(GraphInvariant::EmptyIdentity, GraphSubject::Edge, graph.id, edge.toNode,
                       edge.fromNode, ValidationIsNoneName(edge.toPin) ? edge.toPin : edge.fromPin,
                       index);
                continue;
            }

            const uint32 toInfo = FindInfo(edge.toNode);
            const uint32 fromInfo = FindInfo(edge.fromNode);
            if (toInfo == ValidationNoInfo || fromInfo == ValidationNoInfo) {
                Report(GraphInvariant::EdgeNodesInGraph, GraphSubject::Edge, graph.id, edge.toNode,
                       edge.fromNode, toInfo == ValidationNoInfo ? edge.toPin : edge.fromPin,
                       index);
                continue;
            }

            if (auto collected = CollectEdgeEnds(graph, edge, index, fromInfo, toInfo);
                !collected) {
                return collected;
            }
        }

        return Result<void>::Success();
    }

    Result<void> CollectEdgeEnds(const Graph& graph, const GraphEdge& edge, usize index,
                                 uint32 fromInfo, uint32 toInfo) noexcept {
        ValidationNodeInfo& from = infos_[fromInfo];
        ValidationNodeInfo& to = infos_[toInfo];
        const ValidationEdgeEnd fromEnd = ResolveEnd(from, edge.fromPin);
        const ValidationEdgeEnd toEnd = ResolveEnd(to, edge.toPin);
        if (fromEnd.kind == ValidationEnd::Retained) {
            retainedUsed_[from.retainedBegin + fromEnd.retained] = 1;
        }

        if (toEnd.kind == ValidationEnd::Retained) {
            retainedUsed_[to.retainedBegin + toEnd.retained] = 1;
        }

        // GI2: 알려진 종류의 끝만 본다. 고아 핀은 방향만 본다(6.3절 "알 수 없는 종류").
        const bool fromBad =
            fromEnd.kind == ValidationEnd::Missing ||
            (fromEnd.kind == ValidationEnd::Current &&
             fromEnd.pin->direction != PinDirection::Output) ||
            (fromEnd.kind == ValidationEnd::Retained && from.pinsKnown &&
             from.node->retainedPins[fromEnd.retained].direction != PinDirection::Output);
        const bool toBad =
            toEnd.kind == ValidationEnd::Missing ||
            (toEnd.kind == ValidationEnd::Current && toEnd.pin->direction != PinDirection::Input) ||
            (toEnd.kind == ValidationEnd::Retained && to.pinsKnown &&
             to.node->retainedPins[toEnd.retained].direction != PinDirection::Input);
        if (fromBad) {
            Report(GraphInvariant::EdgePinExists, GraphSubject::Edge, graph.id, edge.toNode,
                   edge.fromNode, edge.fromPin, index);
        }

        if (toBad) {
            Report(GraphInvariant::EdgePinExists, GraphSubject::Edge, graph.id, edge.toNode,
                   edge.fromNode, edge.toPin, index);
        }

        if (toEnd.kind == ValidationEnd::Current && !toBad) {
            ValidationInputUse use;
            use.info = toInfo;
            use.pinRank = ranks_.Rank(edge.toPin);
            use.edge = static_cast<uint32>(index);
            use.maxEdges = toEnd.pin->maxEdges;
            if (auto pushed = inputUses_.TryPushBack(use); !pushed) {
                return pushed;
            }

            if (to.reroute && to.upstream == ValidationNoInfo) {
                to.upstream = fromInfo;
            }
        }

        if (fromEnd.kind == ValidationEnd::Current && !fromBad) {
            ValidationOutputUse use;
            use.info = fromInfo;
            use.pinRank = ranks_.Rank(edge.fromPin);
            use.order = edge.order;
            use.edge = static_cast<uint32>(index);
            use.ordered = fromEnd.pin->ordered;
            if (auto pushed = outputUses_.TryPushBack(use); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    }

    // GI3: 입력 핀마다 간선 수 ≤ maxEdges. 넘는 첫 간선을 보고한다.
    void CheckInputLimits(const Graph& graph) noexcept {
        algo::Sort(inputUses_.View(),
                   [](const ValidationInputUse& left, const ValidationInputUse& right) noexcept {
                       if (left.info != right.info) {
                           return left.info < right.info;
                       }

                       if (left.pinRank != right.pinRank) {
                           return left.pinRank < right.pinRank;
                       }

                       return left.edge < right.edge;
                   });

        usize runStart = 0;
        for (usize index = 0; index < inputUses_.Size(); ++index) {
            if (index > 0 && (inputUses_[index].info != inputUses_[index - 1].info ||
                              inputUses_[index].pinRank != inputUses_[index - 1].pinRank)) {
                runStart = index;
            }

            const ValidationInputUse& use = inputUses_[index];
            if (use.maxEdges != UnlimitedEdges && index - runStart == use.maxEdges) {
                const GraphEdge& edge = graph.edges[use.edge];
                Report(GraphInvariant::InputEdgeLimit, GraphSubject::Edge, graph.id, edge.toNode,
                       edge.fromNode, edge.toPin, use.edge);
            }
        }
    }

    // GI5 순번: ordered 출력 핀은 0부터 빈틈없고, 그 밖은 0이다.
    void CheckOutputOrders(const Graph& graph) noexcept {
        algo::Sort(outputUses_.View(),
                   [](const ValidationOutputUse& left, const ValidationOutputUse& right) noexcept {
                       if (left.info != right.info) {
                           return left.info < right.info;
                       }

                       if (left.pinRank != right.pinRank) {
                           return left.pinRank < right.pinRank;
                       }

                       if (left.order != right.order) {
                           return left.order < right.order;
                       }

                       return left.edge < right.edge;
                   });

        usize runStart = 0;
        bool runReported = false;
        for (usize index = 0; index < outputUses_.Size(); ++index) {
            if (index > 0 && (outputUses_[index].info != outputUses_[index - 1].info ||
                              outputUses_[index].pinRank != outputUses_[index - 1].pinRank)) {
                runStart = index;
                runReported = false;
            }

            const ValidationOutputUse& use = outputUses_[index];
            const uint32 expected = use.ordered ? static_cast<uint32>(index - runStart) : 0;
            if (use.order != expected && !runReported) {
                const GraphEdge& edge = graph.edges[use.edge];
                Report(GraphInvariant::CanonicalOrder, GraphSubject::Edge, graph.id, edge.toNode,
                       edge.fromNode, edge.fromPin, use.edge);
                runReported = use.ordered;
            }
        }
    }

    // GI7: 고아 핀에 간선이나 사용자 값이 있다.
    void CheckRetainedUse(const Graph& graph) noexcept {
        for (usize infoIndex = 0; infoIndex < infos_.Size(); ++infoIndex) {
            const ValidationNodeInfo& info = infos_[infoIndex];
            for (usize retained = 0; retained < info.node->retainedPins.Size(); ++retained) {
                const RetainedPin& pin = info.node->retainedPins[retained];
                if (retainedUsed_[info.retainedBegin + retained] == 0 &&
                    pin.value.bytes.IsEmpty()) {
                    Report(GraphInvariant::RetainedPinUnused, GraphSubject::RetainedPin, graph.id,
                           info.node->id, ValidationNoId, pin.key, retained);
                }
            }
        }
    }

    // GI10: 재경유 입력을 거슬러 올라가는 사슬에 순환이 없다. 순환마다 한 번 보고한다.
    Result<void> CheckRerouteCycles(const Graph& graph) noexcept {
        for (usize start = 0; start < infos_.Size(); ++start) {
            if (!infos_[start].reroute || infos_[start].walk != 0) {
                continue;
            }

            path_.Clear();
            uint32 current = static_cast<uint32>(start);
            while (current != ValidationNoInfo && infos_[current].reroute &&
                   infos_[current].walk == 0) {
                infos_[current].walk = 1;
                if (auto pushed = path_.TryPushBack(current); !pushed) {
                    return pushed;
                }

                current = infos_[current].upstream;
            }

            if (current != ValidationNoInfo && infos_[current].reroute &&
                infos_[current].walk == 1) {
                Report(GraphInvariant::RerouteCycle, GraphSubject::Node, graph.id,
                       infos_[current].node->id, ValidationNoId, Name(),
                       static_cast<usize>(infos_[current].node - graph.nodes.Data()));
            }

            for (uint32 visited : path_) {
                infos_[visited].walk = 2;
            }
        }

        return Result<void>::Success();
    }

    Result<void> CheckGraph(const Graph& graph) noexcept {
        if (auto collected = CollectNodes(graph); !collected) {
            return collected;
        }

        if (auto checked = CheckEdges(graph); !checked) {
            return checked;
        }

        CheckInputLimits(graph);
        CheckOutputOrders(graph);
        CheckRetainedUse(graph);
        return CheckRerouteCycles(graph);
    }

    // GI9: 문서 안 GraphId·NodeId·주석 id가 겹치지 않는다(0은 GI11이 보고하므로 뺀다).
    Result<void> CheckIds() noexcept {
        Array<ValidationIdUse> ids(scratch_);
        auto add = [&ids](const Guid& id, const Guid& graph, GraphSubject subject,
                          usize index) noexcept -> Result<void> {
            if (ValidationIsZero(id)) {
                return Result<void>::Success();
            }

            ValidationIdUse use;
            use.id = id;
            use.graph = graph;
            use.subject = subject;
            use.index = static_cast<uint32>(index);
            return ids.TryPushBack(use);
        };

        for (usize graphIndex = 0; graphIndex < document_.graphs.Size(); ++graphIndex) {
            const Graph& graph = document_.graphs[graphIndex];
            if (auto added = add(graph.id, graph.id, GraphSubject::Graph, graphIndex); !added) {
                return added;
            }

            for (usize index = 0; index < graph.nodes.Size(); ++index) {
                if (auto added = add(graph.nodes[index].id, graph.id, GraphSubject::Node, index);
                    !added) {
                    return added;
                }
            }
        }

        if (layout_ != nullptr) {
            for (const GraphLayout& graph : layout_->graphs) {
                for (usize index = 0; index < graph.comments.Size(); ++index) {
                    if (auto added = add(graph.comments[index].id, graph.graph,
                                         GraphSubject::Comment, index);
                        !added) {
                        return added;
                    }
                }
            }
        }

        // 같은 id 안에서는 (요소 종류, 그래프, 위치) 순이다. 보고하는 쪽(뒤의 것)이 정렬 안정성과
        // 무관하게 정해진다.
        algo::Sort(
            ids.View(), [](const ValidationIdUse& left, const ValidationIdUse& right) noexcept {
                if (const int32 compared = CompareGuid(left.id, right.id); compared != 0) {
                    return compared < 0;
                }

                if (left.subject != right.subject) {
                    return left.subject < right.subject;
                }

                if (const int32 compared = CompareGuid(left.graph, right.graph); compared != 0) {
                    return compared < 0;
                }

                return left.index < right.index;
            });

        for (usize index = 1; index < ids.Size(); ++index) {
            if (ids[index].id == ids[index - 1].id) {
                Report(GraphInvariant::DuplicateId, ids[index].subject, ids[index].graph,
                       ids[index].id, ids[index - 1].graph, Name(), ids[index].index);
            }
        }

        return Result<void>::Success();
    }

    // GI6(배치 1:1)·GI5(배치 순서)·GI11(주석 id).
    Result<void> CheckLayout() noexcept {
        const Array<GraphLayout>& graphs = layout_->graphs;
        Array<uint8> matched(scratch_);
        if (auto grown = matched.TryResize(document_.graphs.Size()); !grown) {
            return grown;
        }

        for (usize index = 0; index < matched.Size(); ++index) {
            matched[index] = 0;
        }

        for (usize index = 0; index < graphs.Size(); ++index) {
            const GraphLayout& graph = graphs[index];
            if (index > 0) {
                const int32 compared = CompareGuid(graphs[index - 1].graph, graph.graph);
                if (compared == 0) {
                    Report(GraphInvariant::LayoutMatchesDocument, GraphSubject::LayoutGraph,
                           graph.graph, ValidationNoId, ValidationNoId, Name(), index);
                } else if (compared > 0) {
                    Report(GraphInvariant::CanonicalOrder, GraphSubject::LayoutGraph, graph.graph,
                           ValidationNoId, graphs[index - 1].graph, Name(), index);
                }
            }

            usize documentIndex = IndexNone;
            for (usize candidate = 0; candidate < document_.graphs.Size(); ++candidate) {
                if (document_.graphs[candidate].id == graph.graph) {
                    documentIndex = candidate;
                    break;
                }
            }

            if (documentIndex == IndexNone) {
                Report(GraphInvariant::LayoutMatchesDocument, GraphSubject::LayoutGraph,
                       graph.graph, ValidationNoId, ValidationNoId, Name(), index);
            } else if (matched[documentIndex] == 0) {
                matched[documentIndex] = 1;
                if (auto checked = CheckLayoutGraph(document_.graphs[documentIndex], graph);
                    !checked) {
                    return checked;
                }
            }

            CheckComments(graph);
            CheckBookmarks(graph);
        }

        for (usize index = 0; index < matched.Size(); ++index) {
            if (matched[index] == 0) {
                Report(GraphInvariant::LayoutMatchesDocument, GraphSubject::LayoutGraph,
                       document_.graphs[index].id, ValidationNoId, ValidationNoId, Name(), index);
            }
        }

        return Result<void>::Success();
    }

    Result<void> CheckLayoutGraph(const Graph& graph, const GraphLayout& layout) noexcept {
        Array<uint8> matched(scratch_);
        if (auto grown = matched.TryResize(graph.nodes.Size()); !grown) {
            return grown;
        }

        for (usize index = 0; index < matched.Size(); ++index) {
            matched[index] = 0;
        }

        // 배치 노드를 문서 노드에 맞춘다. 문서 노드 색인은 keys_를 다시 만들지 않고 이진 탐색용
        // 사본으로 찾는다.
        Array<ValidationNodeKey> keys(scratch_);
        if (auto reserved = keys.TryReserve(graph.nodes.Size()); !reserved) {
            return reserved;
        }

        for (usize index = 0; index < graph.nodes.Size(); ++index) {
            ValidationNodeKey key;
            key.id = graph.nodes[index].id;
            key.info = static_cast<uint32>(index);
            if (auto pushed = keys.TryPushBack(key); !pushed) {
                return pushed;
            }
        }

        algo::Sort(keys.View(),
                   [](const ValidationNodeKey& left, const ValidationNodeKey& right) noexcept {
                       return CompareGuid(left.id, right.id) < 0;
                   });

        for (usize index = 0; index < layout.nodes.Size(); ++index) {
            const NodeLayout& node = layout.nodes[index];
            if (index > 0) {
                const int32 compared = CompareGuid(layout.nodes[index - 1].node, node.node);
                if (compared == 0) {
                    Report(GraphInvariant::LayoutMatchesDocument, GraphSubject::LayoutNode,
                           layout.graph, node.node, ValidationNoId, Name(), index);
                    continue;
                }

                if (compared > 0) {
                    Report(GraphInvariant::CanonicalOrder, GraphSubject::LayoutNode, layout.graph,
                           node.node, layout.nodes[index - 1].node, Name(), index);
                }
            }

            const uint32 found = ValidationFindKey(keys.View(), node.node);
            if (found == ValidationNoInfo) {
                Report(GraphInvariant::LayoutMatchesDocument, GraphSubject::LayoutNode,
                       layout.graph, node.node, ValidationNoId, Name(), index);
            } else {
                matched[found] = 1;
            }
        }

        for (usize index = 0; index < matched.Size(); ++index) {
            if (matched[index] == 0) {
                Report(GraphInvariant::LayoutMatchesDocument, GraphSubject::LayoutNode,
                       layout.graph, graph.nodes[index].id, ValidationNoId, Name(), index);
            }
        }

        return Result<void>::Success();
    }

    void CheckComments(const GraphLayout& layout) noexcept {
        for (usize index = 0; index < layout.comments.Size(); ++index) {
            const CommentBox& comment = layout.comments[index];
            if (ValidationIsZero(comment.id)) {
                Report(GraphInvariant::EmptyIdentity, GraphSubject::Comment, layout.graph,
                       comment.id, ValidationNoId, Name(), index);
            }

            if (index > 0 && CompareGuid(layout.comments[index - 1].id, comment.id) > 0) {
                Report(GraphInvariant::CanonicalOrder, GraphSubject::Comment, layout.graph,
                       comment.id, layout.comments[index - 1].id, Name(), index);
            }
        }
    }

    void CheckBookmarks(const GraphLayout& layout) noexcept {
        for (usize index = 1; index < layout.bookmarks.Size(); ++index) {
            if (layout.bookmarks[index - 1].number >= layout.bookmarks[index].number) {
                Report(GraphInvariant::CanonicalOrder, GraphSubject::Bookmark, layout.graph,
                       ValidationNoId, ValidationNoId, Name(), index);
            }
        }
    }

    const GraphDocument& document_;
    const GraphDocumentLayout* layout_;
    const NodeTypeRegistry& registry_;
    Allocator& scratch_;
    ValidationReporter reporter_;
    GraphNameRanks ranks_;
    Array<ValidationNodeInfo> infos_;
    Array<ValidationNodeKey> keys_;
    Array<PinDescriptor> pinPool_;
    Array<uint8> retainedUsed_;
    Array<ValidationInputUse> inputUses_;
    Array<ValidationOutputUse> outputUses_;
    Array<uint32> path_;
    PinList pins_;
};

// 정규화의 비교기들이다(순위표를 쓴다).
struct CanonicalEdgeLess {
    const GraphNameRanks* ranks;

    bool operator()(const GraphEdge& left, const GraphEdge& right) const noexcept {
        const int32 compared = ValidationCompareEdges(left, right, *ranks);
        if (compared != 0) {
            return compared < 0;
        }

        return left.order < right.order;
    }
};

} // namespace

Result<void> CanonicalizeGraphDocument(GraphDocument& document, GraphDocumentLayout* layout,
                                       NamePool& names, Allocator& scratch) noexcept {
    GraphNameRanks ranks(scratch, names);
    if (auto added = ranks.AddDocument(document); !added) {
        return added;
    }

    if (auto built = ranks.Build(); !built) {
        return built;
    }

    algo::Sort(document.graphs.View(), [](const Graph& left, const Graph& right) noexcept {
        return CompareGuid(left.id, right.id) < 0;
    });

    for (Graph& graph : document.graphs) {
        algo::Sort(graph.nodes.View(), [](const GraphNode& left, const GraphNode& right) noexcept {
            return CompareGuid(left.id, right.id) < 0;
        });

        for (GraphNode& node : graph.nodes) {
            algo::Sort(node.inputDefaults.View(),
                       [&ranks](const PinValueEntry& left, const PinValueEntry& right) noexcept {
                           return ranks.Rank(left.pin) < ranks.Rank(right.pin);
                       });
            algo::Sort(node.retainedPins.View(),
                       [&ranks](const RetainedPin& left, const RetainedPin& right) noexcept {
                           return ranks.Rank(left.key) < ranks.Rank(right.key);
                       });
        }

        algo::Sort(graph.edges.View(), CanonicalEdgeLess{&ranks});
    }

    if (layout == nullptr) {
        return Result<void>::Success();
    }

    algo::Sort(layout->graphs.View(),
               [](const GraphLayout& left, const GraphLayout& right) noexcept {
                   return CompareGuid(left.graph, right.graph) < 0;
               });

    for (GraphLayout& graph : layout->graphs) {
        algo::Sort(graph.nodes.View(),
                   [](const NodeLayout& left, const NodeLayout& right) noexcept {
                       return CompareGuid(left.node, right.node) < 0;
                   });
        algo::Sort(graph.comments.View(),
                   [](const CommentBox& left, const CommentBox& right) noexcept {
                       return CompareGuid(left.id, right.id) < 0;
                   });
        algo::Sort(graph.bookmarks.View(),
                   [](const Bookmark& left, const Bookmark& right) noexcept {
                       return left.number < right.number;
                   });
    }

    return Result<void>::Success();
}

Result<void> ValidateGraphDocument(const GraphDocument& document, const GraphDocumentLayout* layout,
                                   const NodeTypeRegistry& registry, Allocator& scratch,
                                   GraphViolationList& out) noexcept {
    out.Clear();
    DocumentValidator validator(document, layout, registry, scratch, out);
    return validator.Run();
}

} // namespace rocks::nodegraph
