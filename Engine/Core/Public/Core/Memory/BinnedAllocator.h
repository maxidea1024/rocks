#pragma once

#include <Core/Memory/PoolAllocator.h>

namespace rocks {

// 여러 고정 크기 pool과 상위 allocator를 함께 사용하는 범용 저장소다.
// 사용 예: BinnedAllocator memory(system); auto p = memory.Allocate(48, 16);
// 객체는 직접 소멸하고, 반환받은 정확한 주소를 이 allocator에 Deallocate해야 한다.
// backing은 이 객체 및 모든 allocation보다 오래 살고 호출 스레드에서 사용 가능해야 한다.
//
// 16~4096 byte의 9개 크기 구간은 16 byte 이하 정렬에 사용한다. 더 큰 크기나 정렬은
// backing에 직접 요청한다. pool과 소유권 원장의 잠금 및 선형 검색이 있으므로 성능 보장은
// 없다. 사용자와 상위 chunk의 원장은 플랫폼 할당을 사용하며 backing의 예산에 포함되지 않는다.
// 이 원장의 플랫폼 할당 실패는 현재 테스트에서 직접 주입할 수 없다. 상위 allocator가 성공과
// 함께 null, 잘못된 정렬 또는 이 allocator가 보유한 상위 저장소·원장과 겹친 범위를
// 반환하면 Fatal이다. 다른 고객의 저장소와 겹치지 않는 것은 backing의 책임이다.
// 동시 호출은 직렬화되고 상위 호출도 잠금 안에서 수행된다. 같은 스레드에서 재진입하면
// Result API는 Busy, Stats와 파괴는 Fatal이다. 상위 콜백은 다른 스레드의 이 allocator
// 작업을 기다리거나 allocator 사이에 역순 잠금 순환을 만들면 안 된다.
// 파괴와 사용자 memory 접근은 호출자가 동기화해야 한다. 반환하지 않은 allocation이 있으면
// 파괴 시 Fatal이다. raw 주소가 재사용된 뒤에는 오래된 포인터를 새 포인터와 구분할 수 없다.
class BinnedAllocator final : public Allocator {
public:
    explicit BinnedAllocator(Allocator& backing) noexcept;
    ~BinnedAllocator() noexcept override;

    BinnedAllocator(const BinnedAllocator&) = delete;
    BinnedAllocator& operator=(const BinnedAllocator&) = delete;
    BinnedAllocator(BinnedAllocator&&) = delete;
    BinnedAllocator& operator=(BinnedAllocator&&) = delete;

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override;
    // 미소유·중복·중간 주소는 역참조하지 않고 NotFound다. 상위 반환 실패 시 기록을 보존한다.
    Result<void> Deallocate(void* address) noexcept override;

    // 모든 allocation이 반환된 경우에만 pool storage를 회수한다. Busy이면 상태를 유지한다.
    // 상위 allocator가 pool의 소유 storage 반환을 거부하면 PoolAllocator 계약에 따라 Fatal이다.
    Result<void> Trim() noexcept;
    // 사용 중이면 Busy이고 열린 상태를 유지한다. 성공 후 재호출은 성공한다.
    Result<void> Shutdown() noexcept;
    AllocationStats Stats() const noexcept;

private:
    struct Record;
    struct EntryScope;
    class CheckedBacking final : public Allocator {
    public:
        CheckedBacking(BinnedAllocator& owner, Allocator& backing) noexcept;
        ~CheckedBacking() noexcept override;
        Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override;
        Result<void> Deallocate(void* address) noexcept override;

    private:
        struct BackingRecord;
        BinnedAllocator& owner_;
        Allocator& backing_;
        BackingRecord* records_ = nullptr;
    };
    static thread_local EntryScope* activeEntry_;
    static constexpr usize BinCount = 9;

    CheckedBacking backing_;
    PoolAllocator pools_[BinCount];
    mutable platform::Mutex mutex_;
    Record* records_ = nullptr;
    void* pendingRecord_ = nullptr;
    AllocationStats stats_{};
    bool closed_ = false;
};

} // namespace rocks
