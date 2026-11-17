#include "TextAssignApplyInternal.h"
#include "TextJsonInternal.h"

// 통째 값의 같은 값 판정과 ReadOnly 자리의 호출 끝 대조다(리플렉션 B9i 리뷰 반영 —
// Docs/Design/ReflectionExtensions.ko.md 10.2절 "리뷰 반영에서 정한 점"). 목록·항목 목록 대입은
// 지금 내용과 원소마다 비교해 같으면 무동작이고(SameContents), 값 단위의 ReadOnly 자리 대입은
// 상자에 적용한 뒤 호출(PrepareAssignments·Append·ImportJson) 끝에 그 자리의 상자 값과 대상의 지금
// 값을 스키마를 따라 깊게 대조한다(CheckReadOnlyUnits). 그래서 `opt={}` 뒤 `opt.x=5`나 struct 원소
// 배열의 `a=` 뒤 `a[i]={}`·멤버 대입처럼 여러 대입이 함께 지금 값을 다시 만드는 왕복(export →
// 같은 객체 import, asset-info → asset-set)이 무동작으로 통과한다.
namespace rocks::reflection_text::detail {

namespace {

// 읽기 실패를 대조 결과로 옮긴다: 호출 오류는 그 오류, 그 밖(없는 자리·모양 불일치)은 다르다.
Result<bool> ReadFailure(ErrorCode code) noexcept {
    return IsCallError(code) ? Result<bool>::Failure(code) : Result<bool>::Success(false);
}

// 상자 값과 대상의 지금 값을 같은 자리에서 깊게 대조한다. path는 대상 기준 경로이고 상자 경로는
// 그 앞 offset단계(단위 필드 앞)를 뗀 것이다.
struct Comparer {
    AssignmentState& state;
    PathTarget live;
    PathTarget box;
    usize offset;
    JsonPathBuilder path;

    ObjectRegistry& Registry() const noexcept {
        return state.target.Registry();
    }

    const ObjectAccessContext& Access() const noexcept {
        return state.target.Access();
    }

    PropertyPath LivePath() const noexcept {
        return path.View();
    }

    PropertyPath BoxPath() const noexcept {
        const PropertyPath full = path.View();
        return PropertyPath{full.steps.Mid(offset), full.keys};
    }

    Result<bool> SameField(const TextSlot& slot) noexcept;
    Result<bool> SameStruct(StructView view, uint32 flags) noexcept;
    Result<bool> SameInstanced(uint32 flags) noexcept;
    Result<bool> SameArray(const TextSlot& slot) noexcept;
    Result<bool> SameKeyed(const TextSlot& slot) noexcept;
    Result<bool> SameRead() noexcept;
};

Result<bool> Comparer::SameField(const TextSlot& slot) noexcept {
    const FieldSchema& field = *slot.field;
    switch (field.value.kind) {
    case ValueKind::Struct:
        return SameStruct(slot.structView, slot.flags);
    case ValueKind::Optional: {
        if (field.element.kind != ValueKind::Struct) {
            return SameRead();
        }

        auto left = PathIsOptionalSet(Registry(), Access(), live, LivePath());
        if (!left) {
            return ReadFailure(left.Error());
        }

        auto right = PathIsOptionalSet(Registry(), Access(), box, BoxPath());
        if (!right) {
            return ReadFailure(right.Error());
        }

        if (left.Value() != right.Value()) {
            return Result<bool>::Success(false);
        }

        return left.Value() ? SameStruct(slot.structView, slot.flags) : Result<bool>::Success(true);
    }
    case ValueKind::InstancedStruct:
        return SameInstanced(slot.flags);
    case ValueKind::Array:
    case ValueKind::StaticArray:
        return SameArray(slot);
    case ValueKind::Map:
    case ValueKind::Set:
        return SameKeyed(slot);
    case ValueKind::Opaque:
        // 경로 연산으로 읽을 수 없는 값은 같다고 볼 수 없다(보수적으로 다르다).
        return Result<bool>::Success(false);
    default:
        return SameRead();
    }
}

Result<bool> Comparer::SameStruct(StructView view, uint32 flags) noexcept {
    if (view.IsEmpty()) {
        return Result<bool>::Success(false);
    }

    const SchemaScope scope{nullptr, view};
    const auto& fields = view.Schema().fields;
    for (usize index = 0; index < fields.Size(); ++index) {
        const TextSlot member = FieldSlot(scope, index, flags);
        if (!path.Push(member.field->id)) {
            return Result<bool>::Success(false);
        }

        auto same = SameField(member);
        path.Pop();
        if (!same || !same.Value()) {
            return same;
        }
    }

    return Result<bool>::Success(true);
}

// InstancedStruct 값 자리(리플렉션 B9j)는 담긴 타입이 같고(둘 다 비어 있음 포함) 멤버가 같아야
// 한다.
Result<bool> Comparer::SameInstanced(uint32 flags) noexcept {
    auto left = PathReadInstancedType(Registry(), Access(), live, LivePath());
    if (!left) {
        return ReadFailure(left.Error());
    }

    auto right = PathReadInstancedType(Registry(), Access(), box, BoxPath());
    if (!right) {
        return ReadFailure(right.Error());
    }

    if (left.Value().IsValid() != right.Value().IsValid()) {
        return Result<bool>::Success(false);
    }

    if (!left.Value().IsValid()) {
        return Result<bool>::Success(true);
    }

    if (!(left.Value().Id() == right.Value().Id())) {
        return Result<bool>::Success(false);
    }

    return SameStruct(left.Value().Struct(), flags);
}

Result<bool> Comparer::SameArray(const TextSlot& slot) noexcept {
    const FieldSchema& field = *slot.field;
    usize count = field.maxElements;
    if (field.value.kind == ValueKind::Array) {
        auto left = PathCount(Registry(), Access(), live, LivePath());
        if (!left) {
            return ReadFailure(left.Error());
        }

        auto right = PathCount(Registry(), Access(), box, BoxPath());
        if (!right) {
            return ReadFailure(right.Error());
        }

        if (left.Value() != right.Value()) {
            return Result<bool>::Success(false);
        }

        count = left.Value();
    }

    const bool structElement = field.element.kind == ValueKind::Struct;
    for (usize index = 0; index < count; ++index) {
        path.SetIndex(static_cast<uint32>(index));
        auto same = structElement ? SameStruct(slot.structView, slot.flags)
                    : field.element.kind == ValueKind::InstancedStruct ? SameInstanced(slot.flags)
                                                                       : SameRead();
        if (!same || !same.Value()) {
            path.ClearSelector();
            return same;
        }
    }

    path.ClearSelector();
    return Result<bool>::Success(true);
}

Result<bool> Comparer::SameKeyed(const TextSlot& slot) noexcept {
    const FieldSchema& field = *slot.field;
    Array<FieldValue> left(state.allocator);
    Array<FieldValue> right(state.allocator);
    if (auto read = PathReadKeys(Registry(), Access(), live, LivePath(), left); !read) {
        return ReadFailure(read.Error());
    }

    if (auto read = PathReadKeys(Registry(), Access(), box, BoxPath(), right); !read) {
        return ReadFailure(read.Error());
    }

    if (left.Size() != right.Size()) {
        return Result<bool>::Success(false);
    }

    // 순서도 값의 일부다(삽입 순서 — export 출력이 따른다).
    for (usize index = 0; index < left.Size(); ++index) {
        if (!SameValue(left[index], right[index])) {
            return Result<bool>::Success(false);
        }
    }

    if (field.value.kind == ValueKind::Set) {
        return Result<bool>::Success(true);
    }

    const bool structValue = field.element.kind == ValueKind::Struct;
    for (usize index = 0; index < left.Size(); ++index) {
        if (auto selected = path.SetKey(state.options.names, state.allocator, left[index]);
            !selected) {
            return Result<bool>::Failure(selected.Error());
        }

        auto same = structValue ? SameStruct(slot.structView, slot.flags)
                    : field.element.kind == ValueKind::InstancedStruct ? SameInstanced(slot.flags)
                                                                       : SameRead();
        path.ClearSelector();
        if (!same || !same.Value()) {
            return same;
        }
    }

    return Result<bool>::Success(true);
}

Result<bool> Comparer::SameRead() noexcept {
    FieldValue left(state.allocator);
    FieldValue right(state.allocator);
    if (auto read = PathRead(Registry(), Access(), live, LivePath(), left); !read) {
        return ReadFailure(read.Error());
    }

    if (auto read = PathRead(Registry(), Access(), box, BoxPath(), right); !read) {
        return ReadFailure(read.Error());
    }

    return Result<bool>::Success(SameValue(left, right));
}

} // namespace

Result<bool> SameContents(AssignmentState& state, const PathTarget& start, PropertyPath path,
                          const FieldSchema& field, const TextValue& value) noexcept {
    ObjectRegistry& registry = state.target.Registry();
    const ObjectAccessContext& access = state.target.Access();
    const ValueKind kind = field.value.kind;
    // struct 원소·값은 목록 텍스트로 나타나지 않는다(빈 목록만 — 비교는 원소 수뿐이다).
    const bool structElement = field.element.kind == ValueKind::Struct;
    if (structElement && !value.elements.IsEmpty()) {
        return Result<bool>::Success(false);
    }

    const usize stepCount = path.steps.Size();
    if (stepCount == 0 || stepCount > MaxPropertyPathSteps ||
        path.keys.Size() >= MaxPropertyPathSteps) {
        return Result<bool>::Success(false);
    }

    PathStep steps[MaxPropertyPathSteps];
    PathKey keys[MaxPropertyPathSteps];
    for (usize index = 0; index < stepCount; ++index) {
        steps[index] = path.steps[index];
    }

    for (usize index = 0; index < path.keys.Size(); ++index) {
        keys[index] = path.keys[index];
    }

    PathStep& last = steps[stepCount - 1];
    FieldValue current(state.allocator);
    if (kind == ValueKind::Array || kind == ValueKind::StaticArray) {
        usize count = field.maxElements;
        if (kind == ValueKind::Array) {
            auto counted = PathCount(registry, access, start, path);
            if (!counted) {
                return ReadFailure(counted.Error());
            }

            count = counted.Value();
        }

        if (count != value.elements.Size()) {
            return Result<bool>::Success(false);
        }

        const PropertyPath element{Span<const PathStep>(steps, stepCount), path.keys};
        last.selector = PathSelector::Index;
        for (usize index = 0; index < count; ++index) {
            last.index = static_cast<uint32>(index);
            if (auto read = PathRead(registry, access, start, element, current); !read) {
                return ReadFailure(read.Error());
            }

            if (!SameValue(current, value.elements[index])) {
                return Result<bool>::Success(false);
            }
        }

        return Result<bool>::Success(true);
    }

    if (kind != ValueKind::Set && kind != ValueKind::Map) {
        return Result<bool>::Success(false);
    }

    Array<FieldValue> present(state.allocator);
    if (auto read = PathReadKeys(registry, access, start, path, present); !read) {
        return ReadFailure(read.Error());
    }

    if (present.Size() != value.elements.Size()) {
        return Result<bool>::Success(false);
    }

    for (usize index = 0; index < present.Size(); ++index) {
        if (!SameValue(present[index], value.elements[index])) {
            return Result<bool>::Success(false);
        }
    }

    if (kind == ValueKind::Set || structElement) {
        return Result<bool>::Success(true);
    }

    // Map 값은 key 경로로 읽어 비교한다.
    const usize keyCount = path.keys.Size();
    const PropertyPath keyed{Span<const PathStep>(steps, stepCount),
                             Span<const PathKey>(keys, keyCount + 1)};
    Array<uint8> wire(state.allocator);
    last.selector = PathSelector::Key;
    last.index = static_cast<uint32>(keyCount);
    for (usize index = 0; index < present.Size(); ++index) {
        wire.Clear();
        if (auto made = AppendKeyWire(state, present[index], wire); !made) {
            return Result<bool>::Failure(made.Error());
        }

        keys[keyCount] = PathKey{present[index].kind, wire.View()};
        if (auto read = PathRead(registry, access, start, keyed, current); !read) {
            return ReadFailure(read.Error());
        }

        if (!SameValue(current, value.values[index])) {
            return Result<bool>::Success(false);
        }
    }

    return Result<bool>::Success(true);
}

Result<void> NoteReadOnly(AssignmentState& state, AssignmentUnit& unit, PropertyPath path,
                          usize end, TextSource source) noexcept {
    // 처음 ReadOnly가 붙는 단계를 찾는다(값 단위보다 위면 단위 필드·멤버 단계).
    usize anchor = path.steps.Size() - 1;
    for (usize index = 0; index < path.steps.Size(); ++index) {
        TextSlot slot;
        const PropertyPath prefix{Span<const PathStep>(path.steps.Data(), index + 1), path.keys};
        if (auto described = DescribeAssignmentPath(state, prefix, slot); !described) {
            return described;
        }

        if ((slot.flags & static_cast<uint32>(FieldFlags::ReadOnly)) != 0) {
            anchor = index;
            break;
        }
    }

    if (anchor < end) {
        anchor = end;
    }

    PathStep steps[MaxPropertyPathSteps];
    for (usize index = 0; index <= anchor; ++index) {
        steps[index] = path.steps[index];
    }

    steps[anchor].selector = PathSelector::None;
    steps[anchor].index = 0;
    if (auto reserved = unit.readOnlySources.TryReserve(unit.readOnlySources.Size() + 1);
        !reserved) {
        return reserved;
    }

    auto added =
        unit.readOnlyPaths.Add(PropertyPath{Span<const PathStep>(steps, anchor + 1), path.keys});
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    if (added.Value()) {
        (void)unit.readOnlySources.TryPushBack(source);
    }

    unit.box.AcceptReadOnlyWrites();
    return Result<void>::Success();
}

Result<void> CheckReadOnlyUnits(AssignmentState& state) noexcept {
    for (usize index = 0; index < state.units.Size(); ++index) {
        AssignmentUnit& unit = *state.units[index];
        for (usize entry = 0; entry < unit.readOnlyPaths.Count(); ++entry) {
            PathKey keys[MaxPropertyPathSteps];
            const PropertyPath anchor = unit.readOnlyPaths.View(entry, keys);
            TextSlot slot;
            if (auto described = DescribeAssignmentPath(state, anchor, slot); !described) {
                return described;
            }

            Comparer comparer{state, state.Root(), PathTarget::Value(unit.box),
                              unit.steps.Size() - 1, JsonPathBuilder(state.allocator)};
            if (auto assigned = comparer.path.Assign(anchor, state.allocator); !assigned) {
                return assigned;
            }

            auto same = comparer.SameField(slot);
            if (!same) {
                return Result<void>::Failure(same.Error());
            }

            if (same.Value()) {
                continue;
            }

            String text(state.allocator);
            if (auto formatted = FormatAssignmentPath(state, anchor, text); !formatted) {
                return formatted;
            }

            if (auto reported =
                    Diagnose(state, TextDiagnosticCode::ReadOnly, unit.readOnlySources[entry],
                             text.View(), "property is read-only");
                !reported) {
                return reported;
            }
        }

        unit.readOnlyPaths.Clear();
        unit.readOnlySources.Clear();
    }

    return Result<void>::Success();
}

} // namespace rocks::reflection_text::detail
