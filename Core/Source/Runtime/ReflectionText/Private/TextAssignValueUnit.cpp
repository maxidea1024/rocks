#include "TextAssignApplyInternal.h"

#include <Object/PathKey.h>

// 값 단위(상자)와 Registry 컨테이너 단위(준비 사본)에 대입 하나를 적용한다(리플렉션 B9i3c1·c3).
namespace rocks::reflection_text::detail {

Result<void> Apply::Run() noexcept {
    const bool readOnly = state.target.GetKind() != AssignmentTarget::Kind::Staged &&
                          (slot.flags & static_cast<uint32>(FieldFlags::ReadOnly)) != 0;
    const ValueKind fieldKind = slot.field->value.kind;
    switch (assignment.kind) {
    case AssignmentKind::Unset: {
        if (slot.part != TextSlotPart::Field || fieldKind != ValueKind::Optional) {
            return Diagnostic(TextDiagnosticCode::InvalidValue,
                              "only an Optional property can be unset");
        }

        if (readOnly) {
            return Diagnostic(TextDiagnosticCode::ReadOnly, "property is read-only");
        }

        Result<void> done = Result<void>::Success();
        if (slot.field->element.kind == ValueKind::Struct) {
            done = PathResetOptional(Registry(), Access(), Start(), relative, &failure);
        } else {
            FieldValue unset(state.allocator);
            unset.kind = slot.field->element.kind;
            unset.present = false;
            done = PathWrite(Registry(), Access(), Start(), relative, unset, nullptr, &failure);
        }

        return done ? Changed() : Failed(done.Error());
    }
    case AssignmentKind::Erase: {
        if (slot.part == TextSlotPart::Field) {
            return Diagnostic(TextDiagnosticCode::InvalidValue,
                              "erase needs a key or an element index");
        }

        if (readOnly) {
            return Diagnostic(TextDiagnosticCode::ReadOnly, "property is read-only");
        }

        auto done =
            slot.part == TextSlotPart::Element
                ? PathRemoveElement(Registry(), Access(), Start(), relative, nullptr, &failure)
                : PathEraseKey(Registry(), Access(), Start(), relative, nullptr, &failure);
        return done ? Changed() : Failed(done.Error());
    }
    case AssignmentKind::Set:
        break;
    default:
        return Diagnostic(TextDiagnosticCode::InvalidValue, "unknown assignment kind");
    }

    if (slot.part == TextSlotPart::SetElement) {
        // Set 원소 자리는 값이 없다: 빈 값이 원소 넣기다.
        if (!assignment.value.IsEmpty()) {
            return Diagnostic(TextDiagnosticCode::InvalidValue, "a Set element takes no value");
        }

        if (readOnly) {
            return Diagnostic(TextDiagnosticCode::ReadOnly, "property is read-only");
        }

        auto inserted = PathInsertKey(Registry(), Access(), Start(), relative, &failure);
        return inserted ? Changed() : Failed(inserted.Error());
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

    auto pinned = PinReferences(state, value, source, assignment.path);
    if (!pinned || !pinned.Value()) {
        return pinned ? Result<void>::Success() : Result<void>::Failure(pinned.Error());
    }

    if (value.shape == TextValueShape::Single) {
        // 지금 값과 같으면 무동작이다(ReadOnly·Deprecated ClassRef의 같은 값 재대입 포함).
        FieldValue current(state.allocator);
        PropertyPathFailure readFailure;
        auto read = PathRead(Registry(), Access(), Start(), relative, current, &readFailure);
        if (!read && read.Error() == ErrorCode::OutOfMemory) {
            return read;
        }

        if (read && SameValue(current, value.single)) {
            return readOnly ? Result<void>::Success() : AddOverride(state, path);
        }
    }

    if (readOnly) {
        return Diagnostic(TextDiagnosticCode::ReadOnly, "property is read-only");
    }

    return RunSet(value);
}

Result<void> Apply::RunSet(TextValue& value) noexcept {
    const ValueKind fieldKind = slot.field->value.kind;
    switch (value.shape) {
    case TextValueShape::Single: {
        auto written =
            PathWrite(Registry(), Access(), Start(), relative, value.single, nullptr, &failure);
        return written ? Changed() : Failed(written.Error());
    }
    case TextValueShape::StructDefault:
        if (slot.part == TextSlotPart::MapValue) {
            auto inserted = PathInsertKey(Registry(), Access(), Start(), relative, &failure);
            return inserted ? Changed() : Failed(inserted.Error());
        }

        if (slot.part == TextSlotPart::Element) {
            if (fieldKind == ValueKind::StaticArray) {
                // 길이가 고정이라 지우고 넣을 수 없다: 원소 멤버마다 기본값으로 되돌린다.
                if (auto reset = ResetMembers(); !reset || state.failed) {
                    return reset;
                }

                return Changed();
            }

            // Array<struct> 원소: index가 길이면 끝에 기본 원소를 더하고(10.4절 `a[i]={}` — JSON
            // import의 원소 펼침도 이것이다, 리플렉션 B9i4), 작으면 그 자리를 지우고 기본 원소를
            // 넣는다.
            PathStep arraySteps[MaxPropertyPathSteps];
            const usize last = relative.steps.Size() - 1;
            for (usize index = 0; index <= last; ++index) {
                arraySteps[index] = relative.steps[index];
            }

            arraySteps[last].selector = PathSelector::None;
            arraySteps[last].index = 0;
            auto count = PathCount(
                Registry(), Access(), Start(),
                PropertyPath{Span<const PathStep>(arraySteps, last + 1), relative.keys}, &failure);
            if (!count) {
                return Failed(count.Error());
            }

            const bool append = relative.steps[last].index == count.Value();
            if (auto removed = append ? Result<void>::Success()
                                      : PathRemoveElement(Registry(), Access(), Start(), relative,
                                                          nullptr, &failure);
                !removed) {
                return Failed(removed.Error());
            }

            auto inserted =
                PathInsertElement(Registry(), Access(), Start(), relative, nullptr, &failure);
            return inserted ? Changed() : Failed(inserted.Error());
        }

        if (fieldKind == ValueKind::Optional) {
            auto made = PathEmplaceOptional(Registry(), Access(), Start(), relative, &failure);
            return made ? Changed() : Failed(made.Error());
        }

        if (auto reset = ResetStruct(); !reset || state.failed) {
            return reset;
        }

        return Changed();
    case TextValueShape::List:
    case TextValueShape::Entries:
    case TextValueShape::Empty:
        if (fieldKind != ValueKind::Array && fieldKind != ValueKind::StaticArray &&
            fieldKind != ValueKind::Map && fieldKind != ValueKind::Set) {
            return Diagnostic(TextDiagnosticCode::InvalidValue, "value does not fit the property");
        }

        if (auto replaced = ReplaceContents(value); !replaced || state.failed) {
            return replaced;
        }

        return Changed();
    }

    return Diagnostic(TextDiagnosticCode::InvalidValue, "value does not fit the property");
}

Result<void> Apply::ResetStruct() noexcept {
    // 단위 자신이면 상자를 기본값으로, 안쪽 struct면 그 자리를 따로 떼어 기본값으로 바꿔 되돌린다.
    // 값 단위만 struct 자리를 가진다(컨테이너 단위의 값은 참조다).
    if (!box) {
        return Diagnostic(TextDiagnosticCode::InvalidValue, "value does not fit the property");
    }

    if (relative.steps.Size() == 1) {
        box->ResetToDefault();
        return Result<void>::Success();
    }

    return ResetAt(relative);
}

Result<void> Apply::ResetAt(PropertyPath at) noexcept {
    DetachedValue inner(box->GetAllocator());
    if (auto detached = PathDetach(Registry(), Access(), Start(), at, inner, &failure); !detached) {
        return Failed(detached.Error());
    }

    inner.ResetToDefault();
    PreparedAttach attach;
    if (auto located =
            PathPrepareAttach(Registry(), Access(), Start(), at, inner, attach, &failure);
        !located) {
        return Failed(located.Error());
    }

    // 옛 값은 inner가 받아 이 함수를 나갈 때 반환한다.
    return attach.Publish();
}

Result<void> Apply::ResetMembers() noexcept {
    // 정적 배열의 struct 원소는 떼어 낼 수 없으므로(선택자가 있는 끝) 멤버마다 기본값으로 되돌린다.
    const StructView view = slot.structView;
    const usize count = relative.steps.Size();
    if (!box || view.IsEmpty() || count >= MaxPropertyPathSteps) {
        return Diagnostic(TextDiagnosticCode::Unsupported, "this element cannot be reset");
    }

    PathStep steps[MaxPropertyPathSteps];
    for (usize index = 0; index < count; ++index) {
        steps[index] = relative.steps[index];
    }

    FreshFailure fresh(state);
    const auto& members = view.Schema().fields;
    for (usize index = 0; index < members.Size(); ++index) {
        steps[count] = PathStep{};
        steps[count].fieldId = members[index].id;
        if (auto reset =
                ResetAt(PropertyPath{Span<const PathStep>(steps, count + 1), relative.keys});
            !reset || state.failed) {
            return reset;
        }
    }

    return Result<void>::Success();
}

Result<void> Apply::ReplaceContents(const TextValue& value) noexcept {
    const ValueKind fieldKind = slot.field->value.kind;
    PathStep steps[MaxPropertyPathSteps];
    PathKey keys[MaxPropertyPathSteps];
    const usize stepCount = relative.steps.Size();
    const usize keyCount = relative.keys.Size();
    for (usize index = 0; index < stepCount; ++index) {
        steps[index] = relative.steps[index];
    }

    for (usize index = 0; index < keyCount; ++index) {
        keys[index] = relative.keys[index];
    }

    PathStep& last = steps[stepCount - 1];
    const PropertyPath element{Span<const PathStep>(steps, stepCount),
                               Span<const PathKey>(keys, keyCount)};
    const PropertyPath keyed{Span<const PathStep>(steps, stepCount),
                             Span<const PathKey>(keys, keyCount + 1)};
    if (fieldKind == ValueKind::StaticArray) {
        // 길이가 고정이라 원소마다 쓴다(원소 수는 ParseTextValue가 N과 대조했다).
        last.selector = PathSelector::Index;
        for (usize index = 0; index < value.elements.Size(); ++index) {
            last.index = static_cast<uint32>(index);
            if (auto written = PathWrite(Registry(), Access(), Start(), element,
                                         value.elements[index], nullptr, &failure);
                !written) {
                return Failed(written.Error());
            }
        }

        return Result<void>::Success();
    }

    if (auto cleared = PathClear(Registry(), Access(), Start(), relative, &failure); !cleared) {
        return Failed(cleared.Error());
    }

    if (fieldKind == ValueKind::Array) {
        last.selector = PathSelector::Index;
        for (usize index = 0; index < value.elements.Size(); ++index) {
            last.index = static_cast<uint32>(index);
            if (auto inserted = PathInsertElement(Registry(), Access(), Start(), element,
                                                  &value.elements[index], &failure);
                !inserted) {
                return Failed(inserted.Error());
            }
        }

        return Result<void>::Success();
    }

    // Set 원소·Map 항목은 키를 경로 key로 만들어 넣는다.
    Array<uint8> wire(state.allocator);
    last.selector = PathSelector::Key;
    last.index = static_cast<uint32>(keyCount);
    for (usize index = 0; index < value.elements.Size(); ++index) {
        wire.Clear();
        if (auto made = AppendKeyWire(state, value.elements[index], wire); !made) {
            return made;
        }

        keys[keyCount] = PathKey{value.elements[index].kind, wire.View()};
        Result<void> done = Result<void>::Success();
        if (fieldKind == ValueKind::Set) {
            auto inserted = PathInsertKey(Registry(), Access(), Start(), keyed, &failure);
            done = inserted ? Result<void>::Success() : Result<void>::Failure(inserted.Error());
        } else {
            done = PathWrite(Registry(), Access(), Start(), keyed, value.values[index], nullptr,
                             &failure);
        }

        if (!done) {
            return Failed(done.Error());
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::reflection_text::detail
