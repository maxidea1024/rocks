#include <Core/Memory/SystemAllocator.h>
#include <Base/Checked.h>
#include <Platform/Memory.h>

namespace rocks {

struct SystemAllocator::Record {
    void* address;
    usize bytes;
    uint32 tag;
    Record* next;
};

SystemAllocator::~SystemAllocator() noexcept {
    // 소유자의 수명 오류를 숨기는 일괄 free를 하지 않는다. 정상 소유자는 먼저 각각 반환해야 한다.
    if (Stats().liveAllocations != 0) {
        platform::Fatal("SystemAllocator destroyed with live allocations");
    }
}

Result<void*> SystemAllocator::Allocate(usize bytes, usize alignment, uint32 tag) noexcept {
    if (!IsPowerOfTwo(alignment)) {
        return Result<void*>::Failure(ErrorCode::InvalidArgument);
    }

    if (bytes == 0) {
        return Result<void*>::Success(nullptr);
    }

    if (bytes > MaxSize - (alignment - 1)) {
        return Result<void*>::Failure(ErrorCode::Overflow);
    }

    // bookkeeping을 먼저 예약한다. 이후 사용자 storage 할당이 실패하면 이 예약도 반환한다.
    void* recordStorage = platform::AlignedAllocate(sizeof(Record), alignof(Record));
    if (recordStorage == nullptr) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }

    void* address = platform::AlignedAllocate(bytes, alignment);
    if (address == nullptr) {
        platform::AlignedFree(recordStorage);
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }

    auto* record = ConstructAt<Record>(recordStorage, Record{address, bytes, tag, nullptr});
    {
        platform::LockGuard lock(mutex_);
        constexpr uint64 maximum = static_cast<uint64>(-1);
        if (stats_.liveBytes > maximum - bytes || stats_.liveAllocations == maximum ||
            stats_.totalAllocations == maximum) {
            platform::AlignedFree(address);
            DestroyAt(record);
            platform::AlignedFree(recordStorage);
            return Result<void*>::Failure(ErrorCode::Overflow);
        }

        // 목록과 count를 같은 lock 안에서 게시하여 Stats가 부분 반영된 상태를 읽지 않게 한다.
        record->next = records_;
        records_ = record;
        ++stats_.liveAllocations;
        stats_.liveBytes += bytes;
        ++stats_.totalAllocations;
    }

    return Result<void*>::Success(address);
}

Result<void> SystemAllocator::Deallocate(void* address) noexcept {
    if (address == nullptr) {
        return Result<void>::Success();
    }

    Record* found = nullptr;
    {
        platform::LockGuard lock(mutex_);
        Record** previous = &records_;
        while (*previous != nullptr && (*previous)->address != address) {
            previous = &(*previous)->next;
        }

        if (*previous == nullptr) {
            // 미소유 pointer 앞의 header를 읽지 않고 주소 값만 비교한다.
            // 이미 반환된 주소와 다른 allocator의 주소는 모두 NotFound다.
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        found = *previous;
        *previous = found->next;
        --stats_.liveAllocations;
        stats_.liveBytes -= found->bytes;
    }

    // 먼저 목록에서 제거하므로 두 thread가 같은 live record를 동시에 반환할 수 없다.
    // 실제 OS 회수는 lock 밖에서 수행하여 다른 ledger 조회를 불필요하게 막지 않는다.
    platform::AlignedFree(found->address);
    DestroyAt(found);
    platform::AlignedFree(found);
    return Result<void>::Success();
}

AllocationStats SystemAllocator::Stats() const noexcept {
    platform::LockGuard lock(mutex_);
    return stats_;
}

} // namespace rocks
