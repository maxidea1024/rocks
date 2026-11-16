#include "TextAssignInternal.h"

#include <Object/NameField.h>
#include <Object/PathKey.h>

// 대입 하나를 해석해 준비 단위 사본에 적용한다(리플렉션 B9i3c1 — ReflectionText/TextAssignments.h의
// PrepareAssignments 규칙). 경로 해석 → 같은 경로 검사 → 준비 단위(참조 없는 필드·멤버 하나) →
// 값 읽기(resolver) → 사본 위 경로 연산 → override 경로.
namespace rocks::reflection_text::detail {

namespace {

bool SameBytes(const void* left, const void* right, usize size) noexcept {
    const auto* a = static_cast<const uint8*>(left);
    const auto* b = static_cast<const uint8*>(right);
    for (usize index = 0; index < size; ++index) {
        if (a[index] != b[index]) {
            return false;
        }
    }

    return true;
}

// 두 단일 값이 같은 값인지다(실수는 bit 단위 — NaN payload·-0도 구별한다).
bool SameValue(const FieldValue& left, const FieldValue& right) noexcept {
    if (left.kind != right.kind || left.present != right.present) {
        return false;
    }

    if (!left.present) {
        return true;
    }

    switch (left.kind) {
    case ValueKind::Bool:
        return left.boolean == right.boolean;
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
    case ValueKind::UInt64:
        return left.unsignedInteger == right.unsignedInteger;
    case ValueKind::Int8:
    case ValueKind::Int16:
    case ValueKind::Int32:
    case ValueKind::Int64:
        return left.signedInteger == right.signedInteger;
    case ValueKind::Enum:
        return left.signedInteger == right.signedInteger && left.type == right.type;
    case ValueKind::Float32:
        return SameBytes(&left.float32, &right.float32, sizeof(float));
    case ValueKind::Float64:
        return SameBytes(&left.float64, &right.float64, sizeof(double));
    case ValueKind::Guid:
    case ValueKind::AssetId:
    case ValueKind::ClassReference:
        return left.guid == right.guid;
    case ValueKind::Utf8String:
        return left.string.View() == right.string.View();
    case ValueKind::SoftObjectReference:
        return left.guid == right.guid && left.string.View() == right.string.View();
    case ValueKind::Digest256:
        return left.digest == right.digest;
    case ValueKind::Name:
        return left.name == right.name;
    case ValueKind::ObjectReference:
    case ValueKind::WeakObjectReference:
        return left.reference == right.reference;
    default:
        return false;
    }
}

// 호출 오류(대입 내용과 무관한 실패)인지다.
bool IsCallError(ErrorCode code) noexcept {
    return code == ErrorCode::OutOfMemory || code == ErrorCode::Busy ||
           code == ErrorCode::StaleHandle || code == ErrorCode::Destroyed ||
           code == ErrorCode::WrongThread || code == ErrorCode::WrongContext ||
           code == ErrorCode::ShuttingDown;
}

// 새로 가리키는 Weak 대상을 고정한다(Publish가 다시 확인한다). 살아 있지 않으면 진단이다.
Result<bool> PinReferences(AssignmentState& state, const TextValue& value, TextSource source,
                           StringView pathText) noexcept {
    const FieldValue* lists[] = {&value.single};
    const Span<const FieldValue> groups[] = {Span<const FieldValue>(lists[0], 1),
                                             value.elements.View(), value.values.View()};
    for (const Span<const FieldValue>& group : groups) {
        for (usize index = 0; index < group.Size(); ++index) {
            const FieldValue& item = group[index];
            if (item.kind != ValueKind::WeakObjectReference || item.reference.IsNull()) {
                continue;
            }

            auto pinned = state.target.Registry().TryPin(state.target.Access(), item.reference);
            if (!pinned) {
                if (IsCallError(pinned.Error()) && pinned.Error() != ErrorCode::StaleHandle) {
                    return Result<bool>::Failure(pinned.Error());
                }

                if (auto reported = Diagnose(state, TextDiagnosticCode::InvalidValue, source,
                                             pathText, "referenced object is not alive");
                    !reported) {
                    return Result<bool>::Failure(reported.Error());
                }

                return Result<bool>::Success(false);
            }

            if (auto kept = state.pins.TryPushBack(Move(pinned.Value())); !kept) {
                return Result<bool>::Failure(kept.Error());
            }
        }
    }

    return Result<bool>::Success(true);
}

// Map 키·Set 원소 값의 key wire를 wire 뒤에 붙인다.
Result<void> AppendKeyWire(AssignmentState& state, const FieldValue& value,
                           Array<uint8>& wire) noexcept {
    PathKeyValue key;
    key.kind = value.kind;
    key.boolean = value.boolean;
    key.unsignedInteger = value.unsignedInteger;
    key.signedInteger = value.signedInteger;
    key.guid = value.guid;
    key.digest = value.digest;
    key.text = value.string.View();
    if (value.kind != ValueKind::Name) {
        return AppendPathKey(key, wire);
    }

    auto display = AcquireNameFieldText(state.options.names, value.name, state.allocator);
    if (!display) {
        return Result<void>::Failure(display.Error());
    }

    key.text = display.Value().View();
    return AppendPathKey(key, wire);
}

// 대입 하나의 적용 문맥이다.
struct Apply {
    AssignmentState& state;
    AssignmentUnit& unit;
    const TextAssignment& assignment;
    const TextSlot& slot;
    // 대상 기준 끝 경로(override 경로의 원본)와 상자 기준 경로.
    PropertyPath path;
    PropertyPath relative;
    TextSource source;
    PropertyPathFailure failure{};

    ObjectRegistry& Registry() const noexcept {
        return state.target.Registry();
    }

    const ObjectAccessContext& Access() const noexcept {
        return state.target.Access();
    }

    PathTarget Box() const noexcept {
        return PathTarget::Value(unit.box);
    }

    Result<void> Diagnostic(TextDiagnosticCode code, StringView message) noexcept {
        return Diagnose(state, code, source, assignment.path, message);
    }

    Result<void> Failed(ErrorCode code) noexcept {
        return ReportPathFailure(state, code, failure, source, assignment.path);
    }

    // 사본을 바꾼 대입을 기록한다.
    Result<void> Changed() noexcept {
        unit.dirty = true;
        return AddOverride(state, path);
    }

    Result<void> Run() noexcept;
    Result<void> RunSet(TextValue& value) noexcept;
    Result<void> ResetStruct() noexcept;
    Result<void> ReplaceContents(const TextValue& value) noexcept;
};

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
            done = PathResetOptional(Registry(), Access(), Box(), relative, &failure);
        } else {
            FieldValue unset(state.allocator);
            unset.kind = slot.field->element.kind;
            unset.present = false;
            done = PathWrite(Registry(), Access(), Box(), relative, unset, nullptr, &failure);
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
                ? PathRemoveElement(Registry(), Access(), Box(), relative, nullptr, &failure)
                : PathEraseKey(Registry(), Access(), Box(), relative, nullptr, &failure);
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

        auto inserted = PathInsertKey(Registry(), Access(), Box(), relative, &failure);
        return inserted ? Changed() : Failed(inserted.Error());
    }

    TextValue value(state.allocator);
    auto parsed =
        ParseTextValue(state.context, slot, assignment.value, source, assignment.path, value);
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
        auto read = PathRead(Registry(), Access(), Box(), relative, current, &readFailure);
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
            PathWrite(Registry(), Access(), Box(), relative, value.single, nullptr, &failure);
        return written ? Changed() : Failed(written.Error());
    }
    case TextValueShape::StructDefault:
        if (slot.part == TextSlotPart::MapValue) {
            auto inserted = PathInsertKey(Registry(), Access(), Box(), relative, &failure);
            return inserted ? Changed() : Failed(inserted.Error());
        }

        if (slot.part == TextSlotPart::Element) {
            if (fieldKind == ValueKind::StaticArray) {
                return Diagnostic(TextDiagnosticCode::Unsupported,
                                  "a fixed-length struct array element cannot be reset; assign "
                                  "its members");
            }

            // Array<struct> 원소: 그 자리를 지우고 기본 원소를 넣는다.
            if (auto removed =
                    PathRemoveElement(Registry(), Access(), Box(), relative, nullptr, &failure);
                !removed) {
                return Failed(removed.Error());
            }

            auto inserted =
                PathInsertElement(Registry(), Access(), Box(), relative, nullptr, &failure);
            return inserted ? Changed() : Failed(inserted.Error());
        }

        if (fieldKind == ValueKind::Optional) {
            auto made = PathEmplaceOptional(Registry(), Access(), Box(), relative, &failure);
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
    if (relative.steps.Size() == 1) {
        unit.box.ResetToDefault();
        return Result<void>::Success();
    }

    DetachedValue inner(unit.box.GetAllocator());
    if (auto detached = PathDetach(Registry(), Access(), Box(), relative, inner, &failure);
        !detached) {
        return Failed(detached.Error());
    }

    inner.ResetToDefault();
    PreparedAttach attach;
    if (auto located =
            PathPrepareAttach(Registry(), Access(), Box(), relative, inner, attach, &failure);
        !located) {
        return Failed(located.Error());
    }

    // 옛 값은 inner가 받아 이 함수를 나갈 때 반환한다.
    return attach.Publish();
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
            if (auto written = PathWrite(Registry(), Access(), Box(), element,
                                         value.elements[index], nullptr, &failure);
                !written) {
                return Failed(written.Error());
            }
        }

        return Result<void>::Success();
    }

    if (auto cleared = PathClear(Registry(), Access(), Box(), relative, &failure); !cleared) {
        return Failed(cleared.Error());
    }

    if (fieldKind == ValueKind::Array) {
        last.selector = PathSelector::Index;
        for (usize index = 0; index < value.elements.Size(); ++index) {
            last.index = static_cast<uint32>(index);
            if (auto inserted = PathInsertElement(Registry(), Access(), Box(), element,
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
            auto inserted = PathInsertKey(Registry(), Access(), Box(), keyed, &failure);
            done = inserted ? Result<void>::Success() : Result<void>::Failure(inserted.Error());
        } else {
            done = PathWrite(Registry(), Access(), Box(), keyed, value.values[index], nullptr,
                             &failure);
        }

        if (!done) {
            return Failed(done.Error());
        }
    }

    return Result<void>::Success();
}

// 경로의 준비 단위 끝 단계다: 위에서부터 처음 만나는 참조 없는 필드·멤버. 강한 참조 자리(ObjectRef
// 필드·참조 멤버·Registry 컨테이너·참조 struct 통째)면 IndexNone이다.
Result<usize> FindUnitEnd(AssignmentState& state, PropertyPath path) noexcept {
    for (usize index = 0; index < path.steps.Size(); ++index) {
        TextSlot slot;
        const PropertyPath prefix{Span<const PathStep>(path.steps.Data(), index + 1), path.keys};
        if (auto described = DescribeTextPath(state.lease, prefix, slot); !described) {
            return Result<usize>::Failure(described.Error());
        }

        const FieldSchema& field = *slot.field;
        const bool copyable =
            FieldTargetType(field).kind != ValueKind::ObjectReference &&
            (slot.structView.IsEmpty() || !slot.structView.ContainsStrongReference());
        if (copyable) {
            return Result<usize>::Success(index);
        }

        // 참조를 담은 인라인 struct만 멤버로 내려간다(원소·항목 안은 참조 단위 — B9i3c2).
        if (path.steps[index].selector != PathSelector::None ||
            field.value.kind != ValueKind::Struct) {
            break;
        }
    }

    return Result<usize>::Success(IndexNone);
}

} // namespace

Result<void> ProcessAssignment(AssignmentState& state, const TextAssignment& assignment) noexcept {
    const TextSource source = TextSource::Assignment(assignment.sourceIndex);
    TextPath resolved;
    auto made = ResolveTextPath(state.context, state.lease, assignment.path, source, resolved);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    if (!made.Value()) {
        state.failed = true;
        return Result<void>::Success();
    }

    const PropertyPath path = resolved.path.View();
    PathKey keys[MaxPropertyPathSteps];
    for (usize index = 0; index < state.assigned.Count(); ++index) {
        if (SamePropertyPath(state.assigned.View(index, keys), path)) {
            return Diagnose(state, TextDiagnosticCode::DuplicateAssignment, source, assignment.path,
                            "the same path is assigned twice");
        }
    }

    if (auto kept = state.assigned.Append(path); !kept) {
        return kept;
    }

    auto unitEnd = FindUnitEnd(state, path);
    if (!unitEnd) {
        return Result<void>::Failure(unitEnd.Error());
    }

    if (unitEnd.Value() == IndexNone) {
        return Diagnose(state, TextDiagnosticCode::Unsupported, source, assignment.path,
                        "assignments to strong references are not supported yet");
    }

    // 단위 경로는 끝 단계의 선택자를 뗀 필드·멤버 하나다(앞 단계는 모두 인라인 struct라 key가
    // 없다).
    const usize end = unitEnd.Value();
    PathStep unitSteps[MaxPropertyPathSteps];
    for (usize index = 0; index <= end; ++index) {
        unitSteps[index] = path.steps[index];
    }

    unitSteps[end].selector = PathSelector::None;
    unitSteps[end].index = 0;
    auto unit = AcquireUnit(state, PropertyPath{Span<const PathStep>(unitSteps, end + 1), {}},
                            source, assignment.path);
    if (!unit || !unit.Value()) {
        return unit ? Result<void>::Success() : Result<void>::Failure(unit.Error());
    }

    Apply apply{state,      *unit.Value(),
                assignment, resolved.slot,
                path,       PropertyPath{path.steps.Mid(end), path.keys},
                source,     {}};
    return apply.Run();
}

} // namespace rocks::reflection_text::detail
