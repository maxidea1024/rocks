#include <Core/Memory/FixedBlockPool.h>

#include <Platform/Fatal.h>

namespace rocks {

Result<void> FixedBlockPool::Initialize(Allocator& allocator, usize blockBytes,
                                        usize blockAlignment, uint32 blocksPerPage,
                                        uint32 maxBlocks) noexcept {
    if (pages_ != nullptr || blockBytes == 0 || blockAlignment == 0 ||
        (blockAlignment & (blockAlignment - 1)) != 0 || blocksPerPage == 0 || maxBlocks == 0 ||
        maxBlocks > IndexedFreeList<>::MaxCapacity) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (blockBytes > MaxSize - (blockAlignment - 1)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    const usize stride = (blockBytes + blockAlignment - 1) & ~(blockAlignment - 1);
    if (stride > MaxSize / blocksPerPage) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    const uint32 maxPages = (maxBlocks + blocksPerPage - 1) / blocksPerPage;
    auto table = allocator.Allocate(sizeof(unsigned char*) * maxPages, alignof(unsigned char*));
    if (!table) {
        return Result<void>::Failure(table.Error());
    }

    if (auto initialized = free_.Initialize(allocator, maxBlocks, IndexedFreeListStart::Empty);
        !initialized) {
        if (!allocator.Deallocate(table.Value())) {
            platform::Fatal("FixedBlockPool page table deallocation was refused");
        }

        return initialized;
    }

    pages_ = static_cast<unsigned char**>(table.Value());
    allocator_ = &allocator;
    stride_ = stride;
    alignment_ = blockAlignment;
    blocksPerPage_ = blocksPerPage;
    maxBlocks_ = maxBlocks;
    maxPages_ = maxPages;
    pageCount_.Store<platform::MemoryOrder::Relaxed>(0);
    return Result<void>::Success();
}

void FixedBlockPool::Shutdown() noexcept {
    if (pages_ == nullptr) {
        return;
    }

    const uint32 pages = pageCount_.Load<platform::MemoryOrder::Acquire>();
    for (uint32 page = 0; page < pages; ++page) {
        if (!allocator_->Deallocate(pages_[page])) {
            platform::Fatal("FixedBlockPool page deallocation was refused");
        }
    }

    if (!allocator_->Deallocate(pages_)) {
        platform::Fatal("FixedBlockPool page table deallocation was refused");
    }

    free_.Shutdown();
    pages_ = nullptr;
    pageCount_.Store<platform::MemoryOrder::Relaxed>(0);
}

// 빈 목록에서 꺼내고, 없으면 잠금 아래에서 다시 본 뒤 페이지를 늘린다. 새 페이지의 첫 블록은 바로
// 주고 나머지는 빈 목록에 넣는다. 페이지 주소는 빈 목록 Push(release) 전에 쓰므로 그 색인을 꺼낸
// 스레드는 주소를 본다.
Result<FixedBlockPool::Block> FixedBlockPool::TryAllocate() noexcept {
    uint32 index = 0;
    if (free_.TryPop(index)) {
        return Result<Block>::Success(Block{Data(index), index});
    }

    platform::LockGuard lock(growMutex_);
    if (free_.TryPop(index)) {
        return Result<Block>::Success(Block{Data(index), index});
    }

    const uint32 page = pageCount_.Load<platform::MemoryOrder::Relaxed>();
    const uint32 first = page * blocksPerPage_;
    if (page == maxPages_ || first >= maxBlocks_) {
        return Result<Block>::Failure(ErrorCode::CapacityExceeded);
    }

    auto memory = allocator_->Allocate(stride_ * blocksPerPage_, alignment_);
    if (!memory) {
        return Result<Block>::Failure(memory.Error());
    }

    pages_[page] = static_cast<unsigned char*>(memory.Value());
    pageCount_.Store<platform::MemoryOrder::Release>(page + 1);
    const uint32 end = first + blocksPerPage_ < maxBlocks_ ? first + blocksPerPage_ : maxBlocks_;
    for (uint32 block = end; block-- > first + 1;) {
        free_.Push(block);
    }

    return Result<Block>::Success(Block{pages_[page], first});
}

void FixedBlockPool::Free(uint32 index) noexcept {
    if (index >= maxBlocks_) {
        platform::Fatal("FixedBlockPool block index out of range");
    }

    free_.Push(index);
}

void* FixedBlockPool::Data(uint32 index) const noexcept {
    return pages_[index / blocksPerPage_] + static_cast<usize>(index % blocksPerPage_) * stride_;
}

} // namespace rocks
