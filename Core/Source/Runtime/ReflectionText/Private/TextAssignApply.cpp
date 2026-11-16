#include "TextAssignApplyInternal.h"

#include <Object/NameField.h>
#include <Object/PathKey.h>

// 대입 하나를 해석해 준비 단위 사본에 적용한다(리플렉션 B9i3c1 — ReflectionText/TextAssignments.h의
// PrepareAssignments 규칙). 경로 해석 → 같은 경로 검사 → 준비 단위(참조 없는 필드·멤버 하나, 또는
// 단일 강한 참조 위치 — B9i3c2) → 값 읽기(resolver) → 사본 위 경로 연산 또는 참조 자리 검증 →
// override 경로.
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

} // namespace

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

// 새로 가리키는 Hard·Weak 대상을 고정한다(Publish가 다시 확인한다 — 고정은 GC 회수를 막지만
// Discard는 막지 못한다). 살아 있지 않으면 진단이다.
Result<bool> PinReferences(AssignmentState& state, const TextValue& value, TextSource source,
                           StringView pathText) noexcept {
    const FieldValue* lists[] = {&value.single};
    const Span<const FieldValue> groups[] = {Span<const FieldValue>(lists[0], 1),
                                             value.elements.View(), value.values.View()};
    for (const Span<const FieldValue>& group : groups) {
        for (usize index = 0; index < group.Size(); ++index) {
            const FieldValue& item = group[index];
            if ((item.kind != ValueKind::WeakObjectReference &&
                 item.kind != ValueKind::ObjectReference) ||
                item.reference.IsNull()) {
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

namespace {

enum class UnitKind : uint8 {
    None,
    Value,
    Reference,
    Container
};

struct UnitEnd {
    usize index = IndexNone;
    UnitKind kind = UnitKind::None;
};

// 경로의 준비 단위다: 위에서부터 처음 만나는 참조 없는 필드·멤버(값 단위), 단일 강한 참조
// 위치(ObjectRef 필드·멤버, 정적 배열의 ObjectRef 원소 — 참조 단위), ObjectRefArray·ObjectRefMap(그
// 원소·항목 포함 — 컨테이너 단위). 참조를 담은 인라인 struct와 정적 배열·StructRefArray의 struct
// 원소로는 내려간다. 그 밖(StructRefArray 통째·원소 통째·참조 struct 통째)은 None이다.
Result<UnitEnd> FindUnitEnd(AssignmentState& state, PropertyPath path) noexcept {
    for (usize index = 0; index < path.steps.Size(); ++index) {
        TextSlot slot;
        const PropertyPath prefix{Span<const PathStep>(path.steps.Data(), index + 1), path.keys};
        if (auto described = DescribeTextPath(state.lease, prefix, slot); !described) {
            return Result<UnitEnd>::Failure(described.Error());
        }

        const FieldSchema& field = *slot.field;
        const ValueKind target = FieldTargetType(field).kind;
        const bool copyable =
            target != ValueKind::ObjectReference &&
            (slot.structView.IsEmpty() || !slot.structView.ContainsStrongReference());
        if (copyable) {
            return Result<UnitEnd>::Success(UnitEnd{index, UnitKind::Value});
        }

        const PathSelector selector = path.steps[index].selector;
        const bool last = index + 1 == path.steps.Size();
        // ObjectRefArray·ObjectRefMap(통째·원소·항목)는 컨테이너 단위다(리플렉션 B9i3c3).
        const bool registryContainer =
            target == ValueKind::ObjectReference &&
            (field.value.kind == ValueKind::Array || field.value.kind == ValueKind::Map);
        if (registryContainer) {
            return Result<UnitEnd>::Success(last ? UnitEnd{index, UnitKind::Container} : UnitEnd{});
        }

        const bool single =
            (field.value.kind == ValueKind::ObjectReference && selector == PathSelector::None) ||
            (field.value.kind == ValueKind::StaticArray && selector == PathSelector::Index);
        if (last && target == ValueKind::ObjectReference && single) {
            return Result<UnitEnd>::Success(UnitEnd{index, UnitKind::Reference});
        }

        const bool element =
            selector == PathSelector::Index && field.element.kind == ValueKind::Struct &&
            (field.value.kind == ValueKind::StaticArray || field.value.kind == ValueKind::Array);
        const bool member = selector == PathSelector::None && field.value.kind == ValueKind::Struct;
        if (last || !(element || member)) {
            break;
        }
    }

    return Result<UnitEnd>::Success(UnitEnd{});
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

    if (unitEnd.Value().kind == UnitKind::None) {
        return ApplyReferenceStruct(state, assignment, resolved.slot, path, source);
    }

    if (unitEnd.Value().kind == UnitKind::Reference) {
        return ApplyReference(state, assignment, resolved.slot, path, source);
    }

    // 단위 경로는 끝 단계의 선택자를 뗀 필드·멤버 하나다(앞 단계는 인라인 struct와 정적 배열·
    // StructRefArray 원소라 key가 없다).
    const usize end = unitEnd.Value().index;
    PathStep unitSteps[MaxPropertyPathSteps];
    for (usize index = 0; index <= end; ++index) {
        unitSteps[index] = path.steps[index];
    }

    unitSteps[end].selector = PathSelector::None;
    unitSteps[end].index = 0;
    const PropertyPath unitPath{Span<const PathStep>(unitSteps, end + 1), {}};
    if (unitEnd.Value().kind == UnitKind::Container) {
        // 컨테이너 단위는 대상 기준 경로 그대로 준비 사본에 적용한다.
        auto container = AcquireContainer(state, unitPath, source, assignment.path);
        if (!container || !container.Value()) {
            return container ? Result<void>::Success() : Result<void>::Failure(container.Error());
        }

        ContainerUnit& unit = *container.Value();
        Apply apply{state,      state.Root().WithReferences(unit.stage),
                    unit.dirty, nullptr,
                    assignment, resolved.slot,
                    path,       path,
                    source,     {}};
        return apply.Run();
    }

    auto unit = AcquireUnit(state, unitPath, source, assignment.path);
    if (!unit || !unit.Value()) {
        return unit ? Result<void>::Success() : Result<void>::Failure(unit.Error());
    }

    Apply apply{state,
                PathTarget::Value(unit.Value()->box),
                unit.Value()->dirty,
                &unit.Value()->box,
                assignment,
                resolved.slot,
                path,
                PropertyPath{path.steps.Mid(end), path.keys},
                source,
                {}};
    return apply.Run();
}

} // namespace rocks::reflection_text::detail
