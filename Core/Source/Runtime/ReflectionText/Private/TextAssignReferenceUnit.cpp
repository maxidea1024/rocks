#include "TextAssignApplyInternal.h"

// 단일 강한 참조 위치의 간선 변경 단위다(리플렉션 B9i3c2·c3).
namespace rocks::reflection_text::detail {

// 단일 강한 참조 위치 path를 target으로 바꾸는 간선 변경 단위를 둔다(리플렉션 B9i3c2·c3). 같은
// 위치의 단위가 이미 있으면(참조 struct `{}`가 만든 null 단위 등) 그 새 대상을 바꾼다. 지금 대상(그
// 단위의 새 대상, 없으면 객체의 값)과 같으면 무동작이다(record면 override를 넣는다 — ReadOnly
// 자리는 넣지 않는다). 다르면 ReadOnly 자리는 진단, 아니면 pinValue의 대상을 고정하고 자리와 대상을
// 검증(PathPrepareReference — ReadOnly·기대 타입·대상 생존)한다.
Result<void> AssignReference(AssignmentState& state, PropertyPath path, ObjectHandle target,
                             bool readOnly, bool record, const TextValue* pinValue,
                             TextSource source, StringView pathText) noexcept {
    ObjectRegistry& registry = state.target.Registry();
    const ObjectAccessContext& access = state.target.Access();
    const PathTarget root = state.Root();
    ReferenceUnit* existing = nullptr;
    for (usize index = 0; index < state.references.Size() && !existing; ++index) {
        if (SamePropertyPath(state.references[index]->Path(), path)) {
            existing = state.references[index].Get();
        }
    }

    PropertyPathFailure failure;
    ObjectHandle current = existing ? existing->target : ObjectHandle{};
    if (!existing) {
        FieldValue read(state.allocator);
        if (auto found = PathRead(registry, access, root, path, read, &failure); !found) {
            return ReportPathFailure(state, found.Error(), failure, source, pathText);
        }

        current = read.reference;
    }

    if (current == target) {
        return readOnly || !record ? Result<void>::Success() : AddOverride(state, path);
    }

    if (readOnly) {
        return Diagnose(state, TextDiagnosticCode::ReadOnly, source, pathText,
                        "property is read-only");
    }

    FieldValue value(state.allocator);
    value.kind = ValueKind::ObjectReference;
    value.reference = target;
    ReferenceUnit* unit = existing;
    UniquePtr<ReferenceUnit> made;
    if (!unit) {
        auto created = UniquePtr<ReferenceUnit>::TryMake(state.allocator, state.allocator);
        if (!created) {
            return Result<void>::Failure(created.Error());
        }

        made = Move(created.Value());
        unit = made.Get();
        if (auto copied = unit->steps.TryAppend(path.steps); !copied) {
            return copied;
        }

        if (auto reserved = state.references.TryReserve(state.references.Size() + 1); !reserved) {
            return reserved;
        }
    }

    // 앞 단위의 새 대상은 이제 쓰이지 않으므로 그 고정을 놓고(Publish의 검사에서도 빠진다 —
    // 리플렉션 B9i 리뷰 반영) 새 대상을 이 단위의 고정으로 잡는다.
    if (existing) {
        ReleasePins(state, existing);
    }

    if (pinValue) {
        auto pinned = PinReferences(state, *pinValue, source, pathText, unit);
        if (!pinned || !pinned.Value()) {
            if (made) {
                ReleasePins(state, unit);
            }

            return pinned ? Result<void>::Success() : Result<void>::Failure(pinned.Error());
        }
    }

    // 앞 단위가 있으면 자리는 그대로이고 새 대상만 다시 검증한다(찾은 자리는 버린다).
    PreparedReference scratch;
    PreparedReference& located = made ? unit->prepared : scratch;
    if (auto checked = PathPrepareReference(registry, access, root, path, value, located, &failure);
        !checked) {
        if (made) {
            ReleasePins(state, unit);
        }

        return ReportPathFailure(state, checked.Error(), failure, source, pathText);
    }

    unit->target = target;
    if (made) {
        unit->previous = unit->prepared.Previous();
        (void)state.references.TryPushBack(Move(made));
    }

    return record ? AddOverride(state, path) : Result<void>::Success();
}

// 단일 강한 참조 위치의 대입이다(리플렉션 B9i3c2): Set만 받고 단일 ObjectReference 값을 간선 변경
// 단위로 둔다(AssignReference).
Result<void> ApplyReference(AssignmentState& state, const TextAssignment& assignment,
                            const TextSlot& slot, PropertyPath path, TextSource source) noexcept {
    if (assignment.kind != AssignmentKind::Set) {
        return Diagnose(state, TextDiagnosticCode::InvalidValue, source, assignment.path,
                        "a reference cannot be unset or erased; assign none");
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

    if (value.shape != TextValueShape::Single || value.single.kind != ValueKind::ObjectReference) {
        return Diagnose(state, TextDiagnosticCode::InvalidValue, source, assignment.path,
                        "value does not fit the property");
    }

    const bool readOnly = state.target.GetKind() != AssignmentTarget::Kind::Staged &&
                          (slot.flags & static_cast<uint32>(FieldFlags::ReadOnly)) != 0;
    return AssignReference(state, path, value.single.reference, readOnly, true, &value, source,
                           assignment.path);
}

} // namespace rocks::reflection_text::detail
