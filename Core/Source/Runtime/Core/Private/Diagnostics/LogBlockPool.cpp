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
    if (storage_ != nullptr || blockCount == 0 || blockCount >= NoLogBlock ||
        blockBytes < MinimumLogRenderBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (blockBytes > MaxSize / blockCount || blockCount > MaxSize / sizeof(uint32)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    auto storage = allocator.Allocate(blockCount * blockBytes, alignof(uint64));
    if (!storage) {
        return Result<void>::Failure(storage.Error());
    }

    auto next = allocator.Allocate(blockCount * sizeof(uint32), alignof(uint32));
    if (!next) {
        if (!allocator.Deallocate(storage.Value())) {
            platform::Fatal("LogBlockPool storage deallocation was refused");
        }

        return Result<void>::Failure(next.Error());
    }

    allocator_ = &allocator;
    storage_ = static_cast<uint8*>(storage.Value());
    next_ = static_cast<uint32*>(next.Value());
    blockCount_ = blockCount;
    blockBytes_ = blockBytes;
    // 처음에는 0 → 1 → … → 마지막 → 없음으로 잇는다.
    for (usize index = 0; index < blockCount; ++index) {
        next_[index] = index + 1 < blockCount ? static_cast<uint32>(index + 1) : NoLogBlock;
    }

    platform::AtomicStore<MemoryOrder::Release>(&head_.value, uint64{0});
    platform::AtomicStore<MemoryOrder::Relaxed>(&available_, static_cast<uint64>(blockCount));
    return Result<void>::Success();
}

void LogBlockPool::Shutdown() noexcept {
    if (storage_ == nullptr) {
        return;
    }

    if (!allocator_->Deallocate(storage_) || !allocator_->Deallocate(next_)) {
        platform::Fatal("LogBlockPool storage deallocation was refused");
    }

    storage_ = nullptr;
    next_ = nullptr;
    blockCount_ = 0;
    blockBytes_ = 0;
}

usize LogBlockPool::ApproximateAvailable() const noexcept {
    return static_cast<usize>(platform::AtomicLoad<MemoryOrder::Relaxed>(&available_));
}

bool LogBlockPool::TryBorrow(LogBlock& block) noexcept {
    if (storage_ == nullptr) {
        return false;
    }

    uint64 head = platform::AtomicLoad<MemoryOrder::Acquire>(&head_.value);
    for (;;) {
        const auto index = static_cast<uint32>(head & IndexMask);
        if (index == NoLogBlock) {
            return false;
        }

        // next[index]는 다른 스레드가 이 블록을 빌렸다 돌려주며 바꿀 수 있다. 그 경우 태그가 바뀌어
        // 아래 CAS가 실패하므로 읽은 값이 낡아도 쓰이지 않는다.
        const uint32 following = platform::AtomicLoad<MemoryOrder::Relaxed>(&next_[index]);
        const uint64 tag = (head >> 32) + 1;
        const uint64 replacement = (tag << 32) | following;
        if (platform::AtomicCompareExchangeWeak<MemoryOrder::Acquire, MemoryOrder::Acquire>(
                &head_.value, head, replacement)) {
            (void)platform::AtomicFetchSub<MemoryOrder::Relaxed>(&available_, uint64{1});
            block.index = index;
            block.bytes =
                Span<uint8>(storage_ + static_cast<usize>(index) * blockBytes_, blockBytes_);
            return true;
        }
    }
}

Span<const uint8> LogBlockPool::Resolve(uint32 index) const noexcept {
    if (storage_ == nullptr || index >= blockCount_) {
        return Span<const uint8>();
    }

    return Span<const uint8>(storage_ + static_cast<usize>(index) * blockBytes_, blockBytes_);
}

void LogBlockPool::Release(uint32 index) noexcept {
    if (storage_ == nullptr || index >= blockCount_) {
        return;
    }

    uint64 head = platform::AtomicLoad<MemoryOrder::Relaxed>(&head_.value);
    for (;;) {
        platform::AtomicStore<MemoryOrder::Relaxed>(&next_[index],
                                                    static_cast<uint32>(head & IndexMask));
        const uint64 tag = (head >> 32) + 1;
        const uint64 replacement = (tag << 32) | index;
        // release: 이 블록에 쓴 내용과 next[index]를 다음에 빌리는 쪽(acquire)이 본다.
        if (platform::AtomicCompareExchangeWeak<MemoryOrder::Release, MemoryOrder::Relaxed>(
                &head_.value, head, replacement)) {
            (void)platform::AtomicFetchAdd<MemoryOrder::Relaxed>(&available_, uint64{1});
            return;
        }
    }
}

} // namespace rocks
