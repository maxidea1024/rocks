#include <Core/Diagnostics/LogBlockPool.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>

namespace rocks {

using platform::MemoryOrder;

LogBlockPool::~LogBlockPool() noexcept {
    Shutdown();
}

Result<void> LogBlockPool::Initialize(Allocator& allocator, usize blockCount,
                                      usize blockBytes) noexcept {
    if (storage_ != nullptr || blockCount == 0 || blockCount > IndexedFreeList<>::MaxCapacity ||
        blockBytes < MinimumLogRenderBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (blockBytes > MaxSize / blockCount) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    auto storage = allocator.Allocate(blockCount * blockBytes, alignof(uint64));
    if (!storage) {
        return Result<void>::Failure(storage.Error());
    }

    // 처음에는 모든 블록이 비어 있다(0이 맨 위).
    if (auto initialized = free_.Initialize(allocator, blockCount, IndexedFreeListStart::AllFree);
        !initialized) {
        if (!allocator.Deallocate(storage.Value())) {
            platform::Fatal("LogBlockPool storage deallocation was refused");
        }

        return initialized;
    }

    allocator_ = &allocator;
    storage_ = static_cast<uint8*>(storage.Value());
    blockCount_ = blockCount;
    blockBytes_ = blockBytes;
    available_.Store<MemoryOrder::Relaxed>(static_cast<uint64>(blockCount));
    return Result<void>::Success();
}

void LogBlockPool::Shutdown() noexcept {
    if (storage_ == nullptr) {
        return;
    }

    if (!allocator_->Deallocate(storage_)) {
        platform::Fatal("LogBlockPool storage deallocation was refused");
    }

    free_.Shutdown();
    storage_ = nullptr;
    blockCount_ = 0;
    blockBytes_ = 0;
}

usize LogBlockPool::ApproximateAvailable() const noexcept {
    return static_cast<usize>(available_.Load<MemoryOrder::Relaxed>());
}

// 빈 목록의 pop(acquire)이 앞서 돌려준 쪽의 release와 짝이라 블록 내용을 본다.
bool LogBlockPool::TryBorrow(LogBlock& block) noexcept {
    uint32 index = 0;
    if (storage_ == nullptr || !free_.TryPop(index)) {
        return false;
    }

    (void)available_.FetchSub<MemoryOrder::Relaxed>(1);
    block.index = index;
    block.bytes = Span<uint8>(storage_ + static_cast<usize>(index) * blockBytes_, blockBytes_);
    return true;
}

Span<const uint8> LogBlockPool::Resolve(uint32 index) const noexcept {
    if (storage_ == nullptr || index >= blockCount_) {
        return Span<const uint8>();
    }

    return Span<const uint8>(storage_ + static_cast<usize>(index) * blockBytes_, blockBytes_);
}

// 빈 목록의 push(release): 이 블록에 쓴 내용을 다음에 빌리는 쪽(acquire)이 본다.
void LogBlockPool::Release(uint32 index) noexcept {
    if (storage_ == nullptr || index >= blockCount_) {
        return;
    }

    free_.Push(index);
    (void)available_.FetchAdd<MemoryOrder::Relaxed>(1);
}

} // namespace rocks
