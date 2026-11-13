#include "GraphInternal.h"

namespace rocks {
namespace {

using namespace graph_detail;

// 저장할 노드 하나다. lease는 발견할 때 연 대상 타입 lease(root는 SaveNodeFields가 연다)이며
// 참조 검사와 SaveNodeFields가 재사용한다. nodes가 다시 할당될 수 있으므로 lease·노드의 주소를
// 보관하지 않고 index로 다시 접근한다(lease가 돌려준 스키마·필드 정보는 레지스트리 레코드 안이라
// 안정적이다).
struct SaveNode {
    SaveNode(Allocator& allocator, ObjectHandle value) noexcept : handle(value), record(allocator) {
    }

    ObjectHandle handle;
    TypeLease lease;
    TypeId type{};
    uint32 version = 0;
    Array<uint8> record;
};

// graph v1 저장 문맥이다: 강한 참조는 대상 타입을 검사한 뒤 BFS local ID(U64)로 쓴다. nodes가 다시
// 할당될 수 있으므로 원소 참조를 보관하지 않는다. 참조 비용은 참조 수에 선형이다(5.6절): handle →
// local ID 해시 표(키는 domain을 포함한 handle 전체 — 캐시 적중이 domain 검사를 건너뛰지 않게),
// 노드별 TypeLease(발견 때 한 번), 기대 TypeId → lease 캐시를 쓰고 조회·삽입마다 작업량 1을
// 청구한다. BFS 순서와 byte는 선형 탐색 때와 같다.
class GraphEncodeContext final : public FieldEncodeContext {
public:
    GraphEncodeContext(Allocator& allocator, ObjectRegistry& registry,
                       const ObjectAccessContext& access, Array<SaveNode>& nodes, Budget& budget,
                       const ObjectGraphSaveOptions& options) noexcept
        : FieldEncodeContext(options.policy, options.hooks, ReferenceWire), allocator_(allocator),
          registry_(registry), access_(access), nodes_(nodes), budget_(budget),
          scratch_(allocator, budget), observation_(allocator, budget, false), locals_(allocator),
          expected_(allocator) {
    }

    void BeginObject(TypeId type, uint32 index) noexcept {
        SetObject(type, index);
    }

    // root를 첫 노드(local ID 1)로 더한다. lease는 SaveNodeFields가 고정 뒤에 연다.
    Result<void> AddRoot(ObjectHandle root) noexcept {
        auto added = AddNode(root, TypeLease());
        return added ? Result<void>::Success() : Result<void>::Failure(added.Error());
    }

    Result<void> Spend(uint64 work) noexcept override {
        return budget_.Spend(work);
    }

    Result<void> ReserveApply(uint64) noexcept override {
        return Result<void>::Success();
    }

    Allocator& Scratch() noexcept override {
        return scratch_;
    }

    // 저장은 관찰하지 않는다(계약상 부르지 않는다 — 공통 가상 구현).
    Allocator& ObservationScratch() noexcept override {
        return observation_;
    }

    Result<void> AddFields(usize count) noexcept override {
        return budget_.AddFields(count);
    }

    Result<void> AddElements(usize count) noexcept override {
        return budget_.AddElements(count);
    }

    // 중첩 Record는 필드 값 하나라 전체 크기도 값 한도 안이어야 한다(배열 payload의 상한도 이
    // 값이다).
    RecordLimits NestedRecordLimits() const noexcept override {
        RecordLimits limits = budget_.Record();
        limits.archive.maxTotalBytes = budget_.limits.maxValueBytes;
        return limits;
    }

protected:
    Result<void> WriteReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                    const ReferenceExpectation& expectation) noexcept override {
        if (target == ObjectHandle{}) {
            return out.WriteU64(0);
        }

        if (target.IsNull()) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        // 이미 발견한 노드면 그 lease로 검사하고, 처음이면 대상 타입을 열어(생존·문맥 검사 포함)
        // 검사가 끝난 뒤 노드로 더한다.
        if (auto spent = budget_.Spend(1); !spent) {
            return spent;
        }

        uint64 local = 0;
        TypeLease discovered;
        if (const uint64* found = locals_.Find(target)) {
            // 캐시에 맞아도 참조마다 lease 없는 O(1) 생존 조회를 한다 — 참조마다 AcquireType을
            // 부르던 때와 같은 판정이다(무효·재사용 handle은 StaleHandle, 논리 파괴·cutoff는
            // Destroyed, 문맥·종료·재진입 오류는 그대로).
            auto lifecycle = registry_.InspectLifecycle(access_, target);
            if (!lifecycle) {
                return Result<void>::Failure(lifecycle.Error());
            }

            if (lifecycle.Value() != ObjectLifecycle::Alive) {
                return Result<void>::Failure(ErrorCode::Destroyed);
            }

            local = *found;
        } else {
            auto actual = registry_.AcquireType(access_, target);
            if (!actual) {
                return Result<void>::Failure(actual.Error());
            }

            discovered = Move(actual.Value());
        }

        // root의 lease는 SaveNodeFields가 열 때까지 비어 있다(root를 가리키는 참조는 root의 필드를
        // 저장하는 동안에만 나온다 — 그때는 열려 있다). 비어 있으면 다시 연다.
        TypeLease reopened;
        const TypeLease* actual = local == 0 ? &discovered : &nodes_[local - 1].lease;
        if (!actual->IsValid()) {
            auto type = registry_.AcquireType(access_, target);
            if (!type) {
                return Result<void>::Failure(type.Error());
            }

            reopened = Move(type.Value());
            actual = &reopened;
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

        if (local == 0) {
            auto added = AddNode(target, Move(discovered));
            if (!added) {
                return Result<void>::Failure(added.Error());
            }

            local = added.Value();
        }

        return out.WriteU64(local);
    }

private:
    // 새 노드를 BFS 끝에 더하고 local ID를 돌려준다(maxObjects를 넘으면 CapacityExceeded).
    Result<uint64> AddNode(ObjectHandle handle, TypeLease lease) noexcept {
        if (nodes_.Size() >= budget_.limits.maxObjects) {
            return Result<uint64>::Failure(ErrorCode::CapacityExceeded);
        }

        // 노드 배열 성장(기존 원소 이동)과 해시 표 삽입을 청구한다.
        auto spent = budget_.Spend(2 + (nodes_.Size() == nodes_.Capacity() ? nodes_.Size() : 0));
        if (!spent) {
            return Result<uint64>::Failure(spent.Error());
        }

        // 노드를 먼저 더하고 맵 삽입이 실패하면 되돌린다(맵의 local ID는 언제나 nodes 안이다).
        auto added = nodes_.TryEmplaceBack(allocator_, handle);
        if (!added) {
            return Result<uint64>::Failure(added.Error());
        }

        added.Value()->lease = Move(lease);
        const uint64 local = static_cast<uint64>(nodes_.Size());
        if (auto inserted = locals_.Insert(handle, local); !inserted) {
            (void)nodes_.TryPopBack();
            return Result<uint64>::Failure(inserted.Error());
        }

        return Result<uint64>::Success(local);
    }

    // 기대 TypeId의 등록 타입 lease다(저장 끝까지 캐시에 쥔다). 조회·삽입마다 작업량 1이다.
    Result<const TypeLease*> ExpectedLease(TypeId type) noexcept {
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

    Allocator& allocator_;
    ObjectRegistry& registry_;
    const ObjectAccessContext& access_;
    Array<SaveNode>& nodes_;
    Budget& budget_;
    BudgetAllocator scratch_;
    BudgetAllocator observation_;
    HashMap<ObjectHandle, uint64, HandleHash> locals_;
    HashMap<TypeId, TypeLease> expected_;
};

Result<void> SaveNodeFields(Allocator& allocator, ObjectRegistry& registry,
                            const ObjectAccessContext& access, Array<SaveNode>& nodes,
                            usize nodeIndex, Budget& budget, usize& fileBytes,
                            GraphEncodeContext& context) noexcept {
    const ObjectHandle handle = nodes[nodeIndex].handle;
    // 이 노드의 필드를 encode하는 동안 owner를 고정한다(필드마다 잠그지 않는다). 저장 중 콜백이
    // Registry를 바꾸지 않는다는 계약을 어겨도, encode가 순회하는 owner 밖 버퍼(String·Soft 경로·
    // 컨테이너 원소)를 컨테이너·참조 변경 API와 등록된 필드 쓰기가 해제하지 못하고 Busy가 된다.
    // 생존 판정은 AcquireType과 같다(먼저 실패하면 같은 오류다). 실패·성공 모든 경로에서 범위를
    // 벗어날 때 풀린다.
    auto pin = registry.PinForEncode(access, handle);
    if (!pin) {
        return Result<void>::Failure(pin.Error());
    }

    // 발견할 때 연 노드 lease를 재사용한다(root는 여기서 연다). encode가 노드를 더하면 nodes가 다시
    // 할당될 수 있으므로 lease는 매번 nodes[nodeIndex]로 접근한다.
    if (!nodes[nodeIndex].lease.IsValid()) {
        auto type = registry.AcquireType(access, handle);
        if (!type) {
            return Result<void>::Failure(type.Error());
        }

        nodes[nodeIndex].lease = Move(type.Value());
    }

    auto layout = ValidateLayout(nodes[nodeIndex].lease, budget);
    if (!layout) {
        return layout;
    }

    const auto& schema = nodes[nodeIndex].lease.Schema();
    // builder의 필드 수 한도는 이 객체 필드를 세기 전의 남은 누계다(로드와 같은 기준).
    const RecordLimits recordLimits = budget.Record();
    auto counted = budget.AddFields(schema.fields.Size());
    if (!counted) {
        return counted;
    }

    context.BeginObject(schema.id, static_cast<uint32>(nodeIndex));
    RecordBuilder record(allocator, schema.id, schema.version, recordLimits);
    usize payloadBytes = 0;
    for (usize fieldIndex = 0; fieldIndex < schema.fields.Size(); ++fieldIndex) {
        const auto& field = schema.fields[fieldIndex];
        // 최상위 필드의 declaringType은 객체 타입이다(상속 필드도 펼친 스키마로 다룬다).
        const TypeLease& type = nodes[nodeIndex].lease;
        FieldScope scope(context, field, schema.id, type.FieldEnum(fieldIndex),
                         type.FieldStruct(fieldIndex));
        // Transient 참조는 GC 수명에는 참여하지만 저장 그래프의 도달 가능성에는 참여하지 않는다.
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

        ArchiveWriter encoded(allocator, budget.Archive());
        auto wire =
            accessor.Value()->encode(*accessor.Value(), registry, access, handle, encoded, context);
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

        if (!TryAdd(payloadBytes, appended, payloadBytes)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
    }

    usize recordBytes = 0;
    usize total = 0;
    if (!TryAdd(payloadBytes, 44, recordBytes) || !TryAdd(fileBytes, ObjectHeaderBytes, total) ||
        !TryAdd(total, recordBytes, total)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    if (total > budget.limits.maxBytes) {
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

    // BFS가 nodes를 재할당했을 수 있으므로 이전 원소 참조를 저장하지 않고 index로 다시 접근한다.
    nodes[nodeIndex].type = schema.id;
    nodes[nodeIndex].version = schema.version;
    nodes[nodeIndex].record = Move(output.Value());
    fileBytes = total;
    return Result<void>::Success();
}

Result<Array<uint8>> WriteGraph(Allocator& allocator, const Array<SaveNode>& nodes,
                                Budget& budget) noexcept {
    ArchiveWriter output(allocator, budget.Archive(true));
    auto written = output.WriteRaw(Magic);
    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }

    written = output.WriteU32(1);
    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }

    written = output.WriteU32(static_cast<uint32>(nodes.Size()));
    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }

    written = output.WriteU32(1);
    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }

    for (usize index = 0; index < nodes.Size(); ++index) {
        written = output.WriteU64(static_cast<uint64>(index + 1));
        if (!written) {
            return Result<Array<uint8>>::Failure(written.Error());
        }

        written = output.WriteGuid(nodes[index].type);
        if (!written) {
            return Result<Array<uint8>>::Failure(written.Error());
        }

        written = output.WriteU32(nodes[index].version);
        if (!written) {
            return Result<Array<uint8>>::Failure(written.Error());
        }

        written = output.WriteBytes(nodes[index].record.View());
        if (!written) {
            return Result<Array<uint8>>::Failure(written.Error());
        }
    }

    auto spent = budget.Spend(output.WorkUsed());
    if (!spent) {
        return Result<Array<uint8>>::Failure(spent.Error());
    }

    return Result<Array<uint8>>::Success(output.TakeBytes());
}

} // namespace

Result<Array<uint8>> SaveObjectGraph(Allocator& allocator, ObjectRegistry& registry,
                                     const ObjectAccessContext& access, ObjectHandle root,
                                     ObjectGraphSaveOptions options) noexcept {
    if (root.IsNull()) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    if (options.limits.maxObjects == 0 || options.limits.maxBytes < HeaderBytes) {
        return Result<Array<uint8>>::Failure(ErrorCode::CapacityExceeded);
    }

    auto hold = registry.Resolve<Object>(access, root);
    if (!hold) {
        return Result<Array<uint8>>::Failure(hold.Error());
    }

    auto budget = Budget::Create(allocator, options.limits);
    if (!budget) {
        return Result<Array<uint8>>::Failure(budget.Error());
    }

    Array<SaveNode> nodes(allocator);
    GraphEncodeContext context(allocator, registry, access, nodes, budget.Value(), options);
    if (auto added = context.AddRoot(root); !added) {
        return Result<Array<uint8>>::Failure(added.Error());
    }

    usize bytes = HeaderBytes;
    for (usize index = 0; index < nodes.Size(); ++index) {
        auto saved = SaveNodeFields(allocator, registry, access, nodes, index, budget.Value(),
                                    bytes, context);
        if (!saved) {
            return Result<Array<uint8>>::Failure(saved.Error());
        }
    }

    auto output = WriteGraph(allocator, nodes, budget.Value());
    if (!output) {
        return output;
    }

    // 마지막 output 할당 callback까지 포함하여 root의 논리 수명을 다시 확인한 뒤 게시한다.
    auto valid = hold.Value().Get();
    if (!valid) {
        return Result<Array<uint8>>::Failure(valid.Error());
    }

    return output;
}

} // namespace rocks
