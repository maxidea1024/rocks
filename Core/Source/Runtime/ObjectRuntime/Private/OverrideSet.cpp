#include <Base/Guid.h>
#include <Object/OverrideSet.h>
#include <Object/TypeLease.h>

// override 집합과 단위 정규화다(Docs/Design/RuntimeClassMetadata.ko.md '클래스 기본값 구현 명세 —
// A5b'). 단위 필드 ID는 ids_에 덧붙이기만 하고(지울 때 남는 자리는 TryAssign이 정리한다) 정렬은
// starts_·lengths_ 색인이 맡는다 — 더하기가 범위 삽입 없이 실패 없는 한 칸 삽입이다.
namespace rocks {
namespace {

// 두 필드 ID 사슬의 사전 순 비교(<0·0·>0 — 짧은 쪽이 앞부분이면 앞).
int CompareUnits(Span<const Guid> left, Span<const Guid> right) noexcept {
    const usize count = left.Size() < right.Size() ? left.Size() : right.Size();
    for (usize index = 0; index < count; ++index) {
        if (GuidLess(left[index], right[index])) {
            return -1;
        }

        if (GuidLess(right[index], left[index])) {
            return 1;
        }
    }

    if (left.Size() == right.Size()) {
        return 0;
    }

    return left.Size() < right.Size() ? -1 : 1;
}

bool StartsWith(Span<const Guid> unit, Span<const Guid> prefix) noexcept {
    if (unit.Size() < prefix.Size()) {
        return false;
    }

    for (usize index = 0; index < prefix.Size(); ++index) {
        if (!(unit[index] == prefix[index])) {
            return false;
        }
    }

    return true;
}

// 정규화한 단위 하나를 받는 곳이다(더하기·지우기).
template <class Visit>
Result<void> ExpandLeaves(StructView view, Guid (&prefix)[MaxPropertyPathSteps], usize depth,
                          Visit& visit) noexcept {
    const TypeSchema& schema = view.Schema();
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        const FieldSchema& member = schema.fields[index];
        if (HasFlag(member.flags, FieldFlags::Transient)) {
            continue;
        }

        if (depth >= MaxPropertyPathSteps) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        prefix[depth] = member.id;
        const StructView nested = view.FieldStruct(index);
        if (member.value.kind == ValueKind::Struct && !nested.IsEmpty() && !nested.IsAtomic()) {
            if (auto expanded = ExpandLeaves(nested, prefix, depth + 1, visit); !expanded) {
                return expanded;
            }

            continue;
        }

        if (auto visited = visit(Span<const Guid>(prefix, depth + 1)); !visited) {
            return visited;
        }
    }

    return Result<void>::Success();
}

// path를 단위로 정규화해 visit(단위)를 부른다(펼침이면 여러 번).
template <class Visit>
Result<void> VisitUnits(const TypeLease& lease, Span<const PathStep> path, Visit& visit) noexcept {
    if (path.IsEmpty() || path.Size() > MaxPropertyPathSteps) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Guid unit[MaxPropertyPathSteps];
    StructView view{};
    for (usize step = 0; step < path.Size(); ++step) {
        const PathStep& at = path[step];
        const bool top = view.IsEmpty();
        const usize index = top ? lease.FieldIndex(at.fieldId) : view.FieldIndex(at.fieldId);
        if (index == TypeLease::NoField) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        const FieldSchema& field = top ? lease.Schema().fields[index] : view.Schema().fields[index];
        // 이벤트 필드는 값이 아니라 재정의 단위가 아니다(Docs/Design/EventReflection.ko.md 3.1절).
        if (IsEventField(field)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const StructView nested = top ? lease.FieldStruct(index) : view.FieldStruct(index);
        unit[step] = at.fieldId;
        const Span<const Guid> here(unit, step + 1);
        // 원소·key 단계는 그 필드가 단위다(배열·Map 필드 통째).
        if (at.selector != PathSelector::None) {
            return visit(here);
        }

        const bool singleStruct = field.value.kind == ValueKind::Struct && !nested.IsEmpty();
        if (!singleStruct || nested.IsAtomic()) {
            // 단일 값·배열 계열·Atomic struct, 그리고 단일 struct가 아닌 칸(InstancedStruct 내용·
            // Optional 안 — 뒤 단계가 그 안쪽)은 그 필드가 단위다(원소·key와 같은 규칙).
            return visit(here);
        }

        // 비Atomic struct: 끝이면 잎 멤버로 펼치고, 아니면 멤버로 내려간다.
        if (step + 1 == path.Size()) {
            return ExpandLeaves(nested, unit, step + 1, visit);
        }

        view = nested;
    }

    return Result<void>::Success();
}

} // namespace

OverrideSet::OverrideSet(Allocator& allocator) noexcept
    : ids_(allocator), starts_(allocator), lengths_(allocator) {
}

usize OverrideSet::LowerBound(Span<const Guid> unit, bool& found) const noexcept {
    usize low = 0;
    usize high = starts_.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (CompareUnits(At(middle), unit) < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    found = low < starts_.Size() && CompareUnits(At(low), unit) == 0;
    return low;
}

Result<void> OverrideSet::TryReserve(usize units, usize ids) noexcept {
    Result<void> step = ids_.TryReserve(ids_.Size() + ids);
    if (step) {
        step = starts_.TryReserve(starts_.Size() + units);
    }

    if (step) {
        step = lengths_.TryReserve(lengths_.Size() + units);
    }

    return step;
}

usize OverrideSet::IdCount() const noexcept {
    usize total = 0;
    for (uint32 length : lengths_) {
        total += length;
    }

    return total;
}

Span<const Guid> OverrideSet::At(usize index) const noexcept {
    return Span<const Guid>(ids_.Data() + starts_[index], lengths_[index]);
}

Result<void> OverrideSet::Add(Span<const Guid> unit) noexcept {
    if (unit.IsEmpty() || unit.Size() > MaxPropertyPathSteps) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    bool found = false;
    const usize position = LowerBound(unit, found);
    if (found) {
        return Result<void>::Success();
    }

    // 자리를 먼저 모두 잡는다(실패하면 집합은 그대로).
    Result<void> step = ids_.TryReserve(ids_.Size() + unit.Size());
    if (step) {
        step = starts_.TryReserve(starts_.Size() + 1);
    }

    if (step) {
        step = lengths_.TryReserve(lengths_.Size() + 1);
    }

    if (!step) {
        return step;
    }

    const auto start = static_cast<uint32>(ids_.Size());
    (void)ids_.TryAppend(unit);
    (void)starts_.TryInsert(start, position);
    (void)lengths_.TryInsert(static_cast<uint32>(unit.Size()), position);
    return Result<void>::Success();
}

void OverrideSet::Remove(Span<const Guid> unit) noexcept {
    bool found = false;
    const usize position = LowerBound(unit, found);
    if (found) {
        (void)starts_.TryRemoveAt(position);
        (void)lengths_.TryRemoveAt(position);
    }
}

void OverrideSet::RemoveUnder(Span<const Guid> prefix) noexcept {
    for (usize index = starts_.Size(); index > 0; --index) {
        if (StartsWith(At(index - 1), prefix)) {
            (void)starts_.TryRemoveAt(index - 1);
            (void)lengths_.TryRemoveAt(index - 1);
        }
    }
}

bool OverrideSet::Contains(Span<const Guid> unit) const noexcept {
    bool found = false;
    (void)LowerBound(unit, found);
    return found;
}

bool OverrideSet::Contains(Span<const PathStep> path) const noexcept {
    if (path.IsEmpty() || path.Size() > MaxPropertyPathSteps) {
        return false;
    }

    Guid unit[MaxPropertyPathSteps];
    for (usize index = 0; index < path.Size(); ++index) {
        unit[index] = path[index].fieldId;
    }

    return Contains(Span<const Guid>(unit, path.Size()));
}

Result<void> OverrideSet::TryAssign(const OverrideSet& other) noexcept {
    // 남는 자리 없이 정렬 순으로 다시 담는다.
    usize total = 0;
    for (usize index = 0; index < other.Count(); ++index) {
        total += other.lengths_[index];
    }

    Array<Guid> ids(ids_.GetAllocator());
    Array<uint32> starts(starts_.GetAllocator());
    Array<uint32> lengths(lengths_.GetAllocator());
    Result<void> step = ids.TryReserve(total);
    if (step) {
        step = starts.TryReserve(other.Count());
    }

    if (step) {
        step = lengths.TryReserve(other.Count());
    }

    if (!step) {
        return step;
    }

    for (usize index = 0; index < other.Count(); ++index) {
        (void)starts.TryPushBack(static_cast<uint32>(ids.Size()));
        (void)lengths.TryPushBack(other.lengths_[index]);
        (void)ids.TryAppend(other.At(index));
    }

    ids_ = Move(ids);
    starts_ = Move(starts);
    lengths_ = Move(lengths);
    return Result<void>::Success();
}

Result<void> AddOverrideUnits(const TypeLease& lease, Span<const PathStep> path,
                              OverrideSet& set) noexcept {
    auto add = [&](Span<const Guid> unit) noexcept { return set.Add(unit); };
    return VisitUnits(lease, path, add);
}

Result<void> AddPresentUnit(const TypeLease& lease, Span<const Guid> fieldIds,
                            OverrideSet& set) noexcept {
    if (fieldIds.IsEmpty() || fieldIds.Size() > MaxPropertyPathSteps) {
        return Result<void>::Success();
    }

    PathStep steps[MaxPropertyPathSteps];
    StructView view{};
    for (usize index = 0; index < fieldIds.Size(); ++index) {
        steps[index] = PathStep{fieldIds[index]};
        const bool top = view.IsEmpty();
        const usize at = top ? lease.FieldIndex(fieldIds[index]) : view.FieldIndex(fieldIds[index]);
        if (at == TypeLease::NoField) {
            return Result<void>::Success();
        }

        const FieldSchema& field = top ? lease.Schema().fields[at] : view.Schema().fields[at];
        const StructView nested = top ? lease.FieldStruct(at) : view.FieldStruct(at);
        const bool singleStruct = field.value.kind == ValueKind::Struct && !nested.IsEmpty();
        // 끝이 비Atomic struct면 그 struct 자체 present는 버린다(잎 멤버가 따로 온다).
        if (index + 1 == fieldIds.Size() && singleStruct && !nested.IsAtomic()) {
            return Result<void>::Success();
        }

        if (!singleStruct || nested.IsAtomic()) {
            break;
        }

        view = nested;
    }

    auto added = AddOverrideUnits(lease, Span<const PathStep>(steps, fieldIds.Size()), set);
    if (!added && added.Error() != ErrorCode::OutOfMemory) {
        return Result<void>::Success();
    }

    return added;
}

Result<void> RemoveOverrideUnits(const TypeLease& lease, Span<const PathStep> path,
                                 OverrideSet& set) noexcept {
    auto remove = [&](Span<const Guid> unit) noexcept {
        set.Remove(unit);
        return Result<void>::Success();
    };
    return VisitUnits(lease, path, remove);
}

} // namespace rocks
