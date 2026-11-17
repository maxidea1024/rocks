#include "TextAssignApplyInternal.h"

// 참조를 담은 struct 자리의 `{}` 분해다(리플렉션 B9i3c3).
namespace rocks::reflection_text::detail {

// 참조를 담은 struct 자리 path(인라인 struct 필드·멤버, 정적 배열·StructRefArray의 struct 원소 —
// view는 그 struct)를 기본값으로 되돌린다(리플렉션 B9i3c3 — 참조 struct `{}`). 통째로 떼어 낼 수
// 없으므로 멤버마다 기존 준비 단위로 나눈다: 값 멤버는 값 단위의 ResetToDefault, ObjectRef 멤버와
// ObjectRef 정적 배열 원소는 null 참조 단위, ObjectRefArray·ObjectRefMap 멤버는 컨테이너 단위의
// 비우기, 참조 struct 멤버와 그 정적 배열 원소는 다시 나눈다. StructRefArray 멤버는 원소 지우기
// 단위의 비우기다(B9i3c4).
// ReadOnly 멤버(staged 대상 제외)는 값이 이미 기본값인 참조가 아니면 진단 ReadOnly다.
Result<void> ResetReferenceStruct(AssignmentState& state, PropertyPath path, StructView view,
                                  TextSource source, StringView pathText) noexcept {
    const usize count = path.steps.Size();
    if (view.IsEmpty() || count >= MaxPropertyPathSteps) {
        return Diagnose(state, TextDiagnosticCode::Unsupported, source, pathText,
                        "this struct cannot be reset");
    }

    ObjectRegistry& registry = state.target.Registry();
    const ObjectAccessContext& access = state.target.Access();
    const bool staged = state.target.GetKind() == AssignmentTarget::Kind::Staged;
    PathStep steps[MaxPropertyPathSteps];
    for (usize index = 0; index < count; ++index) {
        steps[index] = path.steps[index];
    }

    FreshFailure fresh(state);
    const auto& members = view.Schema().fields;
    for (usize member = 0; member < members.Size() && !state.failed; ++member) {
        const FieldSchema& field = members[member];
        steps[count] = PathStep{};
        steps[count].fieldId = field.id;
        const PropertyPath child{Span<const PathStep>(steps, count + 1), path.keys};
        TextSlot slot;
        if (auto described = DescribeTextPath(state.lease, child, slot); !described) {
            return described;
        }

        const bool readOnly =
            !staged && (slot.flags & static_cast<uint32>(FieldFlags::ReadOnly)) != 0;
        const ValueKind target = FieldTargetType(field).kind;
        const bool strongStruct =
            !slot.structView.IsEmpty() && slot.structView.ContainsStrongReference();
        Result<void> done = Result<void>::Success();
        if (field.value.kind == ValueKind::StaticArray &&
            (target == ValueKind::ObjectReference || strongStruct)) {
            // 원소마다 null 참조 단위이거나 다시 나눈다.
            steps[count].selector = PathSelector::Index;
            for (uint32 element = 0; element < field.maxElements && done && !state.failed;
                 ++element) {
                steps[count].index = element;
                done = target == ValueKind::ObjectReference
                           ? AssignReference(state, child, ObjectHandle{}, readOnly, false, nullptr,
                                             source, pathText)
                           : ResetReferenceStruct(state, child, slot.structView, source, pathText);
            }
        } else if (target == ValueKind::ObjectReference &&
                   (field.value.kind == ValueKind::Array || field.value.kind == ValueKind::Map)) {
            auto container = AcquireContainer(state, child, source, pathText);
            if (!container || !container.Value()) {
                return container ? Result<void>::Success()
                                 : Result<void>::Failure(container.Error());
            }

            PropertyPathFailure failure;
            if (auto cleared = PathClear(registry, access,
                                         state.Root().WithReferences(container.Value()->stage),
                                         child, &failure);
                !cleared) {
                done = ReportPathFailure(state, cleared.Error(), failure, source, pathText);
            } else {
                container.Value()->dirty = true;
            }
        } else if (target == ValueKind::ObjectReference) {
            done = AssignReference(state, child, ObjectHandle{}, readOnly, false, nullptr, source,
                                   pathText);
        } else if (strongStruct && field.value.kind == ValueKind::Struct) {
            done = ResetReferenceStruct(state, child, slot.structView, source, pathText);
        } else if (strongStruct) {
            // StructRefArray 멤버는 원소 지우기 단위의 비우기다(리플렉션 B9i3c4).
            done = ClearElements(state, child, readOnly, source, pathText);
        } else if (readOnly) {
            done = Diagnose(state, TextDiagnosticCode::ReadOnly, source, pathText,
                            "property is read-only");
        } else {
            auto unit = AcquireUnit(state, child, source, pathText);
            if (!unit || !unit.Value()) {
                return unit ? Result<void>::Success() : Result<void>::Failure(unit.Error());
            }

            unit.Value()->box.ResetToDefault();
            unit.Value()->dirty = true;
        }

        if (!done) {
            return done;
        }
    }

    return Result<void>::Success();
}

// 준비 단위가 없는 자리의 대입이다: 참조를 담은 struct 자리(인라인 struct 필드·멤버, 정적 배열·
// StructRefArray의 struct 원소)의 `{}`만 받아 멤버마다 나누고(ResetReferenceStruct) override는 그
// struct 경로로 넣는다. StructRefArray 통째·원소 지우기는 ApplyElements(B9i3c4), 그 밖은
// Unsupported다.
Result<void> ApplyReferenceStruct(AssignmentState& state, const TextAssignment& assignment,
                                  const TextSlot& slot, PropertyPath path,
                                  TextSource source) noexcept {
    // StructRefArray 통째(비우기)와 원소 지우기는 원소 지우기 단위다(리플렉션 B9i3c4).
    if (IsStructRefArraySlot(slot) &&
        (slot.part == TextSlotPart::Field || assignment.kind != AssignmentKind::Set)) {
        return ApplyElements(state, assignment, slot, path, source);
    }

    const bool structSlot =
        !slot.structView.IsEmpty() &&
        ((slot.part == TextSlotPart::Field && slot.field->value.kind == ValueKind::Struct) ||
         slot.part == TextSlotPart::Element);
    if (assignment.kind != AssignmentKind::Set || !structSlot) {
        return Diagnose(state, TextDiagnosticCode::Unsupported, source, assignment.path,
                        "this assignment to a reference-holding value is not supported yet");
    }

    TextValue value(state.allocator);
    auto parsed = ReadAssignmentValue(state, slot, assignment, source, value);
    if (!parsed) {
        return Result<void>::Failure(parsed.Error());
    }

    if (!parsed.Value()) {
        state.failed = true;
        return Result<void>::Success();
    }

    if (value.shape != TextValueShape::StructDefault) {
        return Diagnose(state, TextDiagnosticCode::InvalidValue, source, assignment.path,
                        "value does not fit the property");
    }

    FreshFailure fresh(state);
    if (auto reset = ResetReferenceStruct(state, path, slot.structView, source, assignment.path);
        !reset || state.failed) {
        return reset;
    }

    return AddOverride(state, path);
}

} // namespace rocks::reflection_text::detail
