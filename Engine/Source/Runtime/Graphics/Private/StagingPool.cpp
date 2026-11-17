#include <Graphics/StagingPool.h>

namespace rocks::graphics {

StagingPool::StagingPool(Allocator& allocator, StagingSource& source, uint64 budgetBytes) noexcept
    : source_(source), budgetBytes_(budgetBytes), entries_(allocator) {
}

StagingPool::~StagingPool() noexcept {
    Clear();
}

void StagingPool::TrimFreeLocked(uint64 needed) noexcept {
    // 빈 staging을 지워 예산 안에 needed byte를 둘 자리를 만든다.
    for (usize index = 0; index < entries_.SlotCount() && createdBytes_ + needed > budgetBytes_;
         ++index) {
        for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
            Entry& entry = *iterator;
            if (entry.state == State::Free) {
                source_.DestroyStaging(entry.memory);
                createdBytes_ -= entry.memory.size;
                auto removed = entries_.TryRemove(iterator.Handle());
                (void)removed;
                break;
            }
        }
    }
}

Result<StagingBuffer> StagingPool::Acquire(uint64 bytes) noexcept {
    if (bytes == 0 || bytes > budgetBytes_) {
        return Result<StagingBuffer>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(gate_);
    // 크기가 맞는 빈 staging을 다시 쓴다. handle은 새로 발급해 옛 handle을 stale로 만든다.
    for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
        Entry& entry = *iterator;
        if (entry.state != State::Free || entry.memory.size < bytes ||
            entry.memory.size > bytes * 2) {
            continue;
        }

        Entry reused = entry;
        reused.state = State::Acquired;
        reused.pending = false;
        reused.fence = 0;
        auto removed = entries_.TryRemove(iterator.Handle());
        (void)removed;
        auto added = entries_.TryAdd(reused);
        if (!added) {
            source_.DestroyStaging(reused.memory);
            createdBytes_ -= reused.memory.size;
            return Result<StagingBuffer>::Failure(added.Error());
        }

        ++reused_;
        return Result<StagingBuffer>::Success(
            StagingBuffer{StagingHandle{added.Value()}, reused.memory.data, reused.memory.size});
    }

    if (createdBytes_ + bytes > budgetBytes_) {
        TrimFreeLocked(bytes);
        if (createdBytes_ + bytes > budgetBytes_) {
            return Result<StagingBuffer>::Failure(ErrorCode::CapacityExceeded);
        }
    }

    auto created = source_.CreateStaging(bytes);
    if (!created) {
        return Result<StagingBuffer>::Failure(created.Error());
    }

    Entry entry;
    entry.memory = created.Value();
    auto added = entries_.TryAdd(entry);
    if (!added) {
        source_.DestroyStaging(entry.memory);
        return Result<StagingBuffer>::Failure(added.Error());
    }

    createdBytes_ += entry.memory.size;
    ++created_;
    return Result<StagingBuffer>::Success(
        StagingBuffer{StagingHandle{added.Value()}, entry.memory.data, entry.memory.size});
}

Result<StagingMemory> StagingPool::Find(StagingHandle handle) const noexcept {
    platform::LockGuard lock(gate_);
    const Entry* entry = entries_.Find(handle.slot);
    if (entry == nullptr || entry->state != State::Acquired) {
        return Result<StagingMemory>::Failure(ErrorCode::StaleHandle);
    }

    return Result<StagingMemory>::Success(entry->memory);
}

Result<void> StagingPool::MarkPending(StagingHandle handle) noexcept {
    platform::LockGuard lock(gate_);
    Entry* entry = entries_.Find(handle.slot);
    if (entry == nullptr || entry->state != State::Acquired) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    entry->pending = true;
    return Result<void>::Success();
}

void StagingPool::AssignPending(uint64 fence) noexcept {
    platform::LockGuard lock(gate_);
    for (Entry& entry : entries_) {
        if (entry.pending) {
            entry.pending = false;
            entry.fence = fence;
        }
    }
}

void StagingPool::Release(StagingHandle handle) noexcept {
    platform::LockGuard lock(gate_);
    Entry* entry = entries_.Find(handle.slot);
    if (entry != nullptr && entry->state == State::Acquired) {
        entry->state = State::Released;
    }
}

void StagingPool::Reclaim(uint64 completed) noexcept {
    platform::LockGuard lock(gate_);
    for (Entry& entry : entries_) {
        if (entry.state == State::Released && !entry.pending && entry.fence <= completed) {
            entry.state = State::Free;
        }
    }
}

void StagingPool::Clear() noexcept {
    platform::LockGuard lock(gate_);
    for (const Entry& entry : entries_) {
        source_.DestroyStaging(entry.memory);
    }

    entries_.Clear();
    createdBytes_ = 0;
}

StagingStats StagingPool::Stats() const noexcept {
    platform::LockGuard lock(gate_);
    StagingStats stats;
    for (const Entry& entry : entries_) {
        switch (entry.state) {
        case State::Acquired:
            ++stats.acquired;
            break;
        case State::Released:
            ++stats.waiting;
            break;
        case State::Free:
            ++stats.free;
            break;
        }
    }

    stats.createdBytes = createdBytes_;
    stats.created = created_;
    stats.reused = reused_;
    return stats;
}

} // namespace rocks::graphics
