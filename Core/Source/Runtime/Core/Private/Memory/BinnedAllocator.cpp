#include <Core/Memory/BinnedAllocator.h>
#include <Base/Checked.h>
#include <Platform/Memory.h>

namespace rocks {

namespace {
bool Overlaps(uptr left, usize leftSize, uptr right, usize rightSize) noexcept {
    return left <= right ? right - left < leftSize : left - right < rightSize;
}
} // namespace

struct BinnedAllocator::Record {
    void* address;
    usize bytes;
    usize bin;
    Record* next;
};

struct BinnedAllocator::CheckedBacking::BackingRecord {
    void* address;
    usize bytes;
    BackingRecord* next;
};

BinnedAllocator::CheckedBacking::CheckedBacking(BinnedAllocator& owner, Allocator& backing) noexcept
    : owner_(owner), backing_(backing) {
}

BinnedAllocator::CheckedBacking::~CheckedBacking() noexcept {
    if (records_ != nullptr) {
        platform::Fatal("BinnedAllocator backing destroyed with live storage");
    }
}

Result<void*> BinnedAllocator::CheckedBacking::Allocate(usize bytes, usize alignment,
                                                        uint32 tag) noexcept {
    void* recordStorage = platform::AlignedAllocate(sizeof(BackingRecord), alignof(BackingRecord));
    if (!recordStorage) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }
    auto result = backing_.Allocate(bytes, alignment, tag);
    if (!result) {
        platform::AlignedFree(recordStorage);
        return result;
    }
    void* address = result.Value();
    const uptr start = reinterpret_cast<uptr>(address);
    if (!address || start % alignment != 0) {
        platform::Fatal("BinnedAllocator backing returned invalid storage");
    }
    if (bytes > MaxSize - start) {
        platform::Fatal("BinnedAllocator backing returned overflowing address range");
    }
    if (Overlaps(start, bytes, reinterpret_cast<uptr>(recordStorage), sizeof(BackingRecord)) ||
        (owner_.pendingRecord_ &&
         Overlaps(start, bytes, reinterpret_cast<uptr>(owner_.pendingRecord_), sizeof(Record)))) {
        platform::Fatal("BinnedAllocator backing overlaps allocation metadata");
    }
    for (BackingRecord* item = records_; item; item = item->next) {
        if (Overlaps(start, bytes, reinterpret_cast<uptr>(item), sizeof(BackingRecord)) ||
            Overlaps(start, bytes, reinterpret_cast<uptr>(item->address), item->bytes)) {
            platform::Fatal("BinnedAllocator backing returned overlapping storage");
        }
    }
    for (Record* item = owner_.records_; item; item = item->next) {
        if (Overlaps(start, bytes, reinterpret_cast<uptr>(item), sizeof(Record))) {
            platform::Fatal("BinnedAllocator backing overlaps allocation metadata");
        }
    }
    auto* record =
        ConstructAt<BackingRecord>(recordStorage, BackingRecord{address, bytes, records_});
    records_ = record;
    return result;
}

Result<void> BinnedAllocator::CheckedBacking::Deallocate(void* address) noexcept {
    BackingRecord** link = &records_;
    while (*link && (*link)->address != address) {
        link = &(*link)->next;
    }
    if (!*link) {
        platform::Fatal("BinnedAllocator backing lost storage ownership");
    }
    auto result = backing_.Deallocate(address);
    if (!result) {
        return result;
    }
    BackingRecord* record = *link;
    *link = record->next;
    DestroyAt(record);
    platform::AlignedFree(record);
    return Result<void>::Success();
}

struct BinnedAllocator::EntryScope {
    explicit EntryScope(const BinnedAllocator* allocator) noexcept : owner(allocator) {
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
            activeEntry_ = previous;
        }
    }

    const BinnedAllocator* owner;
    EntryScope* previous = nullptr;
    bool entered = false;
};

thread_local BinnedAllocator::EntryScope* BinnedAllocator::activeEntry_ = nullptr;

BinnedAllocator::BinnedAllocator(Allocator& backing) noexcept
    : backing_(*this, backing),
      pools_{PoolAllocator(backing_, 16, 16),   PoolAllocator(backing_, 32, 16),
             PoolAllocator(backing_, 64, 16),   PoolAllocator(backing_, 128, 16),
             PoolAllocator(backing_, 256, 16),  PoolAllocator(backing_, 512, 16),
             PoolAllocator(backing_, 1024, 16), PoolAllocator(backing_, 2048, 16),
             PoolAllocator(backing_, 4096, 16)} {
}

BinnedAllocator::~BinnedAllocator() noexcept {
    EntryScope entry(this);
    if (!entry.entered) {
        platform::Fatal("BinnedAllocator destroyed during a callback");
    }
    platform::LockGuard lock(mutex_);
    if (records_ != nullptr || stats_.liveAllocations != 0) {
        platform::Fatal("BinnedAllocator destroyed with live allocations");
    }
    // 하위 pool의 멤버 파괴보다 앞서 backing을 반환해야 callback의 재진입을 차단할 수 있다.
    for (auto& pool : pools_) {
        if (!pool.Shutdown()) {
            platform::Fatal("BinnedAllocator failed to close an owned pool");
        }
    }
    closed_ = true;
}

Result<void*> BinnedAllocator::Allocate(usize bytes, usize alignment, uint32 tag) noexcept {
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
    if (bytes > MaxSize - (alignment - 1)) {
        return Result<void*>::Failure(ErrorCode::Overflow);
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

    // 원장을 먼저 확보하므로 실패 시 사용자에게 기록 없는 주소를 게시하지 않는다.
    void* storage = platform::AlignedAllocate(sizeof(Record), alignof(Record));
    if (storage == nullptr) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }
    usize bin = BinCount;
    if (alignment <= 16 && bytes <= 4096) {
        usize capacity = 16;
        bin = 0;
        while (capacity < bytes) {
            capacity *= 2;
            ++bin;
        }
    }
    pendingRecord_ = storage;
    auto result = bin == BinCount ? backing_.Allocate(bytes, alignment, tag)
                                  : pools_[bin].Allocate(bytes, alignment, tag);
    pendingRecord_ = nullptr;
    if (!result) {
        platform::AlignedFree(storage);
        return Result<void*>::Failure(result.Error());
    }
    const uptr start = reinterpret_cast<uptr>(result.Value());
    if (!result.Value() || start % alignment != 0 || bytes > MaxSize - start) {
        platform::Fatal("BinnedAllocator received invalid allocation");
    }
    if (Overlaps(start, bytes, reinterpret_cast<uptr>(storage), sizeof(Record))) {
        platform::Fatal("BinnedAllocator allocation overlaps metadata");
    }
    for (Record* item = records_; item; item = item->next) {
        if (Overlaps(start, bytes, reinterpret_cast<uptr>(item), sizeof(Record)) ||
            Overlaps(start, bytes, reinterpret_cast<uptr>(item->address), item->bytes)) {
            platform::Fatal("BinnedAllocator allocation overlaps live storage");
        }
    }

    auto* record = ConstructAt<Record>(storage, Record{result.Value(), bytes, bin, records_});
    records_ = record;
    ++stats_.liveAllocations;
    stats_.liveBytes += bytes;
    ++stats_.totalAllocations;
    return Result<void*>::Success(record->address);
}

Result<void> BinnedAllocator::Deallocate(void* address) noexcept {
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
    Record** link = &records_;
    while (*link != nullptr && (*link)->address != address) {
        link = &(*link)->next;
    }
    if (*link == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }
    Record* record = *link;
    auto result = record->bin == BinCount ? backing_.Deallocate(address)
                                          : pools_[record->bin].Deallocate(address);
    if (!result) {
        return result;
    }
    *link = record->next;
    --stats_.liveAllocations;
    stats_.liveBytes -= record->bytes;
    DestroyAt(record);
    platform::AlignedFree(record);
    return Result<void>::Success();
}

Result<void> BinnedAllocator::Trim() noexcept {
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
    for (auto& pool : pools_) {
        auto result = pool.Trim();
        if (!result) {
            return result;
        }
    }
    return Result<void>::Success();
}

Result<void> BinnedAllocator::Shutdown() noexcept {
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
    for (auto& pool : pools_) {
        auto result = pool.Shutdown();
        if (!result) {
            return result;
        }
    }
    closed_ = true;
    return Result<void>::Success();
}

AllocationStats BinnedAllocator::Stats() const noexcept {
    EntryScope entry(this);
    if (!entry.entered) {
        platform::Fatal("BinnedAllocator queried during a callback");
    }
    platform::LockGuard lock(mutex_);
    return stats_;
}

} // namespace rocks
