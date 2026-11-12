#include <Core/Memory/StackAllocator.h>
#include <Base/Checked.h>
#include <Core/Move.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Sync.h>

namespace rocks {

namespace {

// 할당자가 같은 주소에 재생성되어도 이전 표시가 새 객체에 통과하지 않는다.
uint64 AcquireStackOwnerId() noexcept {
    static platform::Mutex mutex;
    static uint64 next = 1;
    platform::LockGuard lock(mutex);
    if (next == 0) {
        platform::Fatal("StackAllocator owner identity exhausted");
    }

    const uint64 id = next;
    ++next;
    return id;
}

} // namespace

struct StackAllocator::Chunk {
    Chunk* next;
    usize capacity;
    usize used;
};

struct StackAllocator::Record {
    Record* previous;
    Chunk* chunk;
    void* address;
    usize bytes;
    usize previousUsed;
    uint64 id;
};

struct StackAllocator::CallGuard {
    explicit CallGuard(bool& inCall) noexcept : inCall_(inCall) {
        inCall_ = true;
    }

    CallGuard(const CallGuard&) = delete;
    CallGuard& operator=(const CallGuard&) = delete;

    ~CallGuard() noexcept {
        inCall_ = false;
    }

    bool& inCall_;
};

StackAllocator::StackAllocator(Allocator& backing, usize chunkBytes) noexcept
    : backing_(&backing), chunkBytes_(chunkBytes), ownerThread_(platform::CurrentThreadId()),
      ownerId_(AcquireStackOwnerId()) {
}

StackAllocator::~StackAllocator() noexcept {
    if (!IsOwnerThread()) {
        platform::Fatal("StackAllocator destroyed on wrong thread");
    }
    if (inCall_) {
        platform::Fatal("StackAllocator destroyed during reentrant call");
    }

    CallGuard guard(inCall_);
    if (stats_.allocations.liveAllocations != 0) {
        platform::Fatal("StackAllocator destroyed with live allocations");
    }

    ReleaseChunks();
}

bool StackAllocator::IsOwnerThread() const noexcept {
    return platform::CurrentThreadId() == ownerThread_;
}

bool StackAllocator::TryLayout(const Chunk& chunk, usize bytes, usize alignment,
                               usize& recordOffset, usize& dataOffset,
                               usize& endOffset) const noexcept {
    const uptr base = reinterpret_cast<uptr>(&chunk);
    usize address = 0;
    usize rounded = 0;
    if (!TryAdd(base, chunk.used, address) || !TryAdd(address, alignof(Record) - 1, rounded)) {
        return false;
    }

    rounded &= ~(static_cast<usize>(alignof(Record)) - 1);
    if (rounded < base) {
        return false;
    }

    const usize record = rounded - base;
    if (!TryAdd(rounded, sizeof(Record), address) || !TryAdd(address, alignment - 1, rounded)) {
        return false;
    }

    rounded &= ~(alignment - 1);
    if (rounded < base) {
        return false;
    }

    const usize data = rounded - base;
    usize end = 0;
    if (!TryAdd(data, bytes, end) || end > chunk.capacity) {
        return false;
    }

    recordOffset = record;
    dataOffset = data;
    endOffset = end;
    return true;
}

Result<void*> StackAllocator::Allocate(usize bytes, usize alignment, uint32 tag) noexcept {
    if (!IsOwnerThread()) {
        return Result<void*>::Failure(ErrorCode::WrongThread);
    }
    if (inCall_) {
        return Result<void*>::Failure(ErrorCode::Busy);
    }

    CallGuard guard(inCall_);
    if (!IsPowerOfTwo(alignment)) {
        return Result<void*>::Failure(ErrorCode::InvalidArgument);
    }
    if (bytes == 0) {
        return Result<void*>::Success(nullptr);
    }

    constexpr uint64 maximum = static_cast<uint64>(-1);
    if (stats_.allocations.liveAllocations == maximum ||
        stats_.allocations.totalAllocations == maximum ||
        bytes > maximum - stats_.allocations.liveBytes) {
        return Result<void*>::Failure(ErrorCode::Overflow);
    }

    usize recordOffset = 0;
    usize dataOffset = 0;
    usize endOffset = 0;
    Chunk* chunk = chunks_;
    while (chunk && !TryLayout(*chunk, bytes, alignment, recordOffset, dataOffset, endOffset)) {
        chunk = chunk->next;
    }

    if (!chunk) {
        usize minimum = sizeof(Chunk);
        if (!TryAdd(minimum, alignof(Record) - 1, minimum) ||
            !TryAdd(minimum, sizeof(Record), minimum) || !TryAdd(minimum, alignment - 1, minimum) ||
            !TryAdd(minimum, bytes, minimum)) {
            return Result<void*>::Failure(ErrorCode::Overflow);
        }

        const usize capacity = chunkBytes_ > minimum ? chunkBytes_ : minimum;
        if (capacity > MaxSize - stats_.reservedBytes || stats_.chunks == MaxSize) {
            return Result<void*>::Failure(ErrorCode::Overflow);
        }

        auto allocation = backing_->Allocate(capacity, alignof(Chunk), tag);
        if (!allocation) {
            return Result<void*>::Failure(allocation.Error());
        }

        void* storage = allocation.Value();
        if (!storage || reinterpret_cast<uptr>(storage) % alignof(Chunk) != 0) {
            platform::Fatal("StackAllocator backing allocator returned invalid storage");
        }

        chunk = ConstructAt<Chunk>(storage, Chunk{chunks_, capacity, sizeof(Chunk)});
        if (!TryLayout(*chunk, bytes, alignment, recordOffset, dataOffset, endOffset)) {
            DestroyAt(chunk);
            if (!backing_->Deallocate(storage)) {
                platform::Fatal("StackAllocator backing storage ownership mismatch");
            }
            return Result<void*>::Failure(ErrorCode::Overflow);
        }

        chunks_ = chunk;
        stats_.reservedBytes += capacity;
        ++stats_.chunks;
    }

    auto* storage = reinterpret_cast<unsigned char*>(chunk);
    void* address = storage + dataOffset;
    const uint64 id = stats_.allocations.totalAllocations + 1;
    auto* record = ConstructAt<Record>(storage + recordOffset,
                                       Record{top_, chunk, address, bytes, chunk->used, id});
    top_ = record;
    chunk->used = endOffset;
    ++stats_.allocations.liveAllocations;
    stats_.allocations.liveBytes += bytes;
    ++stats_.allocations.totalAllocations;
    return Result<void*>::Success(address);
}

Result<void> StackAllocator::Deallocate(void* address) noexcept {
    if (!IsOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }
    if (inCall_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    CallGuard guard(inCall_);
    if (!address) {
        return Result<void>::Success();
    }

    // 외부 포인터 자체는 읽지 않는다. 중간 할당을 찾으면 LIFO 위반을 구별한다.
    Record* record = top_;
    while (record && record->address != address) {
        record = record->previous;
    }
    if (!record) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }
    if (record != top_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    top_ = record->previous;
    record->chunk->used = record->previousUsed;
    --stats_.allocations.liveAllocations;
    stats_.allocations.liveBytes -= record->bytes;
    DestroyAt(record);
    return Result<void>::Success();
}

StackMark StackAllocator::Mark() const noexcept {
    auto result = TryMark();
    if (!result) {
        platform::Fatal("StackAllocator mark accessed on wrong thread or during reentrant call");
    }
    return result.Value();
}

Result<StackMark> StackAllocator::TryMark() const noexcept {
    if (!IsOwnerThread()) {
        return Result<StackMark>::Failure(ErrorCode::WrongThread);
    }
    if (inCall_) {
        return Result<StackMark>::Failure(ErrorCode::Busy);
    }

    CallGuard guard(inCall_);
    return Result<StackMark>::Success(StackMark(ownerId_, epoch_, top_ ? top_->id : 0));
}

Result<void> StackAllocator::Rewind(StackMark mark) noexcept {
    if (!IsOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }
    if (inCall_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    CallGuard guard(inCall_);
    if (mark.owner_ != ownerId_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (mark.epoch_ != epoch_) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    if (mark.top_ == 0) {
        return top_ ? Result<void>::Failure(ErrorCode::Busy) : Result<void>::Success();
    }
    if (top_ && top_->id == mark.top_) {
        return Result<void>::Success();
    }

    for (Record* record = top_; record; record = record->previous) {
        if (record->id == mark.top_) {
            return Result<void>::Failure(ErrorCode::Busy);
        }
    }
    return Result<void>::Failure(ErrorCode::StaleHandle);
}

Result<void> StackAllocator::Reset() noexcept {
    if (!IsOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }
    if (inCall_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    CallGuard guard(inCall_);
    if (stats_.allocations.liveAllocations != 0) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (epoch_ == static_cast<uint64>(-1)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
        chunk->used = sizeof(Chunk);
    }
    ++epoch_;
    return Result<void>::Success();
}

Result<void> StackAllocator::Trim() noexcept {
    if (!IsOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }
    if (inCall_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    CallGuard guard(inCall_);
    if (stats_.allocations.liveAllocations != 0) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (epoch_ == static_cast<uint64>(-1)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    ReleaseChunks();
    ++epoch_;
    return Result<void>::Success();
}

StackStats StackAllocator::Stats() const noexcept {
    if (!IsOwnerThread()) {
        platform::Fatal("StackAllocator statistics accessed on wrong thread");
    }
    if (inCall_) {
        platform::Fatal("StackAllocator statistics accessed during reentrant call");
    }

    CallGuard guard(inCall_);
    return stats_;
}

void StackAllocator::ReleaseChunks() noexcept {
    Chunk* chunk = chunks_;
    while (chunk) {
        Chunk* next = chunk->next;
        DestroyAt(chunk);
        if (!backing_->Deallocate(chunk)) {
            platform::Fatal("StackAllocator backing storage ownership mismatch");
        }
        chunk = next;
    }

    chunks_ = nullptr;
    stats_.reservedBytes = 0;
    stats_.chunks = 0;
}

} // namespace rocks
