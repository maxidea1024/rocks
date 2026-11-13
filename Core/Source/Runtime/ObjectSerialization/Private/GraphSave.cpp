#include "GraphInternal.h"

namespace rocks {
namespace {

using namespace graph_detail;

struct SaveNode {
    SaveNode(Allocator& allocator, ObjectHandle value) noexcept : handle(value), record(allocator) {
    }

    ObjectHandle handle;
    TypeId type{};
    uint32 version = 0;
    Array<uint8> record;
};

Result<uint64> AddHandle(Allocator& allocator, Array<SaveNode>& nodes, ObjectHandle handle,
                         Budget& budget) noexcept {
    if (handle == ObjectHandle{}) {
        return Result<uint64>::Success(0);
    }

    if (handle.IsNull()) {
        return Result<uint64>::Failure(ErrorCode::StaleHandle);
    }

    for (usize index = 0; index < nodes.Size(); ++index) {
        auto spent = budget.Spend(1);
        if (!spent) {
            return Result<uint64>::Failure(spent.Error());
        }

        if (nodes[index].handle == handle) {
            return Result<uint64>::Success(static_cast<uint64>(index + 1));
        }
    }

    if (nodes.Size() >= budget.limits.maxObjects) {
        return Result<uint64>::Failure(ErrorCode::CapacityExceeded);
    }

    auto spent = budget.Spend(1 + (nodes.Size() == nodes.Capacity() ? nodes.Size() : 0));
    if (!spent) {
        return Result<uint64>::Failure(spent.Error());
    }

    auto added = nodes.TryEmplaceBack(allocator, handle);
    if (!added) {
        return Result<uint64>::Failure(added.Error());
    }

    return Result<uint64>::Success(static_cast<uint64>(nodes.Size()));
}

// graph v1 저장 문맥이다: 강한 참조는 대상 타입을 검사한 뒤 BFS local ID(U64)로 쓴다. nodes가 다시
// 할당될 수 있으므로 원소 참조를 보관하지 않는다.
class GraphEncodeContext final : public FieldEncodeContext {
public:
    GraphEncodeContext(Allocator& allocator, ObjectRegistry& registry,
                       const ObjectAccessContext& access, Array<SaveNode>& nodes, Budget& budget,
                       const ObjectGraphSaveOptions& options) noexcept
        : FieldEncodeContext(options.policy, options.hooks, ReferenceWire), allocator_(allocator),
          registry_(registry), access_(access), nodes_(nodes), budget_(budget),
          scratch_(allocator, budget) {
    }

    void BeginObject(TypeId type, uint32 index) noexcept {
        SetObject(type, index);
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

protected:
    Result<void> WriteReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                    const ReferenceExpectation& expectation) noexcept override {
        if (target != ObjectHandle{} && !target.IsNull()) {
            auto actual = registry_.AcquireType(access_, target);
            if (!actual) {
                return Result<void>::Failure(actual.Error());
            }

            // Registry setter와 같은 native 상속 계약이다. 선언 타입의 파생 객체와 schema ID가 다른
            // 별명 등록도 허용한다. 정적 thunk의 대상 타입도 함께 만족해야 한다.
            if (expectation.staticNative && !actual.Value().IsNativeA(expectation.staticNative)) {
                return Result<void>::Failure(ErrorCode::TypeMismatch);
            }

            if (expectation.schemaType != Guid{}) {
                auto expected = registry_.AcquireRegisteredType(access_, expectation.schemaType);
                if (!expected) {
                    return Result<void>::Failure(expected.Error());
                }

                if (!actual.Value().IsNativeA(expected.Value().Ops().nativeType)) {
                    return Result<void>::Failure(ErrorCode::TypeMismatch);
                }
            }
        }

        auto local = AddHandle(allocator_, nodes_, target, budget_);
        if (!local) {
            return Result<void>::Failure(local.Error());
        }

        return out.WriteU64(local.Value());
    }

private:
    Allocator& allocator_;
    ObjectRegistry& registry_;
    const ObjectAccessContext& access_;
    Array<SaveNode>& nodes_;
    Budget& budget_;
    BudgetAllocator scratch_;
};

Result<void> SaveNodeFields(Allocator& allocator, ObjectRegistry& registry,
                            const ObjectAccessContext& access, Array<SaveNode>& nodes,
                            usize nodeIndex, Budget& budget, usize& fileBytes,
                            GraphEncodeContext& context) noexcept {
    const ObjectHandle handle = nodes[nodeIndex].handle;
    auto type = registry.AcquireType(access, handle);
    if (!type) {
        return Result<void>::Failure(type.Error());
    }

    auto layout = ValidateLayout(type.Value(), budget);
    if (!layout) {
        return layout;
    }

    const auto& schema = type.Value().Schema();
    auto counted = budget.AddFields(schema.fields.Size());
    if (!counted) {
        return counted;
    }

    context.BeginObject(schema.id, static_cast<uint32>(nodeIndex));
    RecordBuilder record(allocator, schema.id, schema.version, budget.Record());
    usize payloadBytes = 0;
    for (usize fieldIndex = 0; fieldIndex < schema.fields.Size(); ++fieldIndex) {
        const auto& field = schema.fields[fieldIndex];
        // 최상위 필드의 declaringType은 객체 타입이다(상속 필드도 펼친 스키마로 다룬다).
        FieldScope scope(context, field, schema.id, type.Value().FieldEnum(fieldIndex));
        // Transient 참조는 GC 수명에는 참여하지만 저장 그래프의 도달 가능성에는 참여하지 않는다.
        if (context.IsExcluded(context.EffectiveFlags())) {
            continue;
        }

        auto accessor = FindOps(type.Value(), field.id, budget);
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

        // codec이 스키마 값 종류의 wire를 썼는지 다시 확인한다.
        auto expected = detail::FieldWireType(field.value.kind, context.ReferenceWireType());
        if (!expected || expected.Value() != wire.Value()) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        spent = budget.Spend(encoded.WorkUsed());
        if (!spent) {
            return spent;
        }

        usize appended = 0;
        usize work = 0;
        if (!TryAdd(28, encoded.ByteCount(), appended) || !TryAdd(appended, payloadBytes, work) ||
            !TryAdd(work, 1, work) || !TryAdd(work, fieldIndex, work) ||
            !TryAdd(work, fieldIndex, work)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        spent = budget.Spend(work);
        if (!spent) {
            return spent;
        }

        auto added = record.AddField(field.id, wire.Value(), encoded.View());
        if (!added) {
            return added;
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
    auto added = AddHandle(allocator, nodes, root, budget.Value());
    if (!added) {
        return Result<Array<uint8>>::Failure(added.Error());
    }

    GraphEncodeContext context(allocator, registry, access, nodes, budget.Value(), options);
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
