#pragma once

#include <ObjectSerialization/ObjectGraph.h>

namespace rocks::graph_detail {

inline constexpr uint8 Magic[] = {'R', 'O', 'C', 'K', 'S', 'G', 'R', 'F'};
inline constexpr usize HeaderBytes = 20;
inline constexpr usize ObjectHeaderBytes = 36;

class Budget {
public:
    explicit Budget(ObjectGraphLimits value) noexcept : limits(value) {
    }

    Result<void> Spend(uint64 count) noexcept {
        if (count > limits.maxWork - used) {
            used = limits.maxWork;
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }
        used += count;
        return Result<void>::Success();
    }

    Result<void> AddFields(usize count) noexcept {
        if (count > limits.maxTotalFields - fields) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }
        fields += static_cast<uint32>(count);
        return Result<void>::Success();
    }

    ArchiveLimits Archive(bool outer = false) const noexcept {
        ArchiveLimits result;
        result.maxTotalBytes = limits.maxBytes;
        result.maxValueBytes = outer ? limits.maxBytes : limits.maxValueBytes;
        result.maxWork = limits.maxWork - used;
        result.maxOperations = result.maxWork;
        return result;
    }

    RecordLimits Record() const noexcept {
        return {Archive(), limits.maxTotalFields};
    }

    ObjectGraphLimits limits;
    uint64 used = 0;
    uint32 fields = 0;
    usize migrationBytes = 0;
};

Result<const RuntimeFieldOps*> FindOps(const TypeLease&, Guid, Budget&) noexcept;
Result<void> ValidateLayout(const TypeLease&, Budget&) noexcept;
Result<WireType> FieldWireType(ValueKind) noexcept;
Result<void> ValidateWidth(const FieldValue&) noexcept;
// enumSchema는 그 필드가 Enum이면 TypeLease::FieldEnum이 준 enum 스키마다(그 밖은 nullptr). Enum
// 값은 선언된 값(Flags는 선언 값의 OR)이 아니면 저장·복원 모두 InvalidArgument다.
Result<void> EncodeValue(ArchiveWriter&, const FieldValue&, uint64 localReference,
                         const EnumSchema* enumSchema) noexcept;
Result<FieldValue> DecodeValue(Allocator&, const FieldSchema&, const FieldView&, Budget&,
                               uint64& localReference, const EnumSchema* enumSchema) noexcept;

struct DecodedField {
    explicit DecodedField(Allocator& allocator) noexcept : value(allocator) {
    }

    usize schemaIndex = 0;
    uint64 localReference = 0;
    FieldValue value;
};

struct LoadNode {
    explicit LoadNode(Allocator& allocator) noexcept : migrated(allocator), values(allocator) {
    }

    TypeId type{};
    uint32 version = 0;
    Span<const uint8> source;
    TypeLease lease;
    Array<uint8> migrated;
    Array<DecodedField> values;
    ObjectHandle handle{};
};

} // namespace rocks::graph_detail
