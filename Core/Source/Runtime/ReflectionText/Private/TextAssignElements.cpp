#include "TextAssignApplyInternal.h"

// StructRefArray 원소 지우기·비우기·추가 단위다(리플렉션 B9i3c4·B9i5 —
// Docs/Design/ReflectionExtensions.ko.md 10.2절 "구현(B9i3c4)에서 정한 점"·"구현(B9i5)에서 정한
// 점"). 원소 블록은 게시 때까지 그대로이므로 같은 묶음의 다른 단위는 원소를 묶을 때 index로
// 가리킨다: 뒤 대입의 지금 index는 MapElementIndices가 옮기고, 지운 원소 안의 값·참조 단위는
// 버린다(그 원소의 간선은 게시 때 {참조, null}로 선언한다). 더한 원소(id ≥ 묶을 때 원소 수)는
// 준비물이 소유한 블록이고 그 안 대입은 블록에 바로 적용한다(간선은 게시 때 {null, 참조}).
namespace rocks::reflection_text::detail {

namespace {

// 배열 경로 arrayPath(선택자 없음)가 path의 앞부분인지다(배열 단계는 선택자를 보지 않는다).
bool UnderArray(PropertyPath path, PropertyPath arrayPath) noexcept {
    const usize count = arrayPath.steps.Size();
    if (count == 0 || path.steps.Size() < count) {
        return false;
    }

    for (usize index = 0; index < count; ++index) {
        const PathStep& left = path.steps[index];
        const PathStep& right = arrayPath.steps[index];
        if (left.fieldId != right.fieldId) {
            return false;
        }

        if (index + 1 < count && (left.selector != right.selector || left.index != right.index)) {
            return false;
        }
    }

    return true;
}

// path가 지운 원소 안(배열 단계의 index가 지운 원소)인지다.
bool InsideRemoved(PropertyPath path, PropertyPath arrayPath,
                   const PreparedStructElements& stage) noexcept {
    if (!UnderArray(path, arrayPath) || path.steps.Size() == arrayPath.steps.Size()) {
        return false;
    }

    const PathStep& step = path.steps[arrayPath.steps.Size() - 1];
    return step.selector == PathSelector::Index && stage.IsRemoved(step.index);
}

// 지운 원소 안의 값·참조 단위를 버린다(순서는 유지한다 — 할당하지 않는다). 그 단위가 만든 고정도
// 놓는다(Publish의 대상 검사에서 빠진다 — 리플렉션 B9i 리뷰 반영).
void DiscardRemovedUnits(AssignmentState& state, const ElementsUnit& unit) noexcept {
    const PropertyPath arrayPath = unit.Path();
    for (usize index = state.units.Size(); index != 0; --index) {
        if (InsideRemoved(state.units[index - 1]->Path(), arrayPath, unit.stage)) {
            ReleasePins(state, state.units[index - 1].Get());
            (void)state.units.TryRemoveAt(index - 1);
        }
    }

    for (usize index = state.references.Size(); index != 0; --index) {
        if (InsideRemoved(state.references[index - 1]->Path(), arrayPath, unit.stage)) {
            ReleasePins(state, state.references[index - 1].Get());
            (void)state.references.TryRemoveAt(index - 1);
        }
    }

    // 지운 추가 원소 안 대입이 잡은 고정도 놓는다(고정 owner는 블록 주소다).
    const uint32 first = static_cast<uint32>(unit.stage.OriginalCount());
    for (uint32 id = first; unit.stage.IsAdded(id); ++id) {
        if (unit.stage.IsRemoved(id)) {
            ReleasePins(state, unit.stage.AddedBlock(id));
        }
    }
}

// 경로 앞 count단계를 끝 선택자를 뗀 배열 경로로 steps에 복사한다.
PropertyPath ArrayPrefix(PropertyPath path, usize count, PathStep* steps) noexcept {
    for (usize index = 0; index < count; ++index) {
        steps[index] = path.steps[index];
    }

    steps[count - 1].selector = PathSelector::None;
    steps[count - 1].index = 0;
    return PropertyPath{Span<const PathStep>(steps, count), {}};
}

} // namespace

AddedElement FindAddedElement(AssignmentState& state, PropertyPath path) noexcept {
    if (state.elements.IsEmpty()) {
        return {};
    }

    PathStep prefix[MaxPropertyPathSteps];
    for (usize index = 0; index < path.steps.Size(); ++index) {
        if (path.steps[index].selector != PathSelector::Index) {
            continue;
        }

        const PropertyPath arrayPath = ArrayPrefix(path, index + 1, prefix);
        for (usize unit = 0; unit < state.elements.Size(); ++unit) {
            ElementsUnit& elements = *state.elements[unit];
            if (SamePropertyPath(elements.Path(), arrayPath) &&
                elements.stage.IsAdded(path.steps[index].index)) {
                return AddedElement{&elements, index, path.steps[index].index};
            }
        }
    }

    return {};
}

Result<bool> AppendElement(AssignmentState& state, const TextAssignment& assignment,
                           const TextSlot& slot, PropertyPath path, TextSource source) noexcept {
    if (assignment.kind != AssignmentKind::Set || !IsStructRefArraySlot(slot) ||
        slot.part != TextSlotPart::Element || path.steps.IsEmpty()) {
        return Result<bool>::Success(false);
    }

    PathStep steps[MaxPropertyPathSteps];
    const PropertyPath arrayPath = ArrayPrefix(path, path.steps.Size(), steps);
    auto acquired = AcquireElements(state, arrayPath, source, assignment.path);
    if (!acquired || !acquired.Value()) {
        return acquired ? Result<bool>::Success(true) : Result<bool>::Failure(acquired.Error());
    }

    // 지금 원소 수와 같은 index만 추가다(그 밖은 제자리 리셋이나 범위 밖 진단 — 호출자).
    ElementsUnit& unit = *acquired.Value();
    const uint32 index = path.steps[path.steps.Size() - 1].index;
    if (index != unit.stage.Count()) {
        return Result<bool>::Success(false);
    }

    const bool readOnly = state.target.GetKind() != AssignmentTarget::Kind::Staged &&
                          (slot.flags & static_cast<uint32>(FieldFlags::ReadOnly)) != 0;
    if (readOnly) {
        if (auto reported = Diagnose(state, TextDiagnosticCode::ReadOnly, source, assignment.path,
                                     "property is read-only");
            !reported) {
            return Result<bool>::Failure(reported.Error());
        }

        return Result<bool>::Success(true);
    }

    TextValue value(state.allocator);
    auto parsed = ReadAssignmentValue(state, slot, assignment, source, value);
    if (!parsed) {
        return Result<bool>::Failure(parsed.Error());
    }

    if (!parsed.Value()) {
        state.failed = true;
        return Result<bool>::Success(true);
    }

    if (value.shape != TextValueShape::StructDefault) {
        if (auto reported = Diagnose(state, TextDiagnosticCode::InvalidValue, source,
                                     assignment.path, "value does not fit the property");
            !reported) {
            return Result<bool>::Failure(reported.Error());
        }

        return Result<bool>::Success(true);
    }

    // 필드 MaxElements·원소 수 한도는 명령행 목록과 같은 검사다.
    auto fits = CheckElementCount(state.context, *slot.field, unit.stage.Count() + 1, source,
                                  assignment.path);
    if (!fits) {
        return Result<bool>::Failure(fits.Error());
    }

    if (!fits.Value()) {
        state.failed = true;
        return Result<bool>::Success(true);
    }

    auto id = unit.stage.Append();
    if (!id) {
        return Result<bool>::Failure(id.Error());
    }

    // 같은 경로 검사는 추가 원소 id 경로다(뒤의 같은 지금 index 대입이 같은 id로 옮겨진다).
    for (usize step = 0; step < path.steps.Size(); ++step) {
        steps[step] = path.steps[step];
    }

    steps[path.steps.Size() - 1].index = id.Value();
    const PropertyPath mapped{Span<const PathStep>(steps, path.steps.Size()), path.keys};
    if (auto added = state.assigned.Add(mapped); !added) {
        return Result<bool>::Failure(added.Error());
    }

    if (auto overridden = AddOverride(state, mapped); !overridden) {
        return Result<bool>::Failure(overridden.Error());
    }

    return Result<bool>::Success(true);
}

Result<void> ApplyAddedElement(AssignmentState& state, const TextAssignment& assignment,
                               const TextSlot& slot, PropertyPath path, TextSource source,
                               const AddedElement& added) noexcept {
    PreparedStructElements& stage = added.unit->stage;
    auto start = stage.AddedTarget(added.id);
    if (!start) {
        return Result<void>::Failure(start.Error());
    }

    // 원소 블록이 곧 사본이다(값 단위·참조 단위를 두지 않는다). ReadOnly 멤버는 기본값과 같은
    // 대입만 받는다(Apply의 unit 없는 ReadOnly 규칙).
    bool dirty = false;
    Apply apply{state,      start.Value(),
                dirty,      nullptr,
                assignment, slot,
                path,       PropertyPath{path.steps.Mid(added.step + 1), path.keys},
                source,     {},
                nullptr,    stage.AddedBlock(added.id)};
    return apply.Run();
}

bool IsStructRefArraySlot(const TextSlot& slot) noexcept {
    return slot.field && slot.field->value.kind == ValueKind::Array &&
           slot.field->element.kind == ValueKind::Struct && !slot.structView.IsEmpty() &&
           slot.structView.ContainsStrongReference();
}

Result<bool> MapElementIndices(AssignmentState& state, PropertyPath path, PathStep* steps,
                               TextSource source, StringView pathText, bool diagnose) noexcept {
    for (usize index = 0; index < path.steps.Size(); ++index) {
        steps[index] = path.steps[index];
    }

    if (state.elements.IsEmpty()) {
        return Result<bool>::Success(true);
    }

    PathStep prefix[MaxPropertyPathSteps];
    for (usize index = 0; index < path.steps.Size(); ++index) {
        if (path.steps[index].selector != PathSelector::Index) {
            continue;
        }

        const PropertyPath arrayPath = ArrayPrefix(path, index + 1, prefix);
        for (usize unit = 0; unit < state.elements.Size(); ++unit) {
            const PreparedStructElements& stage = state.elements[unit]->stage;
            if (!SamePropertyPath(state.elements[unit]->Path(), arrayPath)) {
                continue;
            }

            if (path.steps[index].index >= stage.Count()) {
                if (!diagnose) {
                    return Result<bool>::Success(false);
                }

                if (auto reported = Diagnose(state, TextDiagnosticCode::OutOfRange, source,
                                             pathText, "element index is out of range");
                    !reported) {
                    return Result<bool>::Failure(reported.Error());
                }

                return Result<bool>::Success(false);
            }

            steps[index].index = stage.OriginalIndex(path.steps[index].index);
            break;
        }
    }

    return Result<bool>::Success(true);
}

Result<void> ClearElements(AssignmentState& state, PropertyPath arrayPath, bool readOnly,
                           TextSource source, StringView pathText) noexcept {
    auto acquired = AcquireElements(state, arrayPath, source, pathText);
    if (!acquired || !acquired.Value()) {
        return acquired ? Result<void>::Success() : Result<void>::Failure(acquired.Error());
    }

    ElementsUnit& unit = *acquired.Value();
    if (unit.stage.Count() == 0) {
        return Result<void>::Success();
    }

    if (readOnly) {
        return Diagnose(state, TextDiagnosticCode::ReadOnly, source, pathText,
                        "property is read-only");
    }

    if (auto cleared = unit.stage.Clear(); !cleared) {
        return cleared;
    }

    DiscardRemovedUnits(state, unit);
    return Result<void>::Success();
}

Result<void> ApplyElements(AssignmentState& state, const TextAssignment& assignment,
                           const TextSlot& slot, PropertyPath path, TextSource source) noexcept {
    const bool readOnly = state.target.GetKind() != AssignmentTarget::Kind::Staged &&
                          (slot.flags & static_cast<uint32>(FieldFlags::ReadOnly)) != 0;
    PathStep steps[MaxPropertyPathSteps];
    const PropertyPath arrayPath = ArrayPrefix(path, path.steps.Size(), steps);
    if (assignment.kind == AssignmentKind::Erase && slot.part == TextSlotPart::Element) {
        if (readOnly) {
            return Diagnose(state, TextDiagnosticCode::ReadOnly, source, assignment.path,
                            "property is read-only");
        }

        auto acquired = AcquireElements(state, arrayPath, source, assignment.path);
        if (!acquired || !acquired.Value()) {
            return acquired ? Result<void>::Success() : Result<void>::Failure(acquired.Error());
        }

        // path의 index는 묶을 때 index다(처음 묶는 대입이면 지금 index와 같다).
        ElementsUnit& unit = *acquired.Value();
        const uint32 original = path.steps[path.steps.Size() - 1].index;
        usize position = IndexNone;
        for (usize index = 0; index < unit.stage.Count() && position == IndexNone; ++index) {
            if (unit.stage.OriginalIndex(index) == original) {
                position = index;
            }
        }

        if (position == IndexNone) {
            return Diagnose(state, TextDiagnosticCode::OutOfRange, source, assignment.path,
                            "element index is out of range");
        }

        if (auto removed = unit.stage.RemoveAt(position); !removed) {
            return removed;
        }

        DiscardRemovedUnits(state, unit);
        return AddOverride(state, path);
    }

    if (assignment.kind != AssignmentKind::Set || slot.part != TextSlotPart::Field) {
        return Diagnose(state, TextDiagnosticCode::InvalidValue, source, assignment.path,
                        assignment.kind == AssignmentKind::Erase
                            ? "erase needs an element index"
                            : "a struct element array can only be emptied or have elements erased");
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

    // 통째 값은 빈 목록(비우기)뿐이다(값 해석이 struct 원소 목록을 거부한다).
    if ((value.shape != TextValueShape::Empty && value.shape != TextValueShape::List) ||
        !value.elements.IsEmpty()) {
        return Diagnose(state, TextDiagnosticCode::InvalidValue, source, assignment.path,
                        "value does not fit the property");
    }

    FreshFailure fresh(state);
    if (auto cleared = ClearElements(state, arrayPath, readOnly, source, assignment.path);
        !cleared || state.failed) {
        return cleared;
    }

    // ReadOnly 배열이 이미 비었으면 override에 넣지 않는다(값이 같은 ReadOnly 대입과 같다).
    return readOnly ? Result<void>::Success() : AddOverride(state, path);
}

} // namespace rocks::reflection_text::detail
