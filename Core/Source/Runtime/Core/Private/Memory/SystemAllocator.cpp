#include <Core/Memory/SystemAllocator.h>

#include "AddressLedger.h"

#include <Base/Checked.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks {

// 원장 저장소(추적 없는 플랫폼 할당)와 주소 원장이다. 저장소가 원장보다 먼저 선언되어 더 오래 산다.
struct SystemAllocator::Ledger {
    detail::UntrackedPlatformAllocator storage;
    detail::AddressLedger entries{storage};
};

SystemAllocator::SystemAllocator() noexcept {
    static_assert(sizeof(Ledger) <= LedgerStorageBytes,
                  "SystemAllocator ledger storage is too small");
    static_assert(alignof(Ledger) <= LedgerStorageAlignment,
                  "SystemAllocator ledger storage is under-aligned");
    ConstructAt<Ledger>(ledger_);
}

SystemAllocator::~SystemAllocator() noexcept {
    // 소유자의 수명 오류를 숨기는 일괄 free를 하지 않는다. 정상 소유자는 먼저 각각 반환해야 한다.
    if (Stats().liveAllocations != 0) {
        platform::Fatal("SystemAllocator destroyed with live allocations");
    }

    DestroyAt(&LedgerState());
}

SystemAllocator::Ledger& SystemAllocator::LedgerState() noexcept {
    return *platform::Launder(reinterpret_cast<Ledger*>(ledger_));
}

Result<void*> SystemAllocator::Allocate(usize bytes, usize alignment, uint32) noexcept {
    if (!IsPowerOfTwo(alignment)) {
        return Result<void*>::Failure(ErrorCode::InvalidArgument);
    }

    if (bytes == 0) {
        return Result<void*>::Success(nullptr);
    }

    if (bytes > MaxSize - (alignment - 1)) {
        return Result<void*>::Failure(ErrorCode::Overflow);
    }

    void* address = platform::AlignedAllocate(bytes, alignment);
    if (address == nullptr) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }

    ErrorCode failure = ErrorCode::None;
    {
        platform::LockGuard lock(mutex_);
        constexpr uint64 maximum = NumericLimits<uint64>::Max();
        if (stats_.liveBytes > maximum - bytes || stats_.liveAllocations == maximum ||
            stats_.totalAllocations == maximum) {
            failure = ErrorCode::Overflow;
        } else if (auto inserted = LedgerState().entries.Insert(address, bytes); !inserted) {
            // OS가 살아 있는 주소를 다시 내주는 일은 없다(원장이 어긋났다).
            if (inserted.Error() == ErrorCode::InvalidArgument) {
                platform::Fatal("SystemAllocator ledger already holds a newly allocated address");
            }

            failure = inserted.Error();
        } else {
            // 원장과 count를 같은 lock 안에서 게시하여 Stats가 부분 반영된 상태를 읽지 않게 한다.
            ++stats_.liveAllocations;
            stats_.liveBytes += bytes;
            ++stats_.totalAllocations;
        }
    }

    if (failure != ErrorCode::None) {
        // 원장에 적지 못한 블록은 게시하지 않고 OS에 돌려준다.
        platform::AlignedFree(address);
        return Result<void*>::Failure(failure);
    }

    return Result<void*>::Success(address);
}

Result<void> SystemAllocator::Deallocate(void* address) noexcept {
    if (address == nullptr) {
        return Result<void>::Success();
    }

    {
        platform::LockGuard lock(mutex_);
        usize bytes = 0;
        if (!LedgerState().entries.Remove(address, bytes)) {
            // 미소유 pointer를 읽지 않고 주소 값만 비교한다. 이미 반환된 주소와 다른 allocator의
            // 주소는 모두 NotFound다.
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        --stats_.liveAllocations;
        stats_.liveBytes -= bytes;
    }

    // 먼저 원장에서 지우므로 두 thread가 같은 live 주소를 동시에 반환할 수 없다. 실제 OS 회수는
    // lock 밖에서 수행하여 다른 ledger 조회를 불필요하게 막지 않는다.
    platform::AlignedFree(address);
    return Result<void>::Success();
}

AllocationStats SystemAllocator::Stats() const noexcept {
    platform::LockGuard lock(mutex_);
    return stats_;
}

void SystemAllocator::FailLedgerStorageOnAttempt(uint64 oneBasedAttempt) noexcept {
    platform::LockGuard lock(mutex_);
    LedgerState().storage.FailOnAttempt(oneBasedAttempt);
}

usize SystemAllocator::LedgerCount() noexcept {
    platform::LockGuard lock(mutex_);
    return LedgerState().entries.Count();
}

} // namespace rocks
