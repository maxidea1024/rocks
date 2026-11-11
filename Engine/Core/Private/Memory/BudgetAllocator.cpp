#include <Core/Memory/BudgetAllocator.h>
#include <Base/Checked.h>
#include <Core/Move.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>

namespace rocks {

struct BudgetAllocator::Record {
    void* address = nullptr;
    usize bytes = 0;
    Record* next = nullptr;
};

BudgetAllocator::BudgetAllocator(Allocator& backing, uint64 limitBytes) noexcept
    : backing_(backing), limitBytes_(limitBytes) {
}

BudgetAllocator::~BudgetAllocator() noexcept {
    platform::LockGuard lock(gate_);
    if (active_ || waiters_ != 0 || stats_.liveAllocations != 0 || records_ != nullptr) {
        platform::Fatal("BudgetAllocator destroyed with active calls or live allocations");
    }
}

BudgetAllocator::OperationGuard::~OperationGuard() noexcept {
    owner_.EndOperation();
}

Result<void> BudgetAllocator::BeginOperation() noexcept {
    const uint64 thread = platform::CurrentThreadId();
    gate_.Lock();
    while (active_) {
        if (activeThread_ == thread) {
            gate_.Unlock();
            return Result<void>::Failure(ErrorCode::Busy);
        }

        if (waiters_ == static_cast<uint64>(-1)) {
            gate_.Unlock();
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        ++waiters_;
        const auto wait = changed_.Wait(gate_);
        --waiters_;
        if (wait != platform::WaitResult::Signaled) {
            gate_.Unlock();
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }
    }

    active_ = true;
    activeThread_ = thread;
    gate_.Unlock();
    return Result<void>::Success();
}

void BudgetAllocator::EndOperation() noexcept {
    platform::LockGuard lock(gate_);
    active_ = false;
    activeThread_ = 0;
    changed_.NotifyAll();
}

Result<void*> BudgetAllocator::Allocate(usize bytes, usize alignment, uint32 tag) noexcept {
    if (!IsPowerOfTwo(alignment)) {
        return Result<void*>::Failure(ErrorCode::InvalidArgument);
    }
    if (bytes == 0) {
        return Result<void*>::Success(nullptr);
    }
    if (bytes > MaxSize - (alignment - 1)) {
        return Result<void*>::Failure(ErrorCode::Overflow);
    }

    auto begin = BeginOperation();
    if (!begin) {
        return Result<void*>::Failure(begin.Error());
    }
    OperationGuard operation(*this);

    {
        platform::LockGuard lock(gate_);
        constexpr uint64 maximum = static_cast<uint64>(-1);
        if (stats_.liveAllocations == maximum || stats_.totalAllocations == maximum ||
            static_cast<uint64>(bytes) > maximum - stats_.liveBytes) {
            return Result<void*>::Failure(ErrorCode::Overflow);
        }
        if (static_cast<uint64>(bytes) > limitBytes_ - stats_.liveBytes) {
            return Result<void*>::Failure(ErrorCode::CapacityExceeded);
        }
    }

    // 기록을 먼저 확보해야 사용자 주소가 게시된 뒤 추적 실패가 발생하지 않는다.
    auto recordMemory = backing_.Allocate(sizeof(Record), alignof(Record));
    if (!recordMemory) {
        return Result<void*>::Failure(recordMemory.Error());
    }
    if (!recordMemory.Value() ||
        reinterpret_cast<uptr>(recordMemory.Value()) % alignof(Record) != 0) {
        platform::Fatal("BudgetAllocator backing returned invalid record storage");
    }

    auto allocation = backing_.Allocate(bytes, alignment, tag);
    if (!allocation) {
        if (!backing_.Deallocate(recordMemory.Value())) {
            platform::Fatal("BudgetAllocator could not release reserved record");
        }
        return Result<void*>::Failure(allocation.Error());
    }
    if (!allocation.Value() || reinterpret_cast<uptr>(allocation.Value()) % alignment != 0) {
        platform::Fatal("BudgetAllocator backing returned invalid allocation");
    }

    auto* record = ConstructAt<Record>(recordMemory.Value());
    record->address = allocation.Value();
    record->bytes = bytes;
    {
        platform::LockGuard lock(gate_);
        record->next = records_;
        records_ = record;
        ++stats_.liveAllocations;
        ++stats_.totalAllocations;
        stats_.liveBytes += static_cast<uint64>(bytes);
        if (stats_.liveBytes > peakBytes_) {
            peakBytes_ = stats_.liveBytes;
        }
    }
    return Result<void*>::Success(allocation.Value());
}

Result<void> BudgetAllocator::Deallocate(void* address) noexcept {
    if (address == nullptr) {
        return Result<void>::Success();
    }

    auto begin = BeginOperation();
    if (!begin) {
        return Result<void>::Failure(begin.Error());
    }
    OperationGuard operation(*this);

    Record* record = nullptr;
    {
        platform::LockGuard lock(gate_);
        for (Record* current = records_; current != nullptr; current = current->next) {
            if (current->address == address) {
                record = current;
                break;
            }
        }
    }
    if (!record) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    auto released = backing_.Deallocate(address);
    if (!released) {
        return Result<void>::Failure(released.Error());
    }

    {
        platform::LockGuard lock(gate_);
        Record** previous = &records_;
        while (*previous != record) {
            previous = &(*previous)->next;
        }
        *previous = record->next;
        --stats_.liveAllocations;
        stats_.liveBytes -= static_cast<uint64>(record->bytes);
    }
    DestroyAt(record);
    if (!backing_.Deallocate(record)) {
        platform::Fatal("BudgetAllocator could not release its allocation record");
    }
    return Result<void>::Success();
}

Result<void> BudgetAllocator::SetLimit(uint64 limitBytes) noexcept {
    auto begin = BeginOperation();
    if (!begin) {
        return begin;
    }
    OperationGuard operation(*this);

    platform::LockGuard lock(gate_);
    if (limitBytes < stats_.liveBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }
    limitBytes_ = limitBytes;
    return Result<void>::Success();
}

AllocationStats BudgetAllocator::Stats() const noexcept {
    platform::LockGuard lock(gate_);
    return stats_;
}

uint64 BudgetAllocator::LimitBytes() const noexcept {
    platform::LockGuard lock(gate_);
    return limitBytes_;
}

uint64 BudgetAllocator::PeakBytes() const noexcept {
    platform::LockGuard lock(gate_);
    return peakBytes_;
}

} // namespace rocks
