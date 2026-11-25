#include "GraphMutatorCore.h"

#include <Core/Assert.h>
#include <NodeGraph/GraphClone.h>
#include <NodeGraph/GraphMutator.h>
#include <Serialization/Record.h>

// 핀 재정리와 migration이다(Docs/Design/NodeGraph 10.1절). 노드의 핀 목록이 바뀌는 곳(속성 변경,
// migration)은 모두 ReconcileNode 하나를 쓴다.

namespace rocks::nodegraph::detail {

namespace {

bool ReconcileNameNone(Name name) noexcept {
    return !name.IsValid() || name.IsNone();
}

Name ReconcileRenamed(Span<const PinKeyRename> renames, Name key) noexcept {
    for (const PinKeyRename& rename : renames) {
        if (rename.from == key) {
            return rename.to;
        }
    }

    return key;
}

usize ReconcileRetainedIndex(const GraphNode& node, Name key) noexcept {
    for (usize index = 0; index < node.retainedPins.Size(); ++index) {
        if (node.retainedPins[index].key == key) {
            return index;
        }
    }

    return IndexNone;
}

bool ReconcilePropertiesMatch(const NodeTypeDescriptor& descriptor,
                              Span<const uint8> bytes) noexcept {
    if (descriptor.propertiesStruct.high == 0 && descriptor.propertiesStruct.low == 0) {
        return bytes.Size() == 0;
    }

    auto header = ReadRecordHeader(bytes);
    return header && header.Value().schema == descriptor.propertiesStruct;
}

Result<void> ReconcileAddUnique(Array<Name>& keys, Name key) noexcept {
    for (const Name& existing : keys) {
        if (existing == key) {
            return Result<void>::Success();
        }
    }

    return keys.TryPushBack(key);
}

} // namespace

GraphCommandReason GraphMutatorCore::UnknownReason(const GraphNode& node) const noexcept {
    // 등록된 종류인데 핀을 모르면 버전이 서술과 다르다(migration 전 — 10.1절).
    return registry_.FindNodeType(document_.domain, node.type) != nullptr
               ? GraphCommandReason::TypeVersion
               : GraphCommandReason::TypeUnknown;
}

void GraphMutatorCore::RevertTo(usize mark) noexcept {
    // mark 뒤 op만 LIFO로 되돌린다(무할당 — 8.6절).
    for (usize index = work_.Size(); index > mark; --index) {
        ROCKS_ASSERT(ApplyOp(work_[index - 1], false));
    }

    if (work_.Size() > mark) {
        ROCKS_ASSERT(work_.TryRemoveAt(mark, work_.Size() - mark));
    }
}

Result<void> GraphMutatorCore::ReconcileNode(Graph& graph, GraphNode&& changed,
                                             Span<const PinKeyRename> renames,
                                             const PinList* oldPins,
                                             uint32* retainedCreated) noexcept {
    const Guid graphId = graph.id;
    const Guid nodeId = changed.id;
    PinList pins(allocator_);
    auto known = PinsOf(graph, changed, pins);
    if (!known) {
        if (known.Error() == ErrorCode::OutOfMemory) {
            return Result<void>::Failure(known.Error());
        }

        return Fail(GraphCommandReason::PropertiesSchema, graphId, nodeId);
    }

    if (!known.Value()) {
        return Fail(UnknownReason(changed), graphId, nodeId);
    }

    // 닿은 간선이다. 자기 간선은 들어오는 쪽에서 한 번만 센다.
    Array<GraphEdge> touching(allocator_);
    if (auto collected = CollectIncoming(graph, nodeId, Name(), touching); !collected) {
        return collected;
    }

    {
        Array<GraphEdge> outgoing(allocator_);
        if (auto collected = CollectOutgoing(graph, nodeId, Name(), outgoing); !collected) {
            return collected;
        }

        for (const GraphEdge& edge : outgoing) {
            if (edge.toNode != nodeId) {
                if (auto pushed = touching.TryPushBack(edge); !pushed) {
                    return pushed;
                }
            }
        }
    }

    // 규칙 1: 키 재지정. 쓰인 옛 키마다 새 키가 None이 아니고 서로 겹치지 않아야 한다.
    if (!renames.IsEmpty()) {
        Array<Name> used(allocator_);
        for (const GraphEdge& edge : touching) {
            if (edge.toNode == nodeId) {
                if (auto added = ReconcileAddUnique(used, edge.toPin); !added) {
                    return added;
                }
            }

            if (edge.fromNode == nodeId) {
                if (auto added = ReconcileAddUnique(used, edge.fromPin); !added) {
                    return added;
                }
            }
        }

        for (const PinValueEntry& entry : changed.inputDefaults) {
            if (auto added = ReconcileAddUnique(used, entry.pin); !added) {
                return added;
            }
        }

        for (const RetainedPin& retained : changed.retainedPins) {
            if (auto added = ReconcileAddUnique(used, retained.key); !added) {
                return added;
            }
        }

        for (usize index = 0; index < used.Size(); ++index) {
            const Name target = ReconcileRenamed(renames, used[index]);
            if (ReconcileNameNone(target)) {
                return Fail(GraphCommandReason::MigrationInvalid, graphId, nodeId, used[index]);
            }

            for (usize other = 0; other < index; ++other) {
                if (ReconcileRenamed(renames, used[other]) == target) {
                    return Fail(GraphCommandReason::MigrationInvalid, graphId, nodeId, target);
                }
            }
        }

        for (PinValueEntry& entry : changed.inputDefaults) {
            entry.pin = ReconcileRenamed(renames, entry.pin);
        }

        for (RetainedPin& retained : changed.retainedPins) {
            retained.key = ReconcileRenamed(renames, retained.key);
        }
    }

    // 규칙 4: 기존 고아 핀. 같은 키·같은 방향의 현재 핀이 생기면 재결합한다(값은 입력 기본값으로).
    for (usize index = changed.retainedPins.Size(); index > 0; --index) {
        RetainedPin& retained = changed.retainedPins[index - 1];
        const PinDescriptor* pin = pins.Find(retained.key);
        if (pin == nullptr) {
            continue;
        }

        if (pin->direction != retained.direction) {
            return Fail(GraphCommandReason::RetainedShadowed, graphId, nodeId, retained.key);
        }

        if (!retained.value.bytes.IsEmpty()) {
            if (pin->direction != PinDirection::Input) {
                return Fail(GraphCommandReason::PinDirection, graphId, nodeId, retained.key);
            }

            if (pin->valuePolicy == PinValuePolicy::Hidden) {
                return Fail(GraphCommandReason::ValueHidden, graphId, nodeId, retained.key);
            }

            PinValueEntry entry(allocator_);
            entry.pin = retained.key;
            entry.value.bytes = Move(retained.value.bytes);
            if (auto pushed = changed.inputDefaults.TryPushBack(Move(entry)); !pushed) {
                return pushed;
            }
        }

        if (auto removed = changed.retainedPins.TryRemoveAt(index - 1); !removed) {
            return removed;
        }
    }

    uint32 created = 0;
    // 키의 고아 핀을 찾거나 만든다. 이미 있는데 방향이 다르면 실패다.
    const auto ensureRetained = [this, &changed, &created, oldPins, graphId,
                                 nodeId](Name key, PinDirection direction,
                                         usize& out) noexcept -> Result<void> {
        out = ReconcileRetainedIndex(changed, key);
        if (out != IndexNone) {
            if (changed.retainedPins[out].direction != direction) {
                return Fail(GraphCommandReason::PinDirection, graphId, nodeId, key);
            }

            return Result<void>::Success();
        }

        RetainedPin retained(allocator_);
        retained.key = key;
        retained.direction = direction;
        retained.lastType = UnresolvedPinType;
        if (oldPins != nullptr) {
            if (const PinDescriptor* old = oldPins->Find(key); old != nullptr) {
                retained.lastType = old->type;
            }
        }

        if (auto pushed = changed.retainedPins.TryPushBack(Move(retained)); !pushed) {
            return pushed;
        }

        out = changed.retainedPins.Size() - 1;
        ++created;
        return Result<void>::Success();
    };

    // 규칙 2: 간선 끝. 현재 핀에 같은 방향으로 있으면 그대로, 키가 없으면 고아 핀 간선이다.
    const auto placeEnd = [&pins, &ensureRetained, this, graphId,
                           nodeId](Name key, PinDirection direction) noexcept -> Result<void> {
        const PinDescriptor* pin = pins.Find(key);
        if (pin != nullptr) {
            if (pin->direction != direction) {
                return Fail(GraphCommandReason::PinDirection, graphId, nodeId, key);
            }

            return Result<void>::Success();
        }

        usize slot = IndexNone;
        return ensureRetained(key, direction, slot);
    };

    for (const GraphEdge& edge : touching) {
        if (edge.toNode == nodeId) {
            const Name key = ReconcileRenamed(renames, edge.toPin);
            if (auto placed = placeEnd(key, PinDirection::Input); !placed) {
                return placed;
            }
        }

        if (edge.fromNode == nodeId) {
            const Name key = ReconcileRenamed(renames, edge.fromPin);
            if (auto placed = placeEnd(key, PinDirection::Output); !placed) {
                return placed;
            }
        }
    }

    // 규칙 3: 입력 기본값. 현재 입력 핀(Hidden 아님)이면 남기고, 키가 없으면 고아 핀 값으로 보낸다.
    for (usize index = changed.inputDefaults.Size(); index > 0; --index) {
        PinValueEntry& entry = changed.inputDefaults[index - 1];
        const PinDescriptor* pin = pins.Find(entry.pin);
        if (pin != nullptr) {
            if (pin->direction != PinDirection::Input) {
                return Fail(GraphCommandReason::PinDirection, graphId, nodeId, entry.pin);
            }

            if (pin->valuePolicy == PinValuePolicy::Hidden) {
                return Fail(GraphCommandReason::ValueHidden, graphId, nodeId, entry.pin);
            }

            continue;
        }

        usize slot = IndexNone;
        if (auto ensured = ensureRetained(entry.pin, PinDirection::Input, slot); !ensured) {
            return ensured;
        }

        changed.retainedPins[slot].value.bytes = Move(entry.value.bytes);
        if (auto removed = changed.inputDefaults.TryRemoveAt(index - 1); !removed) {
            return removed;
        }
    }

    // 규칙 5: 현재 입력 핀의 간선 수 상한.
    for (const PinDescriptor& pin : pins.View()) {
        if (pin.direction != PinDirection::Input || pin.maxEdges == UnlimitedEdges) {
            continue;
        }

        usize count = 0;
        for (const GraphEdge& edge : touching) {
            count +=
                edge.toNode == nodeId && ReconcileRenamed(renames, edge.toPin) == pin.key ? 1 : 0;
        }

        if (count > pin.maxEdges) {
            return Fail(GraphCommandReason::EdgeLimit, graphId, nodeId, pin.key);
        }
    }

    // 규칙 6: 간선·값이 없는 고아 핀은 지운다(GI7).
    for (usize index = changed.retainedPins.Size(); index > 0; --index) {
        const RetainedPin& retained = changed.retainedPins[index - 1];
        if (!retained.value.bytes.IsEmpty()) {
            continue;
        }

        bool linked = false;
        for (const GraphEdge& edge : touching) {
            if (retained.direction == PinDirection::Input) {
                linked = linked || (edge.toNode == nodeId &&
                                    ReconcileRenamed(renames, edge.toPin) == retained.key);
            } else {
                linked = linked || (edge.fromNode == nodeId &&
                                    ReconcileRenamed(renames, edge.fromPin) == retained.key);
            }
        }

        if (!linked) {
            if (auto removed = changed.retainedPins.TryRemoveAt(index - 1); !removed) {
                return removed;
            }
        }
    }

    auto sorted = SortNodeLists(changed);
    if (!sorted) {
        return Result<void>::Failure(sorted.Error());
    }

    ROCKS_ASSERT(sorted.Value());
    if (auto replaced = ReplaceNode(graphId, Move(changed)); !replaced) {
        return replaced;
    }

    // 키가 바뀐 간선을 바꿔 넣는다(정규 순서 자리가 달라진다). 순번은 그대로 두고 아래에서 맞춘다.
    // 연쇄(A → B, B → C)·교환(A ↔ B) 재지정은 새 키가 아직 옮기지 않은 간선의 옛 키와 같을 수
    // 있으므로 바뀌는 간선을 모두 지운 뒤에 넣는다. 재지정이 단사라서 새 간선끼리, 그리고 그대로인
    // 간선과 겹치지 않는다(규칙 1).
    Array<Name> outputs(allocator_);
    Array<GraphEdge> moved(allocator_);
    for (const GraphEdge& edge : touching) {
        GraphEdge target = edge;
        if (edge.fromNode == nodeId) {
            target.fromPin = ReconcileRenamed(renames, edge.fromPin);
            if (auto added = ReconcileAddUnique(outputs, target.fromPin); !added) {
                return added;
            }
        }

        if (edge.toNode == nodeId) {
            target.toPin = ReconcileRenamed(renames, edge.toPin);
        }

        if (target.fromPin == edge.fromPin && target.toPin == edge.toPin) {
            continue;
        }

        if (auto pushed = moved.TryPushBack(target); !pushed) {
            return pushed;
        }

        if (auto erased = EraseEdge(graphId, edge); !erased) {
            return erased;
        }
    }

    for (const GraphEdge& edge : moved) {
        GraphPatchOp op = MakeOp(GraphSlotKind::Edge, graphId);
        op.edgeKey = edge;
        op.edgeKey.order = 0;
        op.edge = edge;
        op.hasValue = true;
        if (auto emitted = Emit(Move(op)); !emitted) {
            return emitted;
        }
    }

    // 규칙 5의 순번: 현재 출력 핀은 ordered면 (옛 순번, 정규 순서)로 다시 매기고 아니면 0이다.
    // 고아 핀 간선은 순번을 그대로 둔다(NormalizeOutputOrder가 고아 핀을 건너뛴다).
    for (const Name& output : outputs) {
        if (auto normalized = NormalizeOutputOrder(graph, nodeId, output); !normalized) {
            return normalized;
        }
    }

    if (retainedCreated != nullptr) {
        *retainedCreated = created;
    }

    return Result<void>::Success();
}

Result<void> GraphMutatorCore::PlanMigration(const Graph& graph, const GraphNode& node,
                                             GraphNode& out,
                                             Array<PinKeyRename>& renames) noexcept {
    // 쓰인 키마다 (원래 키, 지금 키)를 따라간다. 단계마다 지금 키에 그 단계의 재지정을 적용한다.
    Array<Name> origin(allocator_);
    {
        Array<GraphEdge> edges(allocator_);
        if (auto collected = CollectIncoming(graph, node.id, Name(), edges); !collected) {
            return collected;
        }

        for (const GraphEdge& edge : edges) {
            if (auto added = ReconcileAddUnique(origin, edge.toPin); !added) {
                return added;
            }
        }

        edges.Clear();
        if (auto collected = CollectOutgoing(graph, node.id, Name(), edges); !collected) {
            return collected;
        }

        for (const GraphEdge& edge : edges) {
            if (auto added = ReconcileAddUnique(origin, edge.fromPin); !added) {
                return added;
            }
        }

        for (const PinValueEntry& entry : node.inputDefaults) {
            if (auto added = ReconcileAddUnique(origin, entry.pin); !added) {
                return added;
            }
        }

        for (const RetainedPin& retained : node.retainedPins) {
            if (auto added = ReconcileAddUnique(origin, retained.key); !added) {
                return added;
            }
        }
    }

    Array<Name> current(allocator_);
    if (auto copied = current.TryAppend(origin.View()); !copied) {
        return copied;
    }

    // step은 함수에 넘기는 중간 노드다(키 재지정이 반영된 입력 기본값·고아 핀).
    GraphNode step(allocator_);
    if (auto cloned = CloneGraphNode(node, step); !cloned) {
        return cloned;
    }

    const RegisteredNodeType* type = registry_.FindNodeType(document_.domain, step.type);
    ROCKS_ASSERT(type != nullptr);
    uint32 steps = 0;
    while (step.typeSchemaVersion < type->descriptor.schemaVersion) {
        if (++steps > MaxMigrationSteps || type->descriptor.migrate == nullptr) {
            return Fail(GraphCommandReason::MigrationInvalid, graph.id, node.id);
        }

        NodeMigrationResult result(allocator_);
        result.type = step.type;
        result.schemaVersion = step.typeSchemaVersion;
        if (auto copied = CloneBytes(step.properties.bytes, result.properties.bytes); !copied) {
            return copied;
        }

        const NodeMigrationInput input{document_, graph, step, step.typeSchemaVersion,
                                       registry_.Names()};
        if (auto migrated = type->descriptor.migrate(input, result); !migrated) {
            if (migrated.Error() == ErrorCode::OutOfMemory) {
                return migrated;
            }

            return Fail(GraphCommandReason::MigrationInvalid, graph.id, node.id);
        }

        // 버전 규칙: 같은 종류면 올리고, 다른 종류(1:1 교체)는 같은 도메인의 공통 아닌 종류다.
        const RegisteredNodeType* next =
            ReconcileNameNone(result.type) ? nullptr
                                           : registry_.FindNodeType(document_.domain, result.type);
        bool valid = next != nullptr && result.properties.bytes.Size() <= MaxPropertyRecordBytes;
        if (valid && result.type == step.type) {
            valid = result.schemaVersion > step.typeSchemaVersion &&
                    result.schemaVersion <= type->descriptor.schemaVersion;
        } else if (valid) {
            valid = !registry_.IsCommonType(result.type) && result.schemaVersion >= 1 &&
                    result.schemaVersion <= next->descriptor.schemaVersion;
        }

        // 한 단계 안의 재지정은 함수여야 한다(같은 옛 키 두 번 금지).
        for (usize index = 0; valid && index < result.renames.Size(); ++index) {
            const PinKeyRename& rename = result.renames[index];
            valid = !ReconcileNameNone(rename.from) && !ReconcileNameNone(rename.to);
            for (usize other = 0; valid && other < index; ++other) {
                valid = !(result.renames[other].from == rename.from);
            }
        }

        if (!valid) {
            return Fail(GraphCommandReason::MigrationInvalid, graph.id, node.id);
        }

        const Span<const PinKeyRename> stepRenames = result.renames.View();
        for (Name& key : current) {
            key = ReconcileRenamed(stepRenames, key);
        }

        for (PinValueEntry& entry : step.inputDefaults) {
            entry.pin = ReconcileRenamed(stepRenames, entry.pin);
        }

        for (RetainedPin& retained : step.retainedPins) {
            retained.key = ReconcileRenamed(stepRenames, retained.key);
        }

        step.type = result.type;
        step.typeSchemaVersion = result.schemaVersion;
        step.properties.bytes = Move(result.properties.bytes);
        type = next;
    }

    // 최종 속성 schema와 문서당 하나 규칙(1:1 교체)이다.
    if (!ReconcilePropertiesMatch(type->descriptor, step.properties.bytes.View())) {
        return Fail(GraphCommandReason::PropertiesSchema, graph.id, node.id);
    }

    if (node.enableState == NodeEnableState::Disabled &&
        !HasCapability(type->descriptor.capabilities, NodeCapabilities::AllowDisable)) {
        return Fail(GraphCommandReason::EnableState, graph.id, node.id);
    }

    if (!(step.type == node.type) &&
        HasCapability(type->descriptor.capabilities, NodeCapabilities::SinglePerDocument)) {
        for (const Graph& other : document_.graphs) {
            for (const GraphNode& candidate : other.nodes) {
                if (candidate.type == step.type && candidate.id != node.id) {
                    return Fail(GraphCommandReason::SinglePerDocument, graph.id, node.id);
                }
            }
        }
    }

    // 결과 노드: 종류·버전·속성은 마지막 단계 것, 입력 기본값·고아 핀은 원래 키(재지정은
    // ReconcileNode).
    if (auto cloned = CloneGraphNode(node, out); !cloned) {
        return cloned;
    }

    out.type = step.type;
    out.typeSchemaVersion = step.typeSchemaVersion;
    out.properties.bytes = Move(step.properties.bytes);
    renames.Clear();
    for (usize index = 0; index < origin.Size(); ++index) {
        if (!(origin[index] == current[index])) {
            PinKeyRename rename;
            rename.from = origin[index];
            rename.to = current[index];
            if (auto pushed = renames.TryPushBack(rename); !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> GraphMutatorCore::Migrate(uint64 source, GraphMigrationReport& report) noexcept {
    static_cast<void>(source);
    if (!initialized_) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    report.entries.Clear();
    if (broken_) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    if (interaction_ || notifying_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    GraphCommandFailure local;
    failure_ = &local;
    // 끝나면 지역 실패 정보를 가리키지 않게 되돌린다.
    struct FailureReset {
        GraphCommandFailure*& target;
        ~FailureReset() noexcept {
            target = nullptr;
        }
    } failureReset{failure_};
    work_.Clear();
    pending_.changes.Clear();
    pending_.semanticTouched = false;
    const auto abort = [this, &report](ErrorCode error) noexcept {
        Revert(work_, work_.Size());
        work_.Clear();
        pending_.changes.Clear();
        pending_.semanticTouched = false;
        report.entries.Clear();
        return Result<void>::Failure(error);
    };

    // 노드 교체는 자리(그래프·노드 순서)를 바꾸지 않으므로 색인으로 차례로 돈다.
    for (usize graphIndex = 0; graphIndex < document_.graphs.Size(); ++graphIndex) {
        Graph& graph = document_.graphs[graphIndex];
        for (usize nodeIndex = 0; nodeIndex < graph.nodes.Size(); ++nodeIndex) {
            const GraphNode& node = graph.nodes[nodeIndex];
            const RegisteredNodeType* type = registry_.FindNodeType(document_.domain, node.type);
            if (type == nullptr || node.typeSchemaVersion == 0 ||
                node.typeSchemaVersion >= type->descriptor.schemaVersion) {
                continue;
            }

            GraphMigrationEntry entry;
            entry.graph = graph.id;
            entry.node = node.id;
            entry.fromType = node.type;
            entry.fromVersion = node.typeSchemaVersion;
            local = GraphCommandFailure{};
            const usize mark = work_.Size();
            const usize changeMark = pending_.changes.Size();
            const bool semanticBefore = pending_.semanticTouched;
            GraphNode planned(allocator_);
            Array<PinKeyRename> renames(allocator_);
            auto outcome = PlanMigration(graph, node, planned, renames);
            if (outcome) {
                entry.toType = planned.type;
                entry.toVersion = planned.typeSchemaVersion;
                outcome = ReconcileNode(graph, Move(planned), renames.View(), nullptr,
                                        &entry.retainedCreated);
            }

            if (!outcome) {
                if (outcome.Error() != ErrorCode::InvalidArgument) {
                    return abort(outcome.Error());
                }

                // 이 노드만 되돌리고 옛 버전으로 둔다.
                RevertTo(mark);
                if (pending_.changes.Size() > changeMark) {
                    ROCKS_ASSERT(pending_.changes.TryRemoveAt(changeMark, pending_.changes.Size() -
                                                                              changeMark));
                }

                pending_.semanticTouched = semanticBefore;
                entry.toType = Name();
                entry.toVersion = 0;
                entry.retainedCreated = 0;
                entry.failure = local.reason;
                entry.pin = local.pin;
            }

            if (auto pushed = report.entries.TryPushBack(entry); !pushed) {
                return abort(pushed.Error());
            }
        }
    }

    if (work_.IsEmpty()) {
        pending_.changes.Clear();
        pending_.semanticTouched = false;
        return Result<void>::Success();
    }

    if (auto checked = CheckFullValidation(); !checked) {
        return abort(checked.Error());
    }

    // undo 이력 밖 변경이므로 새 상태 토큰을 받아 그 전 patch를 모두 낡게 한다.
    work_.Clear();
    state_ = nextState_;
    ++nextState_;
    ++revision_;
    Notify(GraphChangeCause::Migration, false);
    return Result<void>::Success();
}

} // namespace rocks::nodegraph::detail

namespace rocks::nodegraph {

namespace {

// migration은 새 id를 발급하지 않는다. 임시 mutator용 발급기로, 불리면 실패(Unsupported)를
// 돌려준다.
class MigrationIdSource final : public GraphIdSource {
public:
    Result<Guid> Next() noexcept override {
        return Result<Guid>::Failure(ErrorCode::Unsupported);
    }
};

} // namespace

Result<void> MigrateGraphDocument(GraphDocument& document, GraphDocumentLayout& layout,
                                  const NodeTypeRegistry& registry,
                                  GraphMigrationReport& report) noexcept {
    MigrationIdSource ids;
    GraphMutator mutator(document, layout, registry, ids);
    if (auto initialized = mutator.Initialize(); !initialized) {
        return initialized;
    }

    return mutator.Migrate(0, report);
}

} // namespace rocks::nodegraph
