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

// getter가 String을 복제하기 전에 한도를 검사한다. 이 adapter를 빌리는 FieldValue는 이 scope에서
// 먼저 파괴하며 반환 output/SaveNode에 adapter 주소가 탈출하지 않는다.
class FieldAllocator final : public Allocator {
public:
    FieldAllocator(Allocator& allocator, Budget& budget) noexcept
        : allocator_(allocator), budget_(budget) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        const usize maximum = budget_.limits.maxValueBytes;
        if (bytes != 0 && bytes - 1 > maximum) {
            return Result<void*>::Failure(ErrorCode::CapacityExceeded);
        }
        auto spent = budget_.Spend(bytes);
        if (!spent) {
            return Result<void*>::Failure(spent.Error());
        }
        return allocator_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* value) noexcept override {
        return allocator_.Deallocate(value);
    }

private:
    Allocator& allocator_;
    Budget& budget_;
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

Result<void> SaveNodeFields(Allocator& allocator, ObjectRegistry& registry,
                            const ObjectAccessContext& access, Array<SaveNode>& nodes,
                            usize nodeIndex, Budget& budget, usize& fileBytes) noexcept {
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
    RecordBuilder record(allocator, schema.id, schema.version, budget.Record());
    usize payloadBytes = 0;
    for (usize fieldIndex = 0; fieldIndex < schema.fields.Size(); ++fieldIndex) {
        const auto& field = schema.fields[fieldIndex];
        auto accessor = FindOps(type.Value(), field.id, budget);
        if (!accessor) {
            return Result<void>::Failure(accessor.Error());
        }
        auto spent = budget.Spend(1);
        if (!spent) {
            return spent;
        }
        FieldAllocator fieldAllocator(allocator, budget);
        FieldValue value(fieldAllocator);
        auto read = accessor.Value()->read(registry, access, handle, value);
        if (!read) {
            return read;
        }
        if (value.kind != field.valueKind) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }
        uint64 local = 0;
        if (value.kind == ValueKind::ObjectReference) {
            if (value.reference != ObjectHandle{}) {
                auto target = registry.AcquireType(access, value.reference);
                if (!target) {
                    return Result<void>::Failure(target.Error());
                }
                if (field.referencedType != Guid{}) {
                    auto expected = registry.AcquireRegisteredType(access, field.referencedType);
                    if (!expected) {
                        return Result<void>::Failure(expected.Error());
                    }
                    // Registry setter와 같은 native 계약이다. schema ID가 다른 별명 등록도
                    // 허용한다.
                    if (expected.Value().Ops().nativeType != target.Value().Ops().nativeType) {
                        return Result<void>::Failure(ErrorCode::TypeMismatch);
                    }
                }
            }
            auto referenced = AddHandle(allocator, nodes, value.reference, budget);
            if (!referenced) {
                return Result<void>::Failure(referenced.Error());
            }
            local = referenced.Value();
        }
        ArchiveWriter encoded(allocator, budget.Archive());
        auto written = EncodeValue(encoded, value, local);
        if (!written) {
            return written;
        }
        spent = budget.Spend(encoded.WorkUsed());
        if (!spent) {
            return spent;
        }
        auto wire = FieldWireType(field.valueKind);
        if (!wire) {
            return Result<void>::Failure(wire.Error());
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
                                     ObjectGraphLimits limits) noexcept {
    if (root.IsNull()) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }
    if (limits.maxObjects == 0 || limits.maxBytes < HeaderBytes) {
        return Result<Array<uint8>>::Failure(ErrorCode::CapacityExceeded);
    }
    auto hold = registry.Resolve<Object>(access, root);
    if (!hold) {
        return Result<Array<uint8>>::Failure(hold.Error());
    }
    Budget budget(limits);
    Array<SaveNode> nodes(allocator);
    auto added = AddHandle(allocator, nodes, root, budget);
    if (!added) {
        return Result<Array<uint8>>::Failure(added.Error());
    }
    usize bytes = HeaderBytes;
    for (usize index = 0; index < nodes.Size(); ++index) {
        auto saved = SaveNodeFields(allocator, registry, access, nodes, index, budget, bytes);
        if (!saved) {
            return Result<Array<uint8>>::Failure(saved.Error());
        }
    }
    auto output = WriteGraph(allocator, nodes, budget);
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
