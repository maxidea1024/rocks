#pragma once

#include <Core/Memory/Allocator.h>
#include <Platform/Sync.h>

namespace rocks {

// 상위 allocator에서 얻은 사용자 allocation의 요청 바이트 합계에 상한을 적용한다.
// 사용 예: BudgetAllocator budget(system, 1024 * 1024); Array<int32> values(budget);
// 상위 allocator는 이 객체와 모든 allocation보다 오래 살아야 한다. 파괴 전 모든 caller를
// 종료하고 소유 allocation을 반환한다. 추적 중인 작업·대기자나 남은 allocation은 Fatal이다.
// 인자 조기 검사와 통계 조회까지 파괴와 경합하지 않는지는 호출자가 보장해야 한다.
// 상한·통계에는 이 래퍼의 기록 storage 및 상위 allocator의 정렬 오버헤드는 포함하지 않는다.
// 일반 동시 호출은 직렬화한다. 상위 allocator도 각 호출 스레드의 접근을 지원해야 한다.
// 상위 콜백이 같은 스레드에서 상태를 변경하는 유효한 작업에 재진입하면 Busy다. 인자 오류와
// 0-byte/null no-op은 먼저 처리되며 Stats·LimitBytes·PeakBytes는 게시된 스냅샷을 읽을 수 있다.
// 상위 콜백이 다른 스레드의 이 allocator 호출 완료를 기다리는 것은 교착 위험으로 금지한다.
// 상위 allocator의 실패한 Deallocate는 allocation의 소유권과 내용을 유지해야 한다.
class BudgetAllocator final : public Allocator {
public:
    explicit BudgetAllocator(Allocator& backing, uint64 limitBytes) noexcept;
    ~BudgetAllocator() noexcept override;

    BudgetAllocator(const BudgetAllocator&) = delete;
    BudgetAllocator& operator=(const BudgetAllocator&) = delete;
    BudgetAllocator(BudgetAllocator&&) = delete;
    BudgetAllocator& operator=(BudgetAllocator&&) = delete;

    // 상한을 넘으면 CapacityExceeded, 상위 실패는 원래 오류를 반환한다. 성공한 0 byte는
    // nullptr이며 상한과 통계를 변경하지 않는다. alignment는 0이 아닌 2의 거듭제곱이어야 한다.
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override;

    // 자신이 반환한 살아 있는 주소만 받는다. 소유하지 않은 주소는 역참조하지 않고 NotFound다.
    // 상위 allocator가 반환에 실패하면 기록과 상한 사용량을 보존한다. nullptr는 성공한 무효
    // 연산이다. 주소가 재사용된 뒤에는 오래된 포인터와 새 allocation을 구분할 수 없다.
    Result<void> Deallocate(void* address) noexcept override;

    // 현재 live byte보다 작게 줄일 수 없다. 실패 시 기존 상한은 유지된다.
    Result<void> SetLimit(uint64 limitBytes) noexcept;

    // 한 시점에 완료되어 게시된 요청 byte 기준 통계다. 진행 중 상위 호출은 포함하지 않는다.
    // 0인 스냅샷을 다른 스레드의 호출 종료·객체 파괴 안전성의 증거로 사용하면 안 된다.
    AllocationStats Stats() const noexcept;
    uint64 LimitBytes() const noexcept;
    uint64 PeakBytes() const noexcept;

private:
    struct Record;

    // 성공한 경우에만 guard를 생성한다. 상위 호출 중 같은 스레드의 재진입을 거부한다.
    class OperationGuard {
    public:
        explicit OperationGuard(BudgetAllocator& owner) noexcept : owner_(owner) {
        }
        ~OperationGuard() noexcept;
        OperationGuard(const OperationGuard&) = delete;
        OperationGuard& operator=(const OperationGuard&) = delete;

    private:
        BudgetAllocator& owner_;
    };

    Result<void> BeginOperation() noexcept;
    void EndOperation() noexcept;

    Allocator& backing_;
    mutable platform::Mutex gate_;
    platform::ConditionVariable changed_;
    Record* records_ = nullptr;
    AllocationStats stats_{};
    uint64 limitBytes_ = 0;
    uint64 peakBytes_ = 0;
    uint64 activeThread_ = 0;
    uint64 waiters_ = 0;
    bool active_ = false;
};

} // namespace rocks
