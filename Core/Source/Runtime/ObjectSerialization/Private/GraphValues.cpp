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

    if (found == nullptr || found->encode == nullptr || found->decode == nullptr) {
        return Result<const RuntimeFieldOps*>::Failure(ErrorCode::Unsupported);
    }

    return Result<const RuntimeFieldOps*>::Success(found);
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
        // 필드 단위 wire(배열은 원소에 따라 ValueArray·RecordList)가 있어야 한다.
        auto wire = detail::FieldWireType(field, ReferenceWire);
        if (!wire) {
            return Result<void>::Failure(wire.Error());
        }

        // 대상 형식(배열이면 원소)으로 FieldKind·type 규칙과 등록 enum·struct를 확인한다. 배열의
        // value.type은 0이다.
        const ValueType& target = FieldTargetType(field);
        const bool reference = target.kind == ValueKind::ObjectReference;
        const bool typed = target.kind == ValueKind::SoftObjectReference ||
                           target.kind == ValueKind::Enum || target.kind == ValueKind::Struct;
        if ((field.value.kind == ValueKind::Array && field.value.type != Guid{}) ||
            (reference && field.kind != FieldKind::ObjectReference) ||
            (!reference && (field.kind != FieldKind::Value || (!typed && target.type != Guid{}))) ||
            (target.kind == ValueKind::Enum && type.FieldEnum(index) == nullptr) ||
            (target.kind == ValueKind::Struct && type.FieldStruct(index).IsEmpty())) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        // Transient 필드는 저장·복원하지 않으므로 codec을 요구하지 않는다(등록 규칙과 같다).
        // 함수표의 GUID 1:1은 등록이 강제한다.
        if (HasFlag(field.flags, FieldFlags::Transient)) {
            continue;
        }

        auto accessor = FindOps(type, field.id, budget);
        if (!accessor) {
            return Result<void>::Failure(accessor.Error());
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::graph_detail
