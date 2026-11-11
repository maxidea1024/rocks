#include <Core/Memory/ArenaAllocator.h>
#include <Base/Checked.h>
#include <Core/Move.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>

namespace rocks {

struct ArenaAllocator::Chunk {
    Chunk* next;
    usize capacity;
    usize used;
};

struct ArenaAllocator::Record {
    Record* next;
    void* address;
    usize bytes;
};

// 상위 allocator가 이 arena를 다시 호출해도 미완료 상태가 게시되지 않는다.
struct ArenaAllocator::CallGuard {
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

ArenaAllocator::ArenaAllocator(Allocator& backing, usize chunkBytes) noexcept
    : backing_(&backing), chunkBytes_(chunkBytes), ownerThread_(platform::CurrentThreadId()) {
}

ArenaAllocator::~ArenaAllocator() noexcept {
    if (!IsOwnerThread()) {
        platform::Fatal("ArenaAllocator destroyed on wrong thread");
    }
    if (inCall_) {
        platform::Fatal("ArenaAllocator destroyed during reentrant call");
    }
    CallGuard guard(inCall_);
    if (stats_.allocations.liveAllocations != 0) {
        platform::Fatal("ArenaAllocator destroyed with live allocations");
    }
    ReleaseChunks();
}

bool ArenaAllocator::IsOwnerThread() const noexcept {
    return platform::CurrentThreadId() == ownerThread_;
}

bool ArenaAllocator::TryLayout(const Chunk& chunk, usize bytes, usize alignment,
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

Result<void*> ArenaAllocator::Allocate(usize bytes, usize alignment, uint32 tag) noexcept {
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
        // 상위 allocator를 호출하기 전에 최대 padding과 byte 수를 overflow 검사한다.
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
            platform::Fatal("ArenaAllocator backing allocator returned invalid storage");
        }

        chunk = ConstructAt<Chunk>(storage, Chunk{chunks_, capacity, sizeof(Chunk)});
        if (!TryLayout(*chunk, bytes, alignment, recordOffset, dataOffset, endOffset)) {
            DestroyAt(chunk);
            if (!backing_->Deallocate(storage)) {
                platform::Fatal("ArenaAllocator backing storage ownership mismatch");
            }
            return Result<void*>::Failure(ErrorCode::Overflow);
        }

        chunks_ = chunk;
        stats_.reservedBytes += capacity;
        ++stats_.chunks;
    }

    auto* storage = reinterpret_cast<unsigned char*>(chunk);
    void* address = storage + dataOffset;
    auto* record = ConstructAt<Record>(storage + recordOffset, Record{records_, address, bytes});
    records_ = record;
    chunk->used = endOffset;
    ++stats_.allocations.liveAllocations;
    stats_.allocations.liveBytes += bytes;
    ++stats_.allocations.totalAllocations;
    return Result<void*>::Success(address);
}

Result<void> ArenaAllocator::Deallocate(void* address) noexcept {
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

    Record** link = &records_;
    while (*link && (*link)->address != address) {
        link = &(*link)->next;
    }
    if (!*link) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    Record* record = *link;
    *link = record->next;
    --stats_.allocations.liveAllocations;
    stats_.allocations.liveBytes -= record->bytes;
    DestroyAt(record);
    return Result<void>::Success();
}

Result<void> ArenaAllocator::Reset() noexcept {
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

    for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
        chunk->used = sizeof(Chunk);
    }
    return Result<void>::Success();
}

Result<void> ArenaAllocator::Trim() noexcept {
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

    ReleaseChunks();
    return Result<void>::Success();
}

ArenaStats ArenaAllocator::Stats() const noexcept {
    if (!IsOwnerThread()) {
        platform::Fatal("ArenaAllocator statistics accessed on wrong thread");
    }
    if (inCall_) {
        platform::Fatal("ArenaAllocator statistics accessed during reentrant call");
    }
    CallGuard guard(inCall_);
    return stats_;
}

void ArenaAllocator::ReleaseChunks() noexcept {
    Chunk* chunk = chunks_;
    while (chunk) {
        Chunk* next = chunk->next;
        DestroyAt(chunk);
        if (!backing_->Deallocate(chunk)) {
            platform::Fatal("ArenaAllocator backing storage ownership mismatch");
        }
        chunk = next;
    }
    chunks_ = nullptr;
    stats_.reservedBytes = 0;
    stats_.chunks = 0;
}

} // namespace rocks
