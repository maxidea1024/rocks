#include "TraceSessionInternal.h"

#include <Base/Endian.h>
#include <Platform/Memory.h>

namespace rocks::detail {

using platform::AtomicCompareExchangeWeak;
using platform::AtomicFetchAdd;
using platform::AtomicFetchSub;
using platform::AtomicLoad;
using platform::AtomicStore;

namespace {

// 자유 목록 머리의 하위 32비트(색인 + 1)와 tag 한 칸이다.
constexpr uint64 FreeIndexMask = NumericLimits<uint32>::Max();
constexpr uint64 FreeTagUnit = 1ull << 32;
// 페이지 정렬이다. 블록이 cache line 경계에서 시작하게 한다.
constexpr usize PageAlignment = 64;
// 블록 색인은 uint32이고, 색인 + 1을 자유 목록에 쓰므로 그보다 하나 적다.
constexpr uint64 MaxPoolBlocks = NumericLimits<uint32>::Max() - 1;

} // namespace

// ---- 블록 풀 ----

Result<void> TraceBlockPool::Initialize(Allocator& allocator, usize maxBytes) noexcept {
    const usize pages = maxBytes / TraceBlockLayout::PageBytes;
    if (pages == 0 || pages > MaxPoolBlocks / TraceBlockLayout::PageBlocks) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto table = allocator.Allocate(pages * sizeof(TraceBlock*), alignof(TraceBlock*));
    if (!table) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    allocator_ = &allocator;
    pages_ = static_cast<TraceBlock**>(table.Value());
    platform::MemoryZero(pages_, pages * sizeof(TraceBlock*));
    maxPages_ = static_cast<uint32>(pages);
    return Result<void>::Success();
}

void TraceBlockPool::Release() noexcept {
    if (pages_ == nullptr) {
        return;
    }

    for (uint32 page = 0; page < pageCount_; ++page) {
        auto freed = allocator_->Deallocate(pages_[page]);
        (void)freed;
    }

    auto freed = allocator_->Deallocate(pages_);
    (void)freed;
    pages_ = nullptr;
    maxPages_ = 0;
    pageCount_ = 0;
    freeHead_ = 0;
}

TraceBlock* TraceBlockPool::BlockAt(uint32 index) const noexcept {
    return pages_[index / TraceBlockLayout::PageBlocks] + index % TraceBlockLayout::PageBlocks;
}

TraceBlock* TraceBlockPool::TryPop() noexcept {
    uint64 head = AtomicLoad<MemoryOrder::Acquire>(&freeHead_);
    for (;;) {
        const uint32 slot = static_cast<uint32>(head & FreeIndexMask);
        if (slot == 0) {
            return nullptr;
        }

        // 다른 스레드가 먼저 꺼내 재사용한 블록의 freeNext를 읽을 수 있다. 그 경우 tag가 바뀌어
        // CAS가 실패하므로 읽은 값은 쓰이지 않는다.
        TraceBlock* block = BlockAt(slot - 1);
        const uint32 next = AtomicLoad<MemoryOrder::Relaxed>(&block->header.freeNext);
        const uint64 replacement = ((head & ~FreeIndexMask) + FreeTagUnit) | next;
        if (AtomicCompareExchangeWeak<MemoryOrder::Acquire, MemoryOrder::Acquire>(&freeHead_, head,
                                                                                  replacement)) {
            return block;
        }
    }
}

void TraceBlockPool::Push(TraceBlock* block) noexcept {
    uint64 head = AtomicLoad<MemoryOrder::Relaxed>(&freeHead_);
    for (;;) {
        AtomicStore<MemoryOrder::Relaxed>(&block->header.freeNext,
                                          static_cast<uint32>(head & FreeIndexMask));
        const uint64 replacement = ((head & ~FreeIndexMask) + FreeTagUnit) |
                                   (static_cast<uint64>(block->header.index) + 1);
        if (AtomicCompareExchangeWeak<MemoryOrder::Release, MemoryOrder::Relaxed>(&freeHead_, head,
                                                                                  replacement)) {
            return;
        }
    }
}

void TraceBlockPool::CountAcquired() noexcept {
    const uint64 inUse = AtomicFetchAdd<MemoryOrder::Relaxed>(&inUse_, 1) + 1;
    uint64 peak = AtomicLoad<MemoryOrder::Relaxed>(&peak_);
    while (inUse > peak) {
        // 실패하면 peak에 현재 값이 들어오므로 다시 비교한다.
        if (AtomicCompareExchangeWeak<MemoryOrder::Relaxed, MemoryOrder::Relaxed>(&peak_, peak,
                                                                                  inUse)) {
            return;
        }
    }
}

TraceBlock* TraceBlockPool::TryAcquire() noexcept {
    if (TraceBlock* block = TryPop()) {
        CountAcquired();
        return block;
    }

    // 새 페이지는 잠금 하나로 직렬화한다. 기다리는 동안 다른 스레드가 페이지를 넣었을 수 있으므로
    // 잠근 뒤 다시 꺼내 본다. 상한에 닿았으면 기다리지 않고 실패한다.
    platform::LockGuard lock(growMutex_);
    if (TraceBlock* block = TryPop()) {
        CountAcquired();
        return block;
    }

    const uint32 page = pageCount_;
    if (page >= maxPages_) {
        return nullptr;
    }

    auto memory = allocator_->Allocate(TraceBlockLayout::PageBytes, PageAlignment);
    if (!memory) {
        return nullptr;
    }

    auto* blocks = static_cast<TraceBlock*>(memory.Value());
    pages_[page] = blocks;
    for (uint32 index = 0; index < TraceBlockLayout::PageBlocks; ++index) {
        blocks[index].header.index =
            page * static_cast<uint32>(TraceBlockLayout::PageBlocks) + index;
        blocks[index].header.next = nullptr;
    }

    AtomicStore<MemoryOrder::Relaxed>(&pageCount_, page + 1);
    // 첫 블록은 호출자에게 주고 나머지는 자유 목록에 넣는다. Push의 release가 pages_ 칸을 공개한다.
    for (uint32 index = TraceBlockLayout::PageBlocks - 1; index > 0; --index) {
        Push(&blocks[index]);
    }

    CountAcquired();
    return &blocks[0];
}

void TraceBlockPool::Free(TraceBlock* block) noexcept {
    Push(block);
    AtomicFetchSub<MemoryOrder::Relaxed>(&inUse_, 1);
}

// ---- 정의 버퍼 ----

Result<void> TraceDefinitionBuffer::Initialize(Allocator& allocator, usize capacity) noexcept {
    if (capacity < 4 * TraceFormat::MaxDefinitionRecordBytes ||
        capacity > static_cast<usize>(ReadyBit)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto memory = allocator.Allocate(capacity, RecordAlignment);
    if (!memory) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    allocator_ = &allocator;
    data_ = static_cast<uint8*>(memory.Value());
    platform::MemoryZero(data_, capacity);
    capacity_ = static_cast<uint32>(capacity);
    return Result<void>::Success();
}

void TraceDefinitionBuffer::Release() noexcept {
    if (data_ != nullptr) {
        auto freed = allocator_->Deallocate(data_);
        (void)freed;
        data_ = nullptr;
    }
}

uint8* TraceDefinitionBuffer::TryReserve(usize recordBytes) noexcept {
    if (recordBytes > TraceFormat::MaxDefinitionRecordBytes) {
        return nullptr;
    }

    const uint32 need = static_cast<uint32>(
        RecordHeaderBytes + ((recordBytes + RecordAlignment - 1) & ~(RecordAlignment - 1)));
    uint32 reserved = AtomicLoad<MemoryOrder::Relaxed>(&reserved_);
    for (;;) {
        if (need > capacity_ - reserved) {
            return nullptr;
        }

        if (AtomicCompareExchangeWeak<MemoryOrder::Relaxed, MemoryOrder::Relaxed>(
                &reserved_, reserved, reserved + need)) {
            return data_ + reserved + RecordHeaderBytes;
        }
    }
}

void TraceDefinitionBuffer::Commit(uint8* record, usize recordBytes) noexcept {
    AtomicStore<MemoryOrder::Release>(reinterpret_cast<uint32*>(record - RecordHeaderBytes),
                                      static_cast<uint32>(recordBytes) | ReadyBit);
}

// ---- 레코드 직렬화 ----

usize TraceRecordBytes(const TraceRecordShape& shape) noexcept {
    usize bytes = sizeof(uint16) + (shape.ordered ? sizeof(uint64) : 0) + shape.fixedBytes;
    for (const TraceVariablePart& part : shape.variable) {
        if (part.bytes > TraceFormat::MaxRecordBytes) {
            return MaxSize;
        }

        bytes += sizeof(uint32) + part.bytes;
        if (bytes > TraceFormat::MaxRecordBytes) {
            return MaxSize;
        }
    }

    return bytes > TraceFormat::MaxRecordBytes ? MaxSize : bytes;
}

namespace {

struct FlatSink {
    uint8* cursor;

    void Write(const void* source, usize bytes) noexcept {
        platform::MemoryCopy(cursor, source, bytes);
        cursor += bytes;
    }
};

} // namespace

void EncodeTraceRecord(uint8* destination, const TraceRecordShape& shape) noexcept {
    FlatSink sink{destination};
    WriteTraceRecord(sink, shape);
}

} // namespace rocks::detail
