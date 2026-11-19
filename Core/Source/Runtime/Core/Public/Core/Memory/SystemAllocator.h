#pragma once

#include <Core/Memory/Allocator.h>
#include <Platform/Sync.h>

namespace rocks {

namespace detail {
struct SystemAllocatorTestAccess;
} // namespace detail

// 단순 잠금 기반의 기준 구현이다. Allocate/Deallocate/Stats의 ledger 접근은 동기화하지만
// 반환 메모리의 내용이나 allocator 자체의 파괴를 다른 호출과 동기화해 주지는 않는다.
// 살아 있는 allocation은 주소 원장(hash 표 — Docs/Design/GarbageCollectionScale.ko.md 18.6절)에
// 적는다. 원장 저장소는 독립 OS 할당이라 자기 추적의 재귀가 없다. Deallocate의 조회는 상환
// O(1)이지만 모든 호출이 잠금 하나를 지나므로 최적화 allocator를 뜻하지 않는다(빠른 경로는
// BinnedAllocator다).
//
// 사용 예:
//   SystemAllocator memory;
//   auto block = memory.Allocate(64, 16);
//   if (block) { /* 사용 */ (void)memory.Deallocate(block.Value()); }
class SystemAllocator final : public Allocator {
public:
    SystemAllocator() noexcept;
    // 모든 호출과 사용자를 끝내고 live allocation이 0일 때만 파괴한다. 잔존 시 Fatal로 진단한다.
    ~SystemAllocator() noexcept override;

    SystemAllocator(const SystemAllocator&) = delete;
    SystemAllocator& operator=(const SystemAllocator&) = delete;

    // 잘못된 정렬은 InvalidArgument, 크기 계산 넘침은 Overflow, OS 할당이나 원장 넣기(표 늘리기)
    // 실패는 OutOfMemory다. 실패는 통계와 원장을 바꾸지 않는다.
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override;
    // 원장에서 주소를 찾지 못하면 역참조 없이 NotFound를 반환한다.
    // foreign·중복 해제를 구분하지 않으며 주소 재사용 이후 stale pointer의 식별은 보장하지 않는다.
    Result<void> Deallocate(void* address) noexcept override;
    // 한 시점의 ledger snapshot이다. 0이라는 관찰만으로 다른 thread의 호출 종료가 보장되지는
    // 않는다.
    AllocationStats Stats() const noexcept;

private:
    friend struct detail::SystemAllocatorTestAccess;
    // 주소 원장과 그 저장소 allocator다. 타입은 구현 파일에만 둔다(이 헤더가 HashMap을 끌어오지
    // 않게 — 크기·정렬은 구현 파일이 static_assert한다).
    struct Ledger;
    static constexpr usize LedgerStorageBytes = 128;
    static constexpr usize LedgerStorageAlignment = 16;

    Ledger& LedgerState() noexcept;
    // 시험 통로(detail::SystemAllocatorTestAccess)만 부른다: 원장 저장소의 실패 주입(지금부터
    // oneBasedAttempt번째 할당을 실패시킨다, 0이면 끈다)과 원장에 적힌 주소 수다.
    void FailLedgerStorageOnAttempt(uint64 oneBasedAttempt) noexcept;
    usize LedgerCount() noexcept;

    mutable platform::Mutex mutex_;
    alignas(LedgerStorageAlignment) unsigned char ledger_[LedgerStorageBytes]{};
    AllocationStats stats_{};
};

} // namespace rocks
