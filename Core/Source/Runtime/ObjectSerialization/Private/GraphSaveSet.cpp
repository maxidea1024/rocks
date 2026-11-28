#include "GraphSaveInternal.h"

// 저장 집합이다: 호출자 집합 노드·Include(Outer 사슬 먼저·결정 17 거부)·NodeTable 이름과 outer
// 값·출력 조립(Docs/Design/ObjectFoundations.ko.md 5.1·5.2절).
namespace rocks::graph_detail {

// 호출자 집합을 local ID 순으로 더한다(같은 handle이 두 번이면 InvalidArgument). lease·Outer·
// 이름은 OpenSetNodes가 채운다.
Result<void> GraphEncodeContext::AddSetNodes(Span<const ObjectHandle> set) noexcept {
    if (auto reserved = nodes_.TryReserve(set.Size()); !reserved) {
        return reserved;
    }

    for (ObjectHandle handle : set) {
        // 노드 추가와 해시 표 삽입을 청구한다.
        if (auto spent = budget_.Spend(2); !spent) {
            return spent;
        }

        auto added = nodes_.TryEmplaceBack(allocator_, handle);
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        auto inserted = locals_.Insert(handle, static_cast<uint32>(nodes_.Size()));
        if (!inserted || inserted.Value() == InsertOutcome::AlreadyPresent) {
            (void)nodes_.TryPopBack();
            return Result<void>::Failure(inserted ? ErrorCode::InvalidArgument : inserted.Error());
        }
    }

    return Result<void>::Success();
}

// 집합 노드마다 실제 타입 lease(생존·문맥 검사 — AcquireType)와 Outer, NodeTable이면 이름을
// 연다.
Result<void> GraphEncodeContext::OpenSetNodes() noexcept {
    for (usize index = 0; index < nodes_.Size(); ++index) {
        if (auto spent = budget_.Spend(1); !spent) {
            return spent;
        }

        auto type = registry_.AcquireType(access_, nodes_[index].handle);
        if (!type) {
            return Result<void>::Failure(type.Error());
        }

        auto outer = registry_.GetOuter(access_, nodes_[index].handle);
        if (!outer) {
            return Result<void>::Failure(outer.Error());
        }

        nodes_[index].lease = Move(type.Value());
        nodes_[index].outer = outer.Value();
        if (auto named = AppendName(nodes_[index]); !named) {
            return named;
        }
    }

    return Result<void>::Success();
}

// Include가 쓰는 거부 조건을 준비한다: 노드 1의 조상(기준 Outer부터 outermost까지)과, 패키지
// 정책 Registry면 노드 1의 outermost.
Result<void> GraphEncodeContext::PrepareInclude() noexcept {
    if (externalPolicy_ != GraphExternalPolicy::Include) {
        return Result<void>::Success();
    }

    for (ObjectHandle at = nodes_[0].outer; at != ObjectHandle{};) {
        if (auto spent = budget_.Spend(2); !spent) {
            return spent;
        }

        if (auto inserted = firstAncestors_.Insert(at, true); !inserted) {
            return Result<void>::Failure(inserted.Error());
        }

        auto next = registry_.GetOuter(access_, at);
        if (!next) {
            return Result<void>::Failure(next.Error());
        }

        at = next.Value();
    }

    if (packagePolicy_) {
        auto outermost = registry_.GetOutermost(access_, nodes_[0].handle);
        if (!outermost) {
            return Result<void>::Failure(outermost.Error());
        }

        firstOutermost_ = outermost.Value();
    }

    return Result<void>::Success();
}

// 집합 밖 참조를 지금 위치의 ExternalReference 진단(value 0 = 거부) 뒤 InvalidArgument로
// 거부한다 (hook 실패는 그 오류).
Result<void> GraphEncodeContext::RejectExternal() noexcept {
    if (auto reported = Report(FieldDiagnosticKind::ExternalReference, 0); !reported) {
        return reported;
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

// Include: target을 노드로 더하고 local ID를 돌려준다. target의 Outer 사슬 중 집합에 없는
// 조상을 바깥쪽부터 먼저 더한다(집합 안 조상·기준 Outer·Outer 없음에서 멈춘다). target이나 그
// 조상이 기준 Outer·노드 1의 조상이거나, 패키지 정책에서 target의 outermost가 노드 1의 것과
// 다르거나, 노드 1에 기준 Outer가 있는데 사슬이 Outer 없음에 닿으면 거부한다(11절 결정 17 —
// 그대로 더하면 NodeTable의 Outer 순서·혼합 규칙에 걸린다).
Result<uint32> GraphEncodeContext::IncludeNode(ObjectHandle target, TypeLease lease) noexcept {
    if (firstAncestors_.Contains(target)) {
        auto rejected = RejectExternal();
        return Result<uint32>::Failure(rejected.Error());
    }

    if (packagePolicy_) {
        auto outermost = registry_.GetOutermost(access_, target);
        if (!outermost) {
            return Result<uint32>::Failure(outermost.Error());
        }

        if (outermost.Value() != firstOutermost_) {
            auto rejected = RejectExternal();
            return Result<uint32>::Failure(rejected.Error());
        }
    }

    auto targetOuter = registry_.GetOuter(access_, target);
    if (!targetOuter) {
        return Result<uint32>::Failure(targetOuter.Error());
    }

    // 안쪽 조상부터 모은다. 멈춘 자리(stop)가 가장 바깥 조상의 Outer다.
    chain_.Clear();
    ObjectHandle stop = targetOuter.Value();
    while (stop != ObjectHandle{}) {
        if (auto spent = budget_.Spend(2); !spent) {
            return Result<uint32>::Failure(spent.Error());
        }

        if (locals_.Contains(stop) || stop == nodes_[0].outer) {
            break;
        }

        if (firstAncestors_.Contains(stop)) {
            auto rejected = RejectExternal();
            return Result<uint32>::Failure(rejected.Error());
        }

        if (!chain_.TryPushBack(stop)) {
            return Result<uint32>::Failure(ErrorCode::OutOfMemory);
        }

        auto next = registry_.GetOuter(access_, stop);
        if (!next) {
            return Result<uint32>::Failure(next.Error());
        }

        stop = next.Value();
    }

    // 사슬이 Outer 없음에 닿았는데(다른 트리 — 정책 없는 Registry) 노드 1에 기준 Outer가 있으면
    // 더할 가장 바깥 조상이 Outer 없음 노드가 되어 기준 Outer와 섞이므로 거부한다.
    if (stop == ObjectHandle{} && nodes_[0].outer != ObjectHandle{}) {
        auto rejected = RejectExternal();
        return Result<uint32>::Failure(rejected.Error());
    }

    for (usize index = chain_.Size(); index > 0; --index) {
        const ObjectHandle ancestor = chain_[index - 1];
        auto type = registry_.AcquireType(access_, ancestor);
        if (!type) {
            return Result<uint32>::Failure(type.Error());
        }

        const ObjectHandle outer = index < chain_.Size() ? chain_[index] : stop;
        if (auto added = AddNode(ancestor, outer, Move(type.Value())); !added) {
            return added;
        }
    }

    return AddNode(target, targetOuter.Value(), Move(lease));
}

// 새 노드를 끝에 더하고 local ID를 돌려준다(maxObjects를 넘으면 CapacityExceeded).
Result<uint32> GraphEncodeContext::AddNode(ObjectHandle handle, ObjectHandle outer,
                                           TypeLease lease) noexcept {
    if (nodes_.Size() >= budget_.limits.maxObjects) {
        return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
    }

    // 노드 배열 성장(기존 원소 이동)과 해시 표 삽입을 청구한다.
    auto spent = budget_.Spend(2 + (nodes_.Size() == nodes_.Capacity() ? nodes_.Size() : 0));
    if (!spent) {
        return Result<uint32>::Failure(spent.Error());
    }

    // 노드를 먼저 더하고 이름·맵 삽입이 실패하면 되돌린다(맵의 local ID는 언제나 nodes 안이다).
    auto added = nodes_.TryEmplaceBack(allocator_, handle);
    if (!added) {
        return Result<uint32>::Failure(added.Error());
    }

    added.Value()->outer = outer;
    added.Value()->lease = Move(lease);
    if (auto named = AppendName(*added.Value()); !named) {
        (void)nodes_.TryPopBack();
        return Result<uint32>::Failure(named.Error());
    }

    const uint32 local = static_cast<uint32>(nodes_.Size());
    if (auto inserted = locals_.Insert(handle, local); !inserted) {
        (void)nodes_.TryPopBack();
        return Result<uint32>::Failure(inserted.Error());
    }

    return Result<uint32>::Success(local);
}

// NodeTable을 쓸 때 노드 이름을 이름 버퍼 끝에 붙이고 위치를 기록한다(이름 byte를 청구한다).
Result<void> GraphEncodeContext::AppendName(SaveNode& node) noexcept {
    if (!writeNodeTable_) {
        return Result<void>::Success();
    }

    const usize offset = names_.ByteCount();
    if (auto named = registry_.GetName(access_, node.handle, names_); !named) {
        return named;
    }

    node.nameOffset = offset;
    node.nameBytes = names_.ByteCount() - offset;
    return budget_.Spend(node.nameBytes);
}

// NodeTable의 outer 값과 BaseOuter 비트를 정한다(5.2절): 집합 안 Outer는 그 local ID(자기보다
// 앞이어야 한다), 집합 밖 Outer는 0이고 모두 기준 Outer(노드 1의 Outer)와 같아야 한다 — Outer
// 없음과 기준 Outer를 섞을 수 없다. 어기면 InvalidArgument다. outers는 노드 순서의 outer 값을
// 받는다.
Result<bool> ResolveNodeTable(const Array<SaveNode>& nodes, const GraphEncodeContext& context,
                              Array<uint32>& outers, Budget& budget) noexcept {
    if (auto spent = budget.Spend(nodes.Size()); !spent) {
        return Result<bool>::Failure(spent.Error());
    }

    if (auto reserved = outers.TryReserve(nodes.Size()); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    const ObjectHandle base = nodes[0].outer;
    for (usize index = 0; index < nodes.Size(); ++index) {
        const ObjectHandle outer = nodes[index].outer;
        uint32 value = 0;
        if (outer != ObjectHandle{}) {
            value = context.LocalOf(outer);
            if (value > index) {
                return Result<bool>::Failure(ErrorCode::InvalidArgument);
            }
        }

        if (value == 0 && outer != base) {
            return Result<bool>::Failure(ErrorCode::InvalidArgument);
        }

        if (!outers.TryPushBack(value)) {
            return Result<bool>::Failure(ErrorCode::OutOfMemory);
        }
    }

    return Result<bool>::Success(base != ObjectHandle{});
}

// 선언 subobject 노드를 가린다(Docs/Design/Prefab 10.4절): 생성 출처가 ClassSubobject이고 Outer가
// 집합 안 노드(소유 객체)인 노드는 소유 노드 타입 표에서 이름(= 소유 필드 도구 이름)이 같은 항목의
// SubobjectGuid를 subobjectId에 둔다 — 복원이 새로 만들지 않고 소유 노드가 만든 subobject에 묶는다.
// 소유 객체 없이 저장한 선언 subobject는 객체 노드로 쓴다. NodeTable을 쓰지 않으면(writeNodeTable
// 거짓) 선언 subobject를 표현할 수 없으므로 소유 객체와 함께 든 선언 subobject는 InvalidArgument다.
Result<void> ClassifyDeclaredSubobjects(Array<SaveNode>& nodes, const GraphEncodeContext& context,
                                        ObjectRegistry& registry, const ObjectAccessContext& access,
                                        bool nodeTable, Budget& budget) noexcept {
    if (auto spent = budget.Spend(nodes.Size()); !spent) {
        return spent;
    }

    const StringView names = context.Names();
    for (usize index = 0; index < nodes.Size(); ++index) {
        const uint32 owner =
            nodes[index].outer != ObjectHandle{} ? context.LocalOf(nodes[index].outer) : 0;
        if (owner == 0 || nodes[owner - 1].lease.Schema().subobjects.Size() == 0) {
            continue;
        }

        auto origin = registry.GetOrigin(access, nodes[index].handle);
        if (!origin) {
            return Result<void>::Failure(origin.Error());
        }

        if (origin.Value() != ObjectOrigin::ClassSubobject) {
            continue;
        }

        if (!nodeTable) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const StringView name(names.Data() + nodes[index].nameOffset, nodes[index].nameBytes);
        const TypeSchema& schema = nodes[owner - 1].lease.Schema();
        for (const SubobjectSchema& entry : schema.subobjects) {
            for (const FieldSchema& field : schema.fields) {
                if (field.id != entry.id) {
                    continue;
                }

                const Span<const char> fieldName =
                    field.toolName.Size() != 0 ? field.toolName : field.name;
                if (StringView(fieldName.Data(), fieldName.Size()) == name) {
                    nodes[index].subobjectId = entry.id;
                }
            }
        }

        // 표에 없는 이름의 ClassSubobject는 정합성 위반이다(생성 규칙이 표 이름으로 만든다).
        if (nodes[index].subobjectId == Guid{}) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    return Result<void>::Success();
}

// header·NodeTable·Record를 출력 하나로 쓴다. outers가 비어 있으면 NodeTable 없이 쓴다. Record를
// 쓰기 직전 출력 위치 + 길이 prefix가 그 Record의 출력 시작이고, 그 노드의 외부 참조 위치(Record
// offset)에 더해 출력 offset으로 바꾼다.
Result<Array<uint8>> WriteGraph(Allocator& allocator, const Array<SaveNode>& nodes,
                                GraphEncodeContext& context, const Array<uint32>& outers,
                                bool baseOuter, Budget& budget) noexcept {
    const bool nodeTable = outers.Size() == nodes.Size();
    const uint32 flags = (nodeTable ? NodeTableFlag : 0) | (baseOuter ? BaseOuterFlag : 0);
    const StringView names = context.Names();
    ArchiveWriter output(allocator, budget.Archive(true));
    auto written = output.WriteRaw(Magic);
    const uint32 header[] = {FormatVersion, flags, static_cast<uint32>(nodes.Size()),
                             static_cast<uint32>(context.Externals().Size())};
    for (uint32 value : header) {
        if (written) {
            written = output.WriteU32(value);
        }
    }

    // 노드 항목: 종류 byte 뒤 객체는 {타입, outer, 이름}, 선언 subobject는 {SubobjectGuid, 소유
    // 노드, 빈 이름}이다(graph 형식 3 — Docs/Design/Prefab 10.4절).
    for (usize index = 0; written && nodeTable && index < nodes.Size(); ++index) {
        const bool declared = nodes[index].subobjectId != Guid{};
        written = output.WriteU8(declared ? NodeKindDeclaredSubobject : NodeKindObject);
        if (written) {
            written = output.WriteGuid(declared ? nodes[index].subobjectId : nodes[index].type);
        }

        if (written) {
            written = output.WriteU32(outers[index]);
        }

        if (written) {
            written =
                output.WriteString(declared ? StringView{}
                                            : StringView(names.Data() + nodes[index].nameOffset,
                                                         nodes[index].nameBytes));
        }
    }

    for (usize index = 0; written && index < nodes.Size(); ++index) {
        const usize recordStart = output.ByteCount() + RecordPrefixBytes;
        written = output.WriteBytes(nodes[index].record.View());
        // 이 노드의 위치는 [firstPosition, 다음 노드의 firstPosition)이다.
        const usize end =
            index + 1 < nodes.Size() ? nodes[index + 1].firstPosition : context.PositionCount();
        if (written) {
            written = context.RebasePositions(nodes[index].firstPosition, end, recordStart);
        }
    }

    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }

    auto spent = budget.Spend(output.WorkUsed());
    if (!spent) {
        return Result<Array<uint8>>::Failure(spent.Error());
    }

    return Result<Array<uint8>>::Success(output.TakeBytes());
}

} // namespace rocks::graph_detail
