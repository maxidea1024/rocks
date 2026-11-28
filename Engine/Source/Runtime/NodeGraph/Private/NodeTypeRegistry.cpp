#include "GraphMutatorCore.h"

#include <Core/Hash/TypeHash.h>
#include <Core/Text/NamePool.h>
#include <NodeGraph/GraphSemantic.h>
#include <NodeGraph/NodeTypeRegistry.h>

// schema·노드 종류 등록표, 핀 목록, schema 기본 정규화다(Docs/Design/NodeGraph 6.2·7·10절).
namespace rocks::nodegraph {

namespace {

// 핀 서술 하나가 등록 규칙을 지키는가(정적·동적 핀 공통 — 10절): 키가 None이 아님, maxEdges ≥ 1,
// 방향·종류· 값 정책이 정의된 값, ordered는 출력 핀만, 타입은 UnresolvedPinType이거나 schema 표
// 색인.
bool RegistryPinValid(const PinDescriptor& pin, uint32 typeCount) noexcept {
    const bool directionKnown =
        pin.direction == PinDirection::Input || pin.direction == PinDirection::Output;
    const bool kindKnown =
        pin.kind == PinKind::Data || pin.kind == PinKind::Flow || pin.kind == PinKind::Structural;
    const bool policyKnown = pin.valuePolicy == PinValuePolicy::Editable ||
                             pin.valuePolicy == PinValuePolicy::ReadOnly ||
                             pin.valuePolicy == PinValuePolicy::Hidden;
    return pin.key.IsValid() && !pin.key.IsNone() && pin.maxEdges != 0 && directionKnown &&
           kindKnown && policyKnown && (!pin.ordered || pin.direction == PinDirection::Output) &&
           (pin.type == UnresolvedPinType || pin.type < typeCount);
}

uint64 RegistryNameHash(Name name) noexcept {
    return HashCombine(name.kind == NameKind::Normal ? name.id : 0, name.number);
}

uint64 RegistryTypeKey(Name domain, Name type) noexcept {
    return HashCombine(RegistryNameHash(domain), RegistryNameHash(type));
}

} // namespace

Result<void> GraphSchema::NormalizeSemanticNode(const GraphNode& node,
                                                SemanticNodeWriter& writer) const noexcept {
    return writer.WriteNode(node);
}

ConnectVerdict GraphSchema::CanConnect(const GraphReadView& view,
                                       const ConnectQuery& query) const noexcept {
    static_cast<void>(view);
    return detail::DefaultConnectVerdict(query);
}

Result<void> GraphSchema::CreateDefaultNodes(GraphRole role, Guid graph, GraphIdSource& ids,
                                             GraphCommandList& out) const noexcept {
    static_cast<void>(role);
    static_cast<void>(graph);
    static_cast<void>(ids);
    static_cast<void>(out);
    return Result<void>::Success();
}

Result<void> GraphSchema::Validate(const GraphReadView& view,
                                   GraphDiagnosticList& out) const noexcept {
    static_cast<void>(view);
    static_cast<void>(out);
    return Result<void>::Success();
}

PasteDecision GraphSchema::CanPasteNode(const GraphReadView& view,
                                        const GraphNode& node) const noexcept {
    static_cast<void>(view);
    static_cast<void>(node);
    return PasteDecision::Allow;
}

bool GraphSchema::AllowExternalEdgeReconnect() const noexcept {
    return false;
}

Result<void> PinList::Add(const PinDescriptor& pin) noexcept {
    if (!pin.key.IsValid() || pin.key.IsNone()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (Find(pin.key) != nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (pins_.Size() >= MaxPinsPerNode) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    return pins_.TryPushBack(pin);
}

const PinDescriptor* PinList::Find(Name key) const noexcept {
    for (const PinDescriptor& pin : pins_) {
        if (pin.key == key) {
            return &pin;
        }
    }

    return nullptr;
}

NodeTypeRegistry::NodeTypeRegistry(Allocator& allocator, NamePool& names) noexcept
    : allocator_(allocator), names_(names), schemas_(allocator), nodeTypes_(allocator),
      typeLookup_(allocator), listeners_(allocator) {
}

NodeTypeRegistry::~NodeTypeRegistry() noexcept = default;

Result<void> NodeTypeRegistry::Initialize() noexcept {
    if (initialized_) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    auto reroute = names_.TryIntern(RerouteNodeTypeName);
    if (!reroute) {
        return Result<void>::Failure(reroute.Error());
    }

    auto pinIn = names_.TryIntern(ReroutePinIn);
    if (!pinIn) {
        return Result<void>::Failure(pinIn.Error());
    }

    auto pinOut = names_.TryIntern(ReroutePinOut);
    if (!pinOut) {
        return Result<void>::Failure(pinOut.Error());
    }

    rerouteType_ = reroute.Value();
    reroutePinIn_ = pinIn.Value();
    reroutePinOut_ = pinOut.Value();
    // 재경유 핀은 기본값을 두지 않는다(접으면 사라지므로 — 6.1절). Hidden이면 GI14가 기본값을
    // 거부한다.
    reroutePins_[0] = InputPin(reroutePinIn_, UnresolvedPinType, ReroutePinIn);
    reroutePins_[0].valuePolicy = PinValuePolicy::Hidden;
    reroutePins_[1] = OutputPin(reroutePinOut_, UnresolvedPinType, ReroutePinOut);
    reroutePins_[1].valuePolicy = PinValuePolicy::Hidden;

    NodeTypeDescriptor descriptor;
    descriptor.type = rerouteType_;
    descriptor.schemaVersion = RerouteSchemaVersion;
    descriptor.staticPins = Span<const PinDescriptor>(reroutePins_, 2);
    descriptor.menu.displayName = "Reroute";
    descriptor.menu.category = "Organization";
    descriptor.menu.tooltip = "Reroute node: routes a wire through an extra point.";
    auto added = AddNodeType(Name(), descriptor, ModuleLease());
    if (!added) {
        rerouteType_ = Name();
        return Result<void>::Failure(added.Error());
    }

    if (auto boundary = RegisterBoundaryTypes(); !boundary) {
        nodeTypes_.Clear();
        RebuildTypeLookup();
        rerouteType_ = Name();
        return boundary;
    }

    initialized_ = true;
    return Result<void>::Success();
}

Result<void> NodeTypeRegistry::RegisterBoundaryTypes() noexcept {
    const StringView names[] = {GraphInputNodeTypeName, GraphOutputNodeTypeName,
                                CollapsedNodeTypeName};
    Name* const targets[] = {&graphInputType_, &graphOutputType_, &collapsedType_};
    for (usize index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        auto name = names_.TryIntern(names[index]);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        NodeTypeDescriptor descriptor;
        descriptor.type = name.Value();
        descriptor.schemaVersion = BoundarySchemaVersion;
        // 경계 노드는 지우기·복제·비활성이 안 되고, Collapsed는 지울 수만 있다(10.1절).
        descriptor.capabilities = targets[index] == &collapsedType_ ? NodeCapabilities::CanDelete
                                                                    : NodeCapabilities::None;
        descriptor.menu.displayName = names[index];
        descriptor.menu.category = "Organization";
        if (auto added = AddNodeType(Name(), descriptor, ModuleLease()); !added) {
            return Result<void>::Failure(added.Error());
        }

        *targets[index] = name.Value();
    }

    return Result<void>::Success();
}

Result<void> NodeTypeRegistry::CollectBoundaryPins(const GraphDocument& document,
                                                   const Graph& graph, const GraphNode& node,
                                                   PinList& out) const noexcept {
    // 경계 핀 타입 이름을 문서 도메인 schema 표 색인으로 바꾼다(없으면 UnresolvedPinType).
    const GraphSchema* schema = FindSchema(document.domain);
    const auto typeIndex = [this, schema](Name type) noexcept -> PinTypeId {
        if (schema == nullptr) {
            return UnresolvedPinType;
        }

        const Span<const PinTypeInfo> types = schema->PinTypes().types;
        for (usize index = 0; index < types.Size(); ++index) {
            auto found = names_.TryFind(types[index].name);
            if (found && found.Value() == type) {
                return static_cast<PinTypeId>(index);
            }
        }

        return UnresolvedPinType;
    };

    const auto add = [&out, &typeIndex](const GraphBoundaryPin& boundary,
                                        PinDirection direction) noexcept {
        PinDescriptor pin;
        pin.key = boundary.key;
        pin.direction = direction;
        pin.kind = boundary.kind;
        pin.type = typeIndex(boundary.type);
        pin.maxEdges = direction == PinDirection::Input ? boundary.maxEdges : UnlimitedEdges;
        return out.Add(pin);
    };

    // Graph.Input은 경계 입력마다 출력 핀, Graph.Output은 경계 출력마다 입력 핀이다.
    if (node.type == graphInputType_ || node.type == graphOutputType_) {
        const bool input = node.type == graphInputType_;
        for (const GraphBoundaryPin& boundary : input ? graph.inputs : graph.outputs) {
            if (auto added = add(boundary, input ? PinDirection::Output : PinDirection::Input);
                !added) {
                return added;
            }
        }

        return Result<void>::Success();
    }

    // Graph.Collapsed는 자기를 ownerNode로 갖는 하위 그래프의 인터페이스다. 아직 없으면(목록 중간
    // 상태) 핀이 없다 — 목록 끝 검사(GI16)가 잡는다.
    for (const Graph& child : document.graphs) {
        if (child.role != GraphRole::Collapsed || !(child.ownerNode == node.id)) {
            continue;
        }

        for (const GraphBoundaryPin& boundary : child.inputs) {
            if (auto added = add(boundary, PinDirection::Input); !added) {
                return added;
            }
        }

        for (const GraphBoundaryPin& boundary : child.outputs) {
            if (auto added = add(boundary, PinDirection::Output); !added) {
                return added;
            }
        }

        break;
    }

    return Result<void>::Success();
}

Result<SchemaRegistrationId> NodeTypeRegistry::RegisterSchema(const GraphSchema& schema,
                                                              ModuleLease lease) noexcept {
    if (!initialized_) {
        return Result<SchemaRegistrationId>::Failure(ErrorCode::Unsupported);
    }

    const Name domain = schema.Domain();
    if (!domain.IsValid() || domain.IsNone()) {
        return Result<SchemaRegistrationId>::Failure(ErrorCode::InvalidArgument);
    }

    if (FindSchema(domain) != nullptr) {
        return Result<SchemaRegistrationId>::Failure(ErrorCode::AlreadyExists);
    }

    SchemaEntry entry;
    entry.id.value = nextId_;
    entry.schema = &schema;
    entry.domain = domain;
    entry.lease = Move(lease);
    if (auto pushed = schemas_.TryPushBack(Move(entry)); !pushed) {
        return Result<SchemaRegistrationId>::Failure(pushed.Error());
    }

    SchemaRegistrationId id;
    id.value = nextId_;
    ++nextId_;
    return Result<SchemaRegistrationId>::Success(id);
}

Result<void> NodeTypeRegistry::UnregisterSchema(SchemaRegistrationId id) noexcept {
    for (usize index = 0; index < schemas_.Size(); ++index) {
        if (schemas_[index].id.value != id.value || id.value == 0) {
            continue;
        }

        for (const NodeTypeEntry& type : nodeTypes_) {
            if (type.registered.domain == schemas_[index].domain) {
                return Result<void>::Failure(ErrorCode::Busy);
            }
        }

        return schemas_.TryRemoveAt(index);
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void>
NodeTypeRegistry::ValidateDescriptor(const GraphSchema* schema,
                                     const NodeTypeDescriptor& descriptor) const noexcept {
    if (!descriptor.type.IsValid() || descriptor.type.IsNone() || descriptor.schemaVersion == 0 ||
        descriptor.staticPins.Size() > MaxPinsPerNode ||
        (static_cast<uint32>(descriptor.capabilities) & ~NodeCapabilityMask) != 0 ||
        (descriptor.schemaVersion > 1 && descriptor.migrate == nullptr)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 공통 종류는 schema 없이 등록한다(타입 표 검사를 하지 않는다 — 공통 핀은 wildcard다).
    if (schema != nullptr) {
        auto text = names_.Acquire(descriptor.type);
        if (!text) {
            return Result<void>::Failure(text.Error());
        }

        if (text.Value().View().StartsWith(CommonNodeTypePrefix)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    const uint32 typeCount = schema != nullptr ? schema->PinTypes().Count() : 0;
    for (usize index = 0; index < descriptor.staticPins.Size(); ++index) {
        const PinDescriptor& pin = descriptor.staticPins[index];
        if (!RegistryPinValid(pin, typeCount)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize other = 0; other < index; ++other) {
            if (descriptor.staticPins[other].key == pin.key) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    for (usize index = 0; index < descriptor.passThrough.Size(); ++index) {
        const PassThroughPair& pair = descriptor.passThrough[index];
        bool inputFound = false;
        bool outputFound = false;
        for (const PinDescriptor& pin : descriptor.staticPins) {
            if (pin.key == pair.input && pin.direction == PinDirection::Input) {
                inputFound = true;
            }

            if (pin.key == pair.output && pin.direction == PinDirection::Output) {
                outputFound = true;
            }
        }

        if (!inputFound || !outputFound) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize other = 0; other < index; ++other) {
            if (descriptor.passThrough[other].output == pair.output) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    return Result<void>::Success();
}

Result<NodeTypeRegistrationId> NodeTypeRegistry::AddNodeType(Name domain,
                                                             const NodeTypeDescriptor& descriptor,
                                                             ModuleLease lease) noexcept {
    NodeTypeEntry entry(allocator_);
    if (auto copied = entry.pins.TryAppend(descriptor.staticPins); !copied) {
        return Result<NodeTypeRegistrationId>::Failure(copied.Error());
    }

    if (auto copied = entry.passThrough.TryAppend(descriptor.passThrough); !copied) {
        return Result<NodeTypeRegistrationId>::Failure(copied.Error());
    }

    if (auto reserved = nodeTypes_.TryEnsureCapacity(nodeTypes_.Size() + 1); !reserved) {
        return Result<NodeTypeRegistrationId>::Failure(reserved.Error());
    }

    entry.id.value = nextId_;
    entry.registered.domain = domain;
    entry.registered.descriptor = descriptor;
    entry.registered.descriptor.staticPins = entry.pins.View();
    entry.registered.descriptor.passThrough = entry.passThrough.View();
    entry.lease = Move(lease);
    if (auto pushed = nodeTypes_.TryPushBack(Move(entry)); !pushed) {
        return Result<NodeTypeRegistrationId>::Failure(pushed.Error());
    }

    // 조회 표는 최적화다: 넣지 못하면 선형 탐색이 찾는다.
    const uint32 index = static_cast<uint32>(nodeTypes_.Size() - 1);
    AddTypeLookup(domain, descriptor.type, index);

    NodeTypeRegistrationId id;
    id.value = nextId_;
    ++nextId_;
    return Result<NodeTypeRegistrationId>::Success(id);
}

void NodeTypeRegistry::AddTypeLookup(Name domain, Name type, uint32 index) noexcept {
    // 충돌(같은 hash의 다른 종류)이나 넣기 실패가 있으면 표가 불완전하다 — 빗나간 조회를 선형
    // 탐색으로 확인한다.
    const uint64 key = RegistryTypeKey(domain, type);
    if (const uint32* existing = typeLookup_.Find(key); existing != nullptr) {
        typeLookupComplete_ = false;
        return;
    }

    if (!typeLookup_.InsertOrAssign(key, index)) {
        static_cast<void>(typeLookup_.Erase(key));
        typeLookupComplete_ = false;
    }
}

void NodeTypeRegistry::RebuildTypeLookup() noexcept {
    typeLookup_.Clear();
    typeLookupComplete_ = true;
    for (usize index = 0; index < nodeTypes_.Size(); ++index) {
        const RegisteredNodeType& entry = nodeTypes_[index].registered;
        AddTypeLookup(entry.domain, entry.descriptor.type, static_cast<uint32>(index));
    }
}

const RegisteredNodeType* NodeTypeRegistry::LookupNodeType(Name domain, Name type) const noexcept {
    if (const uint32* index = typeLookup_.Find(RegistryTypeKey(domain, type)); index != nullptr) {
        const RegisteredNodeType& entry = nodeTypes_[*index].registered;
        if (entry.domain == domain && entry.descriptor.type == type) {
            return &entry;
        }
    }

    // 표가 완전하면 빗나감이 곧 없음이다.
    if (typeLookupComplete_) {
        return nullptr;
    }

    for (const NodeTypeEntry& entry : nodeTypes_) {
        if (entry.registered.domain == domain && entry.registered.descriptor.type == type) {
            return &entry.registered;
        }
    }

    return nullptr;
}

Result<NodeTypeRegistrationId>
NodeTypeRegistry::RegisterNodeType(Name domain, const NodeTypeDescriptor& descriptor,
                                   ModuleLease lease) noexcept {
    if (!initialized_) {
        return Result<NodeTypeRegistrationId>::Failure(ErrorCode::Unsupported);
    }

    if (!domain.IsValid() || domain.IsNone()) {
        return Result<NodeTypeRegistrationId>::Failure(ErrorCode::InvalidArgument);
    }

    const GraphSchema* schema = FindSchema(domain);
    if (schema == nullptr) {
        return Result<NodeTypeRegistrationId>::Failure(ErrorCode::NotFound);
    }

    if (auto valid = ValidateDescriptor(schema, descriptor); !valid) {
        return Result<NodeTypeRegistrationId>::Failure(valid.Error());
    }

    if (FindNodeType(domain, descriptor.type) != nullptr) {
        return Result<NodeTypeRegistrationId>::Failure(ErrorCode::AlreadyExists);
    }

    if (notifying_) {
        return Result<NodeTypeRegistrationId>::Failure(ErrorCode::Busy);
    }

    auto added = AddNodeType(domain, descriptor, Move(lease));
    if (added) {
        NotifyListeners(NodeTypeKey{domain, descriptor.type});
    }

    return added;
}

Result<void> NodeTypeRegistry::UnregisterNodeType(NodeTypeRegistrationId id) noexcept {
    if (notifying_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    for (usize index = 0; index < nodeTypes_.Size(); ++index) {
        // 공통 종류(도메인 None)는 해제할 수 없다.
        if (id.value != 0 && nodeTypes_[index].id.value == id.value &&
            !nodeTypes_[index].registered.domain.IsNone()) {
            // 조회에서 빼기 → 통지 → lease 놓기(10절). 항목을 꺼내 두어 통지가 끝날 때까지 서술
            // 데이터와 lease가 살아 있게 한다.
            NodeTypeEntry removed(Move(nodeTypes_[index]));
            if (auto erased = nodeTypes_.TryRemoveAt(index); !erased) {
                nodeTypes_[index] = Move(removed);
                return erased;
            }

            RebuildTypeLookup();
            NotifyListeners(
                NodeTypeKey{removed.registered.domain, removed.registered.descriptor.type});
            return Result<void>::Success();
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> NodeTypeRegistry::AddListener(NodeTypeRegistryListener& listener) noexcept {
    for (NodeTypeRegistryListener* existing : listeners_) {
        if (existing == &listener) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }

    return listeners_.TryPushBack(&listener);
}

void NodeTypeRegistry::RemoveListener(NodeTypeRegistryListener& listener) noexcept {
    for (usize index = 0; index < listeners_.Size(); ++index) {
        if (listeners_[index] == &listener) {
            // 콜백 중이면 자리만 비운다(반복 중 배열이 줄지 않게).
            if (notifying_) {
                listeners_[index] = nullptr;
            } else {
                static_cast<void>(listeners_.TryRemoveAt(index));
            }

            return;
        }
    }
}

void NodeTypeRegistry::NotifyListeners(NodeTypeKey key) noexcept {
    notifying_ = true;
    for (usize index = 0; index < listeners_.Size(); ++index) {
        if (listeners_[index] != nullptr) {
            listeners_[index]->OnNodeTypesChanged(Span<const NodeTypeKey>(&key, 1));
        }
    }

    notifying_ = false;
    for (usize index = listeners_.Size(); index > 0; --index) {
        if (listeners_[index - 1] == nullptr) {
            static_cast<void>(listeners_.TryRemoveAt(index - 1));
        }
    }
}

const GraphSchema* NodeTypeRegistry::FindSchema(Name domain) const noexcept {
    for (const SchemaEntry& entry : schemas_) {
        if (entry.domain == domain) {
            return entry.schema;
        }
    }

    return nullptr;
}

const RegisteredNodeType* NodeTypeRegistry::FindNodeType(Name domain, Name type) const noexcept {
    // 공통 종류가 먼저다(도메인 None으로 등록돼 있다).
    if (const RegisteredNodeType* common = LookupNodeType(Name(), type); common != nullptr) {
        return common;
    }

    return domain.IsNone() ? nullptr : LookupNodeType(domain, type);
}

Result<void> NodeTypeRegistry::CollectPins(const GraphDocument& document, const Graph& graph,
                                           const GraphNode& node, PinList& out) const noexcept {
    out.Clear();
    const RegisteredNodeType* type = FindNodeType(document.domain, node.type);
    if (type == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    // 서술과 버전이 다른 노드(migration 전)는 핀 목록을 모른다(10.1절).
    if (node.typeSchemaVersion != type->descriptor.schemaVersion) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (IsBoundary(node.type) || IsCollapsed(node.type)) {
        if (auto collected = CollectBoundaryPins(document, graph, node, out); !collected) {
            out.Clear();
            return collected;
        }

        return Result<void>::Success();
    }

    for (const PinDescriptor& pin : type->descriptor.staticPins) {
        if (auto added = out.Add(pin); !added) {
            out.Clear();
            return added;
        }
    }

    if (type->descriptor.dynamicPins != nullptr) {
        const DynamicPinsInput input{document, graph, node, names_};
        if (auto added = type->descriptor.dynamicPins(input, out); !added) {
            out.Clear();
            return added;
        }

        // 동적 핀도 정적 핀과 같은 규칙을 지켜야 한다(타입 색인은 도메인 schema 표).
        const GraphSchema* schema = FindSchema(document.domain);
        const uint32 typeCount = schema != nullptr ? schema->PinTypes().Count() : 0;
        const Span<const PinDescriptor> pins = out.View();
        for (usize index = type->descriptor.staticPins.Size(); index < pins.Size(); ++index) {
            if (!RegistryPinValid(pins[index], typeCount)) {
                out.Clear();
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::nodegraph
