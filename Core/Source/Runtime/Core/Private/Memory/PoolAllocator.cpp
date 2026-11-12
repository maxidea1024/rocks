#include <Core/Memory/PoolAllocator.h>
#include <Base/Checked.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>

namespace rocks {

struct PoolAllocator::Chunk {
    Chunk* next = nullptr;
    Slot* slots = nullptr;
    unsigned char* data = nullptr;
    usize dataBytes = 0;
};

struct PoolAllocator::Slot {
    Slot* nextFree = nullptr;
    void* address = nullptr;
    usize requested = 0;
    uint32 tag = 0;
    bool live = false;
};

// backing 콜백이 A -> B -> A 순서로 돌아와도 A의 mutex에 다시 진입하지 않는다.
struct PoolAllocator::EntryScope {
    explicit EntryScope(const PoolAllocator* owner) noexcept : owner(owner) {
        for (EntryScope* entry = activeEntry_; entry != nullptr; entry = entry->previous) {
            if (entry->owner == owner) {
                return;
            }
        }

        previous = activeEntry_;
        activeEntry_ = this;
        entered = true;
    }

    ~EntryScope() noexcept {
        if (entered) {
            if (activeEntry_ != this) {
                platform::Fatal("PoolAllocator entry stack corrupted");
            }

            activeEntry_ = previous;
        }
    }

    EntryScope(const EntryScope&) = delete;
    EntryScope& operator=(const EntryScope&) = delete;

    const PoolAllocator* owner;
    EntryScope* previous = nullptr;
    bool entered = false;
};

thread_local PoolAllocator::EntryScope* PoolAllocator::activeEntry_ = nullptr;

namespace {

bool AlignUp(usize value, usize alignment, usize& result) noexcept {
    usize rounded = 0;
    if (!TryAdd(value, alignment - 1, rounded)) {
        return false;
    }

    result = rounded & ~(alignment - 1);
    return true;
}

} // namespace

PoolAllocator::PoolAllocator(Allocator& backing, usize blockBytes, usize blockAlignment,
                             usize blocksPerChunk) noexcept
    : backing_(backing), blockBytes_(blockBytes), blockAlignment_(blockAlignment),
      blocksPerChunk_(blocksPerChunk) {
    if (blockBytes == 0 || blocksPerChunk == 0 || !IsPowerOfTwo(blockAlignment)) {
        platform::Fatal("PoolAllocator requires nonzero blocks and power-of-two alignment");
    }

    chunkAlignment_ = blockAlignment_;
    if (chunkAlignment_ < alignof(Chunk)) {
        chunkAlignment_ = alignof(Chunk);
    }
    if (chunkAlignment_ < alignof(Slot)) {
        chunkAlignment_ = alignof(Slot);
    }

    usize slotBytes = 0;
    usize afterSlots = 0;
    usize dataBytes = 0;
    layoutOverflow_ = !AlignUp(blockBytes_, blockAlignment_, stride_) ||
                      !AlignUp(sizeof(Chunk), alignof(Slot), slotOffset_) ||
                      !TryMultiply(blocksPerChunk_, sizeof(Slot), slotBytes) ||
                      !TryAdd(slotOffset_, slotBytes, afterSlots) ||
                      !AlignUp(afterSlots, blockAlignment_, dataOffset_) ||
                      !TryMultiply(blocksPerChunk_, stride_, dataBytes) ||
                      !TryAdd(dataOffset_, dataBytes, chunkBytes_);
}

PoolAllocator::~PoolAllocator() noexcept {
    EntryScope entry(this);
    if (!entry.entered) {
        platform::Fatal("PoolAllocator destroyed during reentrant operation");
    }

    platform::LockGuard lock(mutex_);
    if (stats_.liveAllocations != 0) {
        platform::Fatal("PoolAllocator destroyed with live allocations");
    }

    ReleaseChunks();
}

Result<void*> PoolAllocator::Allocate(usize bytes, usize alignment, uint32 tag) noexcept {
    EntryScope entry(this);
    if (!entry.entered) {
        return Result<void*>::Failure(ErrorCode::Busy);
    }
    if (!IsPowerOfTwo(alignment)) {
        return Result<void*>::Failure(ErrorCode::InvalidArgument);
    }
    if (bytes == 0) {
        return Result<void*>::Success(nullptr);
    }
    if (bytes > blockBytes_ || alignment > blockAlignment_) {
        return Result<void*>::Failure(ErrorCode::Unsupported);
    }

    platform::LockGuard lock(mutex_);
    if (closed_) {
        return Result<void*>::Failure(ErrorCode::ShuttingDown);
    }
    constexpr uint64 maximum = static_cast<uint64>(-1);
    if (stats_.liveAllocations == maximum || stats_.totalAllocations == maximum ||
        stats_.liveBytes > maximum - bytes) {
        return Result<void*>::Failure(ErrorCode::Overflow);
    }

    if (free_ == nullptr) {
        auto result = AddChunk();
        if (!result) {
            return Result<void*>::Failure(result.Error());
        }
    }

    Slot* slot = free_;
    free_ = slot->nextFree;
    slot->nextFree = nullptr;
    slot->requested = bytes;
    slot->tag = tag;
    slot->live = true;
    ++stats_.liveAllocations;
    stats_.liveBytes += bytes;
    ++stats_.totalAllocations;
    return Result<void*>::Success(slot->address);
}

Result<void> PoolAllocator::Deallocate(void* address) noexcept {
    EntryScope entry(this);
    if (!entry.entered) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (address == nullptr) {
        return Result<void>::Success();
    }

    platform::LockGuard lock(mutex_);
    if (closed_) {
        return Result<void>::Failure(ErrorCode::ShuttingDown);
    }

    const uptr target = reinterpret_cast<uptr>(address);
    for (Chunk* chunk = chunks_; chunk != nullptr; chunk = chunk->next) {
        const uptr base = reinterpret_cast<uptr>(chunk->data);
        if (target < base) {
            continue;
        }

        const uptr offset = target - base;
        if (offset >= chunk->dataBytes || offset % stride_ != 0) {
            continue;
        }

        Slot& slot = chunk->slots[offset / stride_];
        if (!slot.live || slot.address != address) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        --stats_.liveAllocations;
        stats_.liveBytes -= slot.requested;
        slot.requested = 0;
        slot.tag = 0;
        slot.live = false;
        slot.nextFree = free_;
        free_ = &slot;
        return Result<void>::Success();
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> PoolAllocator::Trim() noexcept {
    EntryScope entry(this);
    if (!entry.entered) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    platform::LockGuard lock(mutex_);
    if (closed_) {
        return Result<void>::Failure(ErrorCode::ShuttingDown);
    }
    if (stats_.liveAllocations != 0) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    ReleaseChunks();
    return Result<void>::Success();
}

Result<void> PoolAllocator::Shutdown() noexcept {
    EntryScope entry(this);
    if (!entry.entered) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    platform::LockGuard lock(mutex_);
    if (closed_) {
        return Result<void>::Success();
    }
    if (stats_.liveAllocations != 0) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    ReleaseChunks();
    closed_ = true;
    return Result<void>::Success();
}

AllocationStats PoolAllocator::Stats() const noexcept {
    EntryScope entry(this);
    if (!entry.entered) {
        platform::Fatal("PoolAllocator stats called during reentrant operation");
    }

    platform::LockGuard lock(mutex_);
    return stats_;
}

usize PoolAllocator::Capacity() const noexcept {
    EntryScope entry(this);
    if (!entry.entered) {
        platform::Fatal("PoolAllocator capacity called during reentrant operation");
    }

    platform::LockGuard lock(mutex_);
    return capacity_;
}

usize PoolAllocator::ChunkCount() const noexcept {
    EntryScope entry(this);
    if (!entry.entered) {
        platform::Fatal("PoolAllocator chunk count called during reentrant operation");
    }

    platform::LockGuard lock(mutex_);
    return chunkCount_;
}

Result<void> PoolAllocator::AddChunk() noexcept {
    if (layoutOverflow_ || capacity_ > MaxSize - blocksPerChunk_ || chunkCount_ == MaxSize) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    // 공개 전에 한 번만 할당한다. 할당 실패에는 기존 블록·free list가 그대로다.
    auto allocation = backing_.Allocate(chunkBytes_, chunkAlignment_);
    if (!allocation) {
        return Result<void>::Failure(allocation.Error());
    }

    void* storage = allocation.Value();
    if (storage == nullptr || reinterpret_cast<uptr>(storage) % chunkAlignment_ != 0) {
        platform::Fatal("PoolAllocator backing returned invalid storage");
    }

    auto* bytes = static_cast<unsigned char*>(storage);
    auto* chunk = ConstructAt<Chunk>(storage);
    chunk->slots = reinterpret_cast<Slot*>(bytes + slotOffset_);
    chunk->data = bytes + dataOffset_;
    chunk->dataBytes = stride_ * blocksPerChunk_;
    for (usize index = 0; index < blocksPerChunk_; ++index) {
        Slot* slot = ConstructAt<Slot>(chunk->slots + index);
        slot->address = chunk->data + index * stride_;
        slot->nextFree = free_;
        free_ = slot;
    }

    chunk->next = chunks_;
    chunks_ = chunk;
    capacity_ += blocksPerChunk_;
    ++chunkCount_;
    return Result<void>::Success();
}

void PoolAllocator::ReleaseChunks() noexcept {
    // 호출 전 live=0이므로 슬롯의 메모리를 사용하는 객체가 남아 있지 않아야 한다.
    Chunk* chunk = chunks_;
    while (chunk != nullptr) {
        Chunk* next = chunk->next;
        for (usize index = 0; index < blocksPerChunk_; ++index) {
            DestroyAt(chunk->slots + index);
        }
        DestroyAt(chunk);
        if (!backing_.Deallocate(chunk)) {
            platform::Fatal("PoolAllocator backing rejected owned chunk");
        }
        chunk = next;
    }

    chunks_ = nullptr;
    free_ = nullptr;
    capacity_ = 0;
    chunkCount_ = 0;
}

} // namespace rocks
