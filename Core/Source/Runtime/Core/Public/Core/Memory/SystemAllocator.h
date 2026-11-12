#pragma once

#include <Core/Memory/Allocator.h>
#include <Platform/Sync.h>

namespace rocks {

// 단순 잠금 기반의 기준 구현이다. Allocate/Deallocate/Stats의 ledger 접근은 동기화하지만
// 반환 메모리의 내용이나 allocator 자체의 파괴를 다른 호출과 동기화해 주지는 않는다.
// 독립 OS 할당으로 ledger를 먼저 확보하여 자기 추적의 재귀와 기록 없는 allocation 게시를 피한다.
// 살아 있는 allocation의 선형 목록이므로 조회 비용은 O(n)이며 최적화 allocator를 뜻하지 않는다.
class SystemAllocator final : public Allocator {
public:
    SystemAllocator() noexcept = default;
    // 모든 호출과 사용자를 끝내고 live allocation이 0일 때만 파괴한다. 잔존 시 Fatal로 진단한다.
    ~SystemAllocator() noexcept override;

    SystemAllocator(const SystemAllocator&) = delete;
    SystemAllocator& operator=(const SystemAllocator&) = delete;

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override;
    // live 목록에서 주소를 찾지 못하면 역참조 없이 NotFound를 반환한다.
    // foreign·중복 해제를 구분하지 않으며 주소 재사용 이후 stale pointer의 식별은 보장하지 않는다.
    Result<void> Deallocate(void* address) noexcept override;
    // 한 시점의 ledger snapshot이다. 0이라는 관찰만으로 다른 thread의 호출 종료가 보장되지는
    // 않는다.
    AllocationStats Stats() const noexcept;

private:
    struct Record;

    mutable platform::Mutex mutex_;
    Record* records_ = nullptr;
    AllocationStats stats_{};
};

} // namespace rocks
