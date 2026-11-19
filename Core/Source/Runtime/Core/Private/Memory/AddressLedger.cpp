#include "AddressLedger.h"

#include <Base/Checked.h>
#include <Platform/Memory.h>

namespace rocks::detail {

Result<void*> UntrackedPlatformAllocator::Allocate(usize bytes, usize alignment, uint32) noexcept {
    if (!IsPowerOfTwo(alignment)) {
        return Result<void*>::Failure(ErrorCode::InvalidArgument);
    }

    if (bytes == 0) {
        return Result<void*>::Success(nullptr);
    }

    ++attempts_;
    if (failureAttempt_ != 0 && attempts_ == failureAttempt_) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }

    void* address = platform::AlignedAllocate(bytes, alignment);
    if (address == nullptr) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<void*>::Success(address);
}

Result<void> UntrackedPlatformAllocator::Deallocate(void* address) noexcept {
    if (address != nullptr) {
        platform::AlignedFree(address);
    }

    return Result<void>::Success();
}

AddressLedger::AddressLedger(Allocator& storage) noexcept : entries_(storage) {
}

Result<void> AddressLedger::Insert(const void* address, usize bytes) noexcept {
    if (address == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto inserted = entries_.Insert(reinterpret_cast<uptr>(address), bytes);
    if (!inserted) {
        return Result<void>::Failure(inserted.Error());
    }

    if (inserted.Value() != InsertOutcome::Inserted) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

bool AddressLedger::Remove(const void* address, usize& bytes) noexcept {
    const uptr key = reinterpret_cast<uptr>(address);
    const usize* found = entries_.Find(key);
    if (found == nullptr) {
        return false;
    }

    bytes = *found;
    return entries_.Erase(key);
}

usize AddressLedger::Count() const noexcept {
    return entries_.Count();
}

usize AddressLedger::LongestProbe() const noexcept {
    return entries_.LongestProbe();
}

} // namespace rocks::detail
