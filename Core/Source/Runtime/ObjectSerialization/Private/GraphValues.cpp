#include "GraphInternal.h"

namespace rocks::graph_detail {

Result<const RuntimeFieldOps*> FindOps(const TypeLease& type, Guid id, Budget& budget) noexcept {
    const RuntimeFieldOps* found = nullptr;
    for (usize index = 0; index < type.Ops().fields.Size(); ++index) {
        const auto& field = type.Ops().fields[index];
        auto spent = budget.Spend(1);
        if (!spent) {
            return Result<const RuntimeFieldOps*>::Failure(spent.Error());
        }
        if (field.fieldId == id) {
            if (found != nullptr) {
                return Result<const RuntimeFieldOps*>::Failure(ErrorCode::Unsupported);
            }
            found = &field;
        }
    }
    if (found == nullptr || found->read == nullptr || found->writeStaged == nullptr) {
        return Result<const RuntimeFieldOps*>::Failure(ErrorCode::Unsupported);
    }
    return Result<const RuntimeFieldOps*>::Success(found);
}

Result<WireType> FieldWireType(ValueKind kind) noexcept {
    switch (kind) {
    case ValueKind::Bool:
        return Result<WireType>::Success(WireType::Bool);
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
        return Result<WireType>::Success(WireType::U32);
    case ValueKind::UInt64:
    case ValueKind::ObjectReference:
        return Result<WireType>::Success(WireType::U64);
    case ValueKind::Int32:
    case ValueKind::Int64:
        return Result<WireType>::Success(WireType::I64);
    case ValueKind::Float32:
        return Result<WireType>::Success(WireType::F32);
    case ValueKind::Float64:
        return Result<WireType>::Success(WireType::F64);
    case ValueKind::Guid:
        return Result<WireType>::Success(WireType::Guid);
    case ValueKind::Utf8String:
        return Result<WireType>::Success(WireType::String);
    case ValueKind::Opaque:
        return Result<WireType>::Failure(ErrorCode::Unsupported);
    }
    return Result<WireType>::Failure(ErrorCode::Unsupported);
}

Result<void> ValidateLayout(const TypeLease& type, Budget& budget) noexcept {
    const auto& schema = type.Schema();
    const auto& ops = type.Ops();
    if (!ops.completeFieldLayout || schema.fields.Size() != ops.fields.Size()) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }
    if (schema.fields.Size() > budget.limits.maxTotalFields) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        const auto& field = schema.fields[index];
        auto wire = FieldWireType(field.valueKind);
        if (!wire) {
            return Result<void>::Failure(wire.Error());
        }
        const bool reference = field.valueKind == ValueKind::ObjectReference;
        if ((reference && field.kind != FieldKind::ObjectReference) ||
            (!reference && (field.kind != FieldKind::Value || field.referencedType != Guid{}))) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }
        auto accessor = FindOps(type, field.id, budget);
        if (!accessor) {
            return Result<void>::Failure(accessor.Error());
        }
    }
    return Result<void>::Success();
}

Result<void> ValidateWidth(const FieldValue& value) noexcept {
    if ((value.kind == ValueKind::UInt8 && value.unsignedInteger > 0xffu) ||
        (value.kind == ValueKind::UInt16 && value.unsignedInteger > 0xffffu) ||
        (value.kind == ValueKind::UInt32 && value.unsignedInteger > 0xffffffffu) ||
        (value.kind == ValueKind::Int32 &&
         (value.signedInteger < -2147483648LL || value.signedInteger > 2147483647LL))) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }
    return Result<void>::Success();
}

Result<void> EncodeValue(ArchiveWriter& writer, const FieldValue& value, uint64 local) noexcept {
    auto width = ValidateWidth(value);
    if (!width) {
        return width;
    }
    switch (value.kind) {
    case ValueKind::Bool:
        return writer.WriteBool(value.boolean);
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
        return writer.WriteU32(static_cast<uint32>(value.unsignedInteger));
    case ValueKind::UInt64:
        return writer.WriteU64(value.unsignedInteger);
    case ValueKind::Int32:
    case ValueKind::Int64:
        return writer.WriteI64(value.signedInteger);
    case ValueKind::Float32:
        return writer.WriteF32(value.float32);
    case ValueKind::Float64:
        return writer.WriteF64(value.float64);
    case ValueKind::Guid:
        return writer.WriteGuid(value.guid);
    case ValueKind::Utf8String:
        return writer.WriteString(value.string.View());
    case ValueKind::ObjectReference:
        return writer.WriteU64(local);
    case ValueKind::Opaque:
        return Result<void>::Failure(ErrorCode::Unsupported);
    }
    return Result<void>::Failure(ErrorCode::Unsupported);
}

Result<FieldValue> DecodeValue(Allocator& allocator, const FieldSchema& schema,
                               const FieldView& field, Budget& budget, uint64& local) noexcept {
    auto expected = FieldWireType(schema.valueKind);
    if (!expected) {
        return Result<FieldValue>::Failure(expected.Error());
    }
    if (field.wireType != static_cast<uint32>(expected.Value())) {
        return Result<FieldValue>::Failure(ErrorCode::TypeMismatch);
    }
    auto reader = ArchiveReader::Create(field.payload, budget.Archive());
    if (!reader) {
        return Result<FieldValue>::Failure(reader.Error());
    }
    auto& archive = reader.Value();
    FieldValue value(allocator);
    value.kind = schema.valueKind;
    uint64 localCandidate = 0;
    switch (schema.valueKind) {
    case ValueKind::Bool: {
        auto decoded = archive.ReadBool();
        if (!decoded) {
            return Result<FieldValue>::Failure(decoded.Error());
        }
        value.boolean = decoded.Value();
        break;
    }
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32: {
        auto decoded = archive.ReadU32();
        if (!decoded) {
            return Result<FieldValue>::Failure(decoded.Error());
        }
        value.unsignedInteger = decoded.Value();
        break;
    }
    case ValueKind::UInt64:
    case ValueKind::ObjectReference: {
        auto decoded = archive.ReadU64();
        if (!decoded) {
            return Result<FieldValue>::Failure(decoded.Error());
        }
        if (schema.valueKind == ValueKind::ObjectReference) {
            localCandidate = decoded.Value();
        } else {
            value.unsignedInteger = decoded.Value();
        }
        break;
    }
    case ValueKind::Int32:
    case ValueKind::Int64: {
        auto decoded = archive.ReadI64();
        if (!decoded) {
            return Result<FieldValue>::Failure(decoded.Error());
        }
        value.signedInteger = decoded.Value();
        break;
    }
    case ValueKind::Float32: {
        auto decoded = archive.ReadF32();
        if (!decoded) {
            return Result<FieldValue>::Failure(decoded.Error());
        }
        value.float32 = decoded.Value();
        break;
    }
    case ValueKind::Float64: {
        auto decoded = archive.ReadF64();
        if (!decoded) {
            return Result<FieldValue>::Failure(decoded.Error());
        }
        value.float64 = decoded.Value();
        break;
    }
    case ValueKind::Guid: {
        auto decoded = archive.ReadGuid();
        if (!decoded) {
            return Result<FieldValue>::Failure(decoded.Error());
        }
        value.guid = decoded.Value();
        break;
    }
    case ValueKind::Utf8String: {
        auto decoded = archive.ReadString(allocator);
        if (!decoded) {
            return Result<FieldValue>::Failure(decoded.Error());
        }
        value.string = Move(decoded.Value());
        break;
    }
    case ValueKind::Opaque:
        return Result<FieldValue>::Failure(ErrorCode::Unsupported);
    }
    if (!archive.AtEnd()) {
        return Result<FieldValue>::Failure(ErrorCode::InvalidArgument);
    }
    auto width = ValidateWidth(value);
    if (!width) {
        return Result<FieldValue>::Failure(width.Error());
    }
    auto spent = budget.Spend(archive.WorkUsed());
    if (!spent) {
        return Result<FieldValue>::Failure(spent.Error());
    }
    local = localCandidate;
    return Result<FieldValue>::Success(Move(value));
}

} // namespace rocks::graph_detail
