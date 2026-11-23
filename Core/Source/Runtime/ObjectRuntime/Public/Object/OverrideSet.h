#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Error.h>
#include <Meta/PropertyPathSchema.h>

// 객체 하나의 override 집합이다(Docs/Design/RuntimeClassMetadata.ko.md '클래스 기본값 구현 명세 —
// A5b'). 원소는 override 단위 경로(필드 ID 사슬 — 최상위 단일 값·배열 계열 필드, 비Atomic struct는
// 잎 멤버까지, Atomic struct는 그 struct 경로)다. 차이 쓰기는 기준과 같은 값이라도 단위 경로의 값은
// 쓴다(저장할 값 = 집합 ∪ 기준과 다른 값). 편집기 패키지 집합이 객체마다 가진다(런타임에는 없다).
//
// 사용 예:
//   OverrideSet set(allocator);
//   PropertyPathBuffer path;   // ParsePropertyPath(lease, "point.x", path)
//   if (auto added = AddOverrideUnits(lease, Span<const PathStep>(path.steps, path.count), set);
//       !added) { ... }
//   if (set.Contains(steps)) { /* 기준과 같아도 쓴다 */ }
namespace rocks {

class TypeLease;

class ROCKS_OBJECTRUNTIME_API OverrideSet {
public:
    explicit OverrideSet(Allocator& allocator) noexcept;
    OverrideSet(OverrideSet&&) noexcept = default;
    OverrideSet& operator=(OverrideSet&&) noexcept = default;
    OverrideSet(const OverrideSet&) = delete;
    OverrideSet& operator=(const OverrideSet&) = delete;

    // 단위 경로를 더한다(이미 있으면 그대로 성공). 단위들은 (필드 ID 사슬) 사전 순으로 정렬해 둔다
    // (같은 집합이면 순서가 같다). 오류: 빈 경로·MaxPropertyPathSteps 초과 InvalidArgument, 할당
    // 실패(집합은 그대로).
    Result<void> Add(Span<const Guid> unit) noexcept;
    // 단위 units개·필드 ID ids개를 더할 자리를 미리 잡는다(그 안의 Add는 할당하지 않아 실패하지
    // 않는다 — 값 게시 뒤의 기록, RuntimeClassMetadata 'A5b'). 오류: 할당 실패.
    Result<void> TryReserve(usize units, usize ids) noexcept;
    // 필드 ID 수의 합이다(예약 크기 계산).
    usize IdCount() const noexcept;
    // 단위 경로 하나를 지운다(없으면 아무것도 하지 않는다).
    void Remove(Span<const Guid> unit) noexcept;
    // prefix로 시작하는 단위를 모두 지운다(prefix 자체 포함 — 되돌리기의 struct 통째).
    void RemoveUnder(Span<const Guid> prefix) noexcept;
    // 단위가 있는지다. path 판은 경로 단계의 필드 ID만 본다(원소 단계가 없는 위치 — 차이 쓰기의
    // 필드·멤버 위치).
    bool Contains(Span<const Guid> unit) const noexcept;
    bool Contains(Span<const PathStep> path) const noexcept;

    usize Count() const noexcept {
        return starts_.Size();
    }

    bool IsEmpty() const noexcept {
        return starts_.IsEmpty();
    }

    // index번째 단위다(index < Count() — 정렬 순).
    Span<const Guid> At(usize index) const noexcept;

    // other의 단위로 바꾼다(복제·ID 수정 사본). 오류: 할당 실패(집합은 그대로).
    Result<void> TryAssign(const OverrideSet& other) noexcept;

private:
    // 단위 index의 위치 — 정렬 순에서 unit이 들어갈 자리와 같은지.
    usize LowerBound(Span<const Guid> unit, bool& found) const noexcept;

    // 단위들의 필드 ID를 이어 둔 것과 각 단위의 시작·길이다.
    Array<Guid> ids_;
    Array<uint32> starts_;
    Array<uint32> lengths_;
};

// path(lease 스키마로 해석한 경로)를 override 단위로 정규화해 set에 더한다: 원소·key 단계(index·
// key selector)는 그 필드에서 자르고, 처음 만나는 Atomic struct는 그 struct 경로에서 자르며, 끝이
// 비Atomic struct면 그 아래 잎 멤버(Transient 말고) 경로로 모두 펼친다(struct 통째 쓰기는 멤버마다
// — 결정: struct 필드 자체는 단위가 아니다). 오류: 빈 경로 InvalidArgument, 스키마에 없는 단계
// NotFound, struct가 아닌 값 뒤의 단계 TypeMismatch, 할당 실패(그때까지 더한 단위는 남는다).
ROCKS_OBJECTRUNTIME_API Result<void>
AddOverrideUnits(const TypeLease& lease, Span<const PathStep> path, OverrideSet& set) noexcept;

// 차이 형식 로드의 present 경로 하나(필드 ID 사슬 — 원소 단계 없음)를 단위로
// 더한다(RuntimeClassMetadata 'A5b'): 끝이 비Atomic struct면 버리고(그 잎 멤버가 따로 온다), Atomic
// struct 안 멤버는 그 struct 경로로 자른다. 스키마에 없는 단계(모르는 필드·마이그레이션의 옛
// 필드)나 정규화할 수 없는 경로는 오류가 아니라 무시한다. 오류: 할당 실패.
ROCKS_OBJECTRUNTIME_API Result<void>
AddPresentUnit(const TypeLease& lease, Span<const Guid> fieldIds, OverrideSet& set) noexcept;

// AddOverrideUnits와 같은 정규화로 단위를 지운다(되돌리기 — 비Atomic struct 끝이면 그 아래 모두).
// 오류: AddOverrideUnits와 같다(할당 없음).
ROCKS_OBJECTRUNTIME_API Result<void>
RemoveOverrideUnits(const TypeLease& lease, Span<const PathStep> path, OverrideSet& set) noexcept;

} // namespace rocks
