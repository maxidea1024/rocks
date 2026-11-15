#include "GraphSaveInternal.h"

namespace rocks::graph_detail {

// 저장 결과를 만드는 내부 통로다(ObjectGraphSaveResult의 생성자는 비공개다).
struct SaveResultAccess {
    static ObjectGraphSaveResult Make(Array<uint8>&& bytes, Array<ObjectHandle>&& nodes,
                                      Array<GraphExternal>&& externals, Array<uint32>&& positions,
                                      uint32 maxExternalReferences) noexcept {
        return ObjectGraphSaveResult(Move(bytes), Move(nodes), Move(externals), Move(positions),
                                     maxExternalReferences);
    }
};

Result<void>
GraphEncodeContext::WriteReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                       const ReferenceExpectation& expectation) noexcept {
    if (target == ObjectHandle{}) {
        return out.WriteU32(0);
    }

    if (target.IsNull()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (auto spent = budget_.Spend(1); !spent) {
        return spent;
    }

    // 집합 노드·Import한 외부 대상이면 그 lease로 검사하고, 아니면 대상 타입을 열어(생존·문맥 검사
    // 포함) 검사가 끝난 뒤 정책대로 처리한다.
    uint32 local = 0;
    uint32 slot = 0;
    TypeLease discovered;
    const TypeLease* actual = &discovered;
    const uint32* found = locals_.Find(target);
    if (!found && externalPolicy_ == GraphExternalPolicy::Admit) {
        if (auto spent = budget_.Spend(1); !spent) {
            return spent;
        }

        if (const uint32* imported = slots_.Find(target)) {
            slot = *imported;
        }
    }

    if (found || slot != 0) {
        // 캐시에 맞아도 참조마다 lease 없는 O(1) 생존 조회를 한다 — 참조마다 AcquireType을
        // 부르는 것과 같은 판정이다(무효·재사용 handle은 StaleHandle, 논리 파괴·cutoff는
        // Destroyed, 문맥·종료·재진입 오류는 그대로).
        auto lifecycle = registry_.InspectLifecycle(access_, target);
        if (!lifecycle) {
            return Result<void>::Failure(lifecycle.Error());
        }

        if (lifecycle.Value() != ObjectLifecycle::Alive) {
            return Result<void>::Failure(ErrorCode::Destroyed);
        }

        if (found) {
            local = *found;
            actual = &nodes_[local - 1].lease;
        } else {
            actual = &externalLeases_[slot - 1];
        }
    } else {
        auto acquired = registry_.AcquireType(access_, target);
        if (!acquired) {
            return Result<void>::Failure(acquired.Error());
        }

        discovered = Move(acquired.Value());
    }

    // Registry setter와 같은 native 상속 계약이다. 선언 타입의 파생 객체와 schema ID가 다른
    // 별명 등록도 허용한다. 정적 thunk의 대상 타입도 함께 만족해야 한다.
    if (expectation.staticNative && !actual->IsNativeA(expectation.staticNative)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if (expectation.schemaType != Guid{}) {
        auto expected = ExpectedLease(expectation.schemaType);
        if (!expected) {
            return Result<void>::Failure(expected.Error());
        }

        if (!actual->IsNativeA(expected.Value()->Ops().nativeType)) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }
    }

    if (local != 0) {
        return out.WriteU32(local);
    }

    if (slot != 0) {
        return WriteImported(out, slot);
    }

    switch (externalPolicy_) {
    case GraphExternalPolicy::Reject:
        break;
    case GraphExternalPolicy::Admit:
        return AdmitExternal(out, target, Move(discovered), expectation);
    case GraphExternalPolicy::Include: {
        auto added = IncludeNode(target, Move(discovered));
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        return out.WriteU32(added.Value());
    }
    }

    return RejectExternal();
}

// 기대 TypeId의 등록 타입 lease다(저장 끝까지 캐시에 쥔다). 조회·삽입마다 작업량 1이다.
Result<const TypeLease*> GraphEncodeContext::ExpectedLease(TypeId type) noexcept {
    if (auto spent = budget_.Spend(1); !spent) {
        return Result<const TypeLease*>::Failure(spent.Error());
    }

    if (const TypeLease* cached = expected_.Find(type)) {
        return Result<const TypeLease*>::Success(cached);
    }

    auto acquired = registry_.AcquireRegisteredType(access_, type);
    if (!acquired) {
        return Result<const TypeLease*>::Failure(acquired.Error());
    }

    if (auto spent = budget_.Spend(1); !spent) {
        return Result<const TypeLease*>::Failure(spent.Error());
    }

    if (auto inserted = expected_.Insert(type, Move(acquired.Value())); !inserted) {
        return Result<const TypeLease*>::Failure(inserted.Error());
    }

    return Result<const TypeLease*>::Success(expected_.Find(type));
}

} // namespace rocks::graph_detail

namespace rocks {
namespace {

using namespace graph_detail;

// 노드 하나의 필드를 encode해 Record를 만든다. fileBytes는 지금까지의 출력 byte 누계이고 이 노드의
// Bytes(Record)와(NodeTable이면) 노드 항목, 외부 참조 위치 표 메모리를 더해 maxBytes와 비교한다.
// 필드마다 외부 참조 위치를 필드 writer offset에서 Record offset으로 바꾼다.
Result<void> SaveNodeFields(Allocator& allocator, ObjectRegistry& registry,
                            const ObjectAccessContext& access, Array<SaveNode>& nodes,
                            usize nodeIndex, Budget& budget, usize& fileBytes, bool nodeTable,
                            GraphEncodeContext& context) noexcept {
    const ObjectHandle handle = nodes[nodeIndex].handle;
    // 이 노드의 필드를 encode하는 동안 owner를 고정한다(필드마다 잠그지 않는다). 저장 중 콜백이
    // Registry를 바꾸지 않는다는 계약을 어겨도, encode가 순회하는 owner 밖 버퍼(String·Soft 경로·
    // 컨테이너 원소)를 컨테이너·참조 변경 API와 등록된 필드 쓰기가 해제하지 못하고 Busy가 된다.
    // 실패·성공 모든 경로에서 범위를 벗어날 때 풀린다.
    auto pin = registry.PinForEncode(access, handle);
    if (!pin) {
        return Result<void>::Failure(pin.Error());
    }

    // 집합 노드는 encode 전에 배치를 검사했다. Include가 더한 노드는 여기서 검사한다(같은 판정을 두
    // 번 해도 결과는 같다).
    auto layout = ValidateLayout(nodes[nodeIndex].lease, budget);
    if (!layout) {
        return layout;
    }

    const auto& schema = nodes[nodeIndex].lease.Schema();
    // builder의 필드 수 한도는 이 객체 필드를 세기 전의 남은 누계다(로드와 같은 기준). 크기 한도는
    // 남은 파일 byte(이 노드의 Record 길이 prefix와 NodeTable 항목, 지금까지의 위치 표를 뺀 것)라
    // 넘칠 Record를 끝까지 만들지 않고, 노드 Record들의 합이 maxBytes로 묶인다(필드 writer도 같은
    // 한도로 연다).
    const usize entryBytes = nodeTable ? NodeEntryBytes + nodes[nodeIndex].nameBytes : 0;
    const usize firstPosition = context.PositionCount();
    usize reserved = 0;
    if (!TryAdd(fileBytes, RecordPrefixBytes, reserved) ||
        !TryAdd(reserved, entryBytes, reserved) ||
        !TryAdd(reserved, context.PositionBytes(), reserved)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    RecordLimits recordLimits = budget.Record();
    recordLimits.archive.maxTotalBytes =
        reserved < budget.limits.maxBytes ? budget.limits.maxBytes - reserved : 0;
    auto counted = budget.AddFields(schema.fields.Size());
    if (!counted) {
        return counted;
    }

    context.BeginObject(schema.id, static_cast<uint32>(nodeIndex), fileBytes);
    RecordBuilder record(allocator, schema.id, schema.version, recordLimits);
    usize payloadBytes = 0;
    for (usize fieldIndex = 0; fieldIndex < schema.fields.Size(); ++fieldIndex) {
        const auto& field = schema.fields[fieldIndex];
        // 최상위 필드의 declaringType은 객체 타입이다(상속 필드도 펼친 스키마로 다룬다). encode가
        // 노드를 더하면 nodes가 다시 할당될 수 있으므로 lease는 매번 nodes[nodeIndex]로 접근한다.
        const TypeLease& type = nodes[nodeIndex].lease;
        FieldScope scope(context, field, schema.id, type.FieldEnum(fieldIndex),
                         type.FieldStruct(fieldIndex));
        // Transient 참조는 GC 수명에는 참여하지만 저장 그래프에는 참여하지 않는다.
        if (context.IsExcluded(context.EffectiveFlags())) {
            continue;
        }

        auto accessor = FindOps(type, field.id, budget);
        if (!accessor) {
            return Result<void>::Failure(accessor.Error());
        }

        auto spent = budget.Spend(1);
        if (!spent) {
            return spent;
        }

        ArchiveLimits fieldLimits = budget.Archive();
        fieldLimits.maxTotalBytes = recordLimits.archive.maxTotalBytes;
        ArchiveWriter encoded(allocator, fieldLimits);
        // 이 필드의 외부 참조 위치는 [fieldPosition, 끝)이고 encoded 안 offset으로 기록된다.
        const usize fieldPosition = context.PositionCount();
        context.SetFieldWriter(&encoded);
        auto wire =
            accessor.Value()->encode(*accessor.Value(), registry, access, handle, encoded, context);
        context.SetFieldWriter(nullptr);
        if (!wire) {
            return Result<void>::Failure(wire.Error());
        }

        // codec이 스키마 필드의 wire(배열은 원소에 따라 ValueArray·RecordList)를 썼는지 다시
        // 확인한다.
        auto expected = detail::FieldWireType(field, context.ReferenceWireType());
        if (!expected || expected.Value() != wire.Value()) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        spent = budget.Spend(encoded.WorkUsed());
        if (!spent) {
            return spent;
        }

        usize appended = 0;
        if (!TryAdd(28, encoded.ByteCount(), appended)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        // builder의 실제 작업(복사, 성장할 때만 기존 byte 이동, id 중복 검사)을 청구한다.
        const uint64 builderBefore = record.WorkUsed();
        auto added = record.AddField(field.id, wire.Value(), encoded.View());
        if (!added) {
            return added;
        }

        spent = budget.Spend(record.WorkUsed() - builderBefore);
        if (!spent) {
            return spent;
        }

        // 필드 writer offset → Record offset(이 필드 payload의 Record 안 시작을 더한다).
        if (auto rebased = context.RebasePositions(fieldPosition, context.PositionCount(),
                                                   record.LastFieldPayloadOffset());
            !rebased) {
            return rebased;
        }

        if (!TryAdd(payloadBytes, appended, payloadBytes)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
    }

    // 이 노드가 출력에 더하는 byte: Bytes(Record) = 8 + 44 + payload, NodeTable이면 노드 항목.
    // 외부 참조 위치 표 메모리도 그래프 바이트 누계에 든다(5.3절).
    usize recordBytes = 0;
    usize total = 0;
    usize charged = 0;
    if (!TryAdd(payloadBytes, RecordHeaderBytes, recordBytes) ||
        !TryAdd(fileBytes, RecordPrefixBytes, total) || !TryAdd(total, recordBytes, total) ||
        !TryAdd(total, entryBytes, total) || !TryAdd(total, context.PositionBytes(), charged)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    if (charged > budget.limits.maxBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    auto spent = budget.Spend(static_cast<uint64>(recordBytes) + 1);
    if (!spent) {
        return spent;
    }

    auto output = record.Build();
    if (!output) {
        return Result<void>::Failure(output.Error());
    }

    // encode가 nodes를 재할당했을 수 있으므로 이전 원소 참조를 저장하지 않고 index로 다시 접근한다.
    nodes[nodeIndex].type = schema.id;
    nodes[nodeIndex].record = Move(output.Value());
    nodes[nodeIndex].firstPosition = firstPosition;
    fileBytes = total;
    return Result<void>::Success();
}

} // namespace

Result<ObjectGraphSaveResult> SaveObjectGraphSet(Allocator& allocator, ObjectRegistry& registry,
                                                 const ObjectAccessContext& access,
                                                 Span<const ObjectHandle> set,
                                                 ObjectGraphSaveOptions options) noexcept {
    using Output = Result<ObjectGraphSaveResult>;
    if (options.limits.maxObjects > MaxNodeCount ||
        options.limits.maxExternalReferences > MaxExternalCount) {
        return Output::Failure(ErrorCode::InvalidArgument);
    }

    if (set.Size() == 0) {
        return Output::Failure(ErrorCode::InvalidArgument);
    }

    for (ObjectHandle handle : set) {
        if (handle.IsNull()) {
            return Output::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (options.limits.maxObjects == 0 || set.Size() > options.limits.maxObjects ||
        options.limits.maxBytes < HeaderBytes) {
        return Output::Failure(ErrorCode::CapacityExceeded);
    }

    auto budget = Budget::Create(allocator, options.limits);
    if (!budget) {
        return Output::Failure(budget.Error());
    }

    Array<SaveNode> nodes(allocator);
    GraphEncodeContext context(allocator, registry, access, nodes, budget.Value(), options);
    if (auto added = context.AddSetNodes(set); !added) {
        return Output::Failure(added.Error());
    }

    // 첫 노드의 Borrow가 저장 끝까지 GC slice를 막는다.
    auto hold = registry.Resolve<Object>(access, set[0]);
    if (!hold) {
        return Output::Failure(hold.Error());
    }

    if (auto opened = context.OpenSetNodes(); !opened) {
        return Output::Failure(opened.Error());
    }

    for (usize index = 0; index < nodes.Size(); ++index) {
        if (auto layout = ValidateLayout(nodes[index].lease, budget.Value()); !layout) {
            return Output::Failure(layout.Error());
        }
    }

    if (auto prepared = context.PrepareInclude(); !prepared) {
        return Output::Failure(prepared.Error());
    }

    // Include는 encode 중에 노드를 끝에 더하므로 매번 크기를 다시 본다(더한 노드도 이 순서로 encode
    // 된다 — 외부 참조 위치 표가 노드 순서다).
    usize bytes = HeaderBytes;
    for (usize index = 0; index < nodes.Size(); ++index) {
        auto saved = SaveNodeFields(allocator, registry, access, nodes, index, budget.Value(),
                                    bytes, options.writeNodeTable, context);
        if (!saved) {
            return Output::Failure(saved.Error());
        }
    }

    Array<uint32> outers(allocator);
    bool baseOuter = false;
    if (options.writeNodeTable) {
        auto resolved = ResolveNodeTable(nodes, context, outers, budget.Value());
        if (!resolved) {
            return Output::Failure(resolved.Error());
        }

        baseOuter = resolved.Value();
    }

    auto output = WriteGraph(allocator, nodes, context, outers, baseOuter, budget.Value());
    if (!output) {
        return Output::Failure(output.Error());
    }

    // 마지막 output 할당 callback까지 포함하여 모든 노드와 외부 참조 대상의 논리 수명을 다시
    // 확인한 뒤 게시한다(결과의 handle이 게시 시점에 모두 생존 객체다).
    auto alive = [&](ObjectHandle handle) noexcept -> Result<void> {
        auto lifecycle = registry.InspectLifecycle(access, handle);
        if (!lifecycle) {
            return Result<void>::Failure(lifecycle.Error());
        }

        if (lifecycle.Value() != ObjectLifecycle::Alive) {
            return Result<void>::Failure(ErrorCode::Destroyed);
        }

        return Result<void>::Success();
    };

    Array<ObjectHandle> handles(allocator);
    if (auto reserved = handles.TryReserve(nodes.Size()); !reserved) {
        return Output::Failure(reserved.Error());
    }

    for (usize index = 0; index < nodes.Size(); ++index) {
        if (auto live = alive(nodes[index].handle); !live) {
            return Output::Failure(live.Error());
        }

        if (!handles.TryPushBack(nodes[index].handle)) {
            return Output::Failure(ErrorCode::OutOfMemory);
        }
    }

    for (const GraphExternal& external : context.Externals()) {
        if (auto live = alive(external.target); !live) {
            return Output::Failure(live.Error());
        }
    }

    return Output::Success(
        SaveResultAccess::Make(Move(output.Value()), Move(handles), Move(context.Externals()),
                               Move(context.Positions()), options.limits.maxExternalReferences));
}

Result<Array<uint8>> SaveObjectGraph(Allocator& allocator, ObjectRegistry& registry,
                                     const ObjectAccessContext& access, ObjectHandle root,
                                     ObjectGraphSaveOptions options) noexcept {
    options.externals = GraphExternalPolicy::Include;
    options.writeNodeTable = true;
    const ObjectHandle set[] = {root};
    auto saved = SaveObjectGraphSet(allocator, registry, access, set, Move(options));
    if (!saved) {
        return Result<Array<uint8>>::Failure(saved.Error());
    }

    return Result<Array<uint8>>::Success(saved.Value().TakeBytes());
}

} // namespace rocks
