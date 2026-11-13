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
        auto wire = detail::FieldWireType(field.value.kind, ReferenceWire);
        if (!wire) {
            return Result<void>::Failure(wire.Error());
        }

        const bool reference = field.value.kind == ValueKind::ObjectReference;
        const bool typed = field.value.kind == ValueKind::SoftObjectReference ||
                           field.value.kind == ValueKind::Enum ||
                           field.value.kind == ValueKind::Struct;
        if ((reference && field.kind != FieldKind::ObjectReference) ||
            (!reference &&
             (field.kind != FieldKind::Value || (!typed && field.value.type != Guid{}))) ||
            (field.value.kind == ValueKind::Enum && type.FieldEnum(index) == nullptr) ||
            (field.value.kind == ValueKind::Struct && type.FieldStruct(index).IsEmpty())) {
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
