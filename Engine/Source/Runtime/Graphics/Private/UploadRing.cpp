#include <Graphics/UploadRing.h>

namespace rocks::graphics {

namespace {

constexpr uint64 AlignUp(uint64 value, uint64 alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

UploadRingAllocator::UploadRingAllocator(Allocator& allocator) noexcept : batches_(allocator) {
}

Result<void> UploadRingAllocator::Reset(uint64 capacity) noexcept {
    if (capacity == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    batches_.Clear();
    capacity_ = capacity;
    head_ = 0;
    tail_ = 0;
    used_ = 0;
    openBytes_ = 0;
    wraps_ = 0;
    return Result<void>::Success();
}

Result<uint64> UploadRingAllocator::Allocate(uint64 size, uint64 alignment) noexcept {
    if (size == 0 || size > capacity_ || alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }

    // 묶음 기록 칸을 미리 확보한다(CloseBatch가 실패하지 않게).
    auto reserved = batches_.TryEnsureCapacity(batches_.Size() + 1);
    if (!reserved) {
        return Result<uint64>::Failure(reserved.Error());
    }

    if (used_ == capacity_) {
        return Result<uint64>::Failure(ErrorCode::CapacityExceeded);
    }

    uint64 offset = 0;
    uint64 consumed = 0;
    const uint64 aligned = AlignUp(head_, alignment);
    if (head_ >= tail_) {
        // 빈 공간은 [head, 끝)과 [0, tail)이다.
        if (aligned + size <= capacity_) {
            offset = aligned;
            consumed = aligned + size - head_;
            head_ = aligned + size;
        } else if (used_ == 0) {
            // 비었으면 링 전체가 비어 있다. 버릴 끝 칸 없이 0으로 감는다.
            offset = 0;
            consumed = size;
            head_ = size;
            tail_ = 0;
            ++wraps_;
        } else if (size <= tail_) {
            // 끝에 남은 칸을 버리고 0으로 감는다.
            offset = 0;
            consumed = (capacity_ - head_) + size;
            head_ = size;
            ++wraps_;
        } else {
            return Result<uint64>::Failure(ErrorCode::CapacityExceeded);
        }
    } else {
        // 빈 공간은 [head, tail)이다.
        if (aligned + size > tail_) {
            return Result<uint64>::Failure(ErrorCode::CapacityExceeded);
        }

        offset = aligned;
        consumed = aligned + size - head_;
        head_ = aligned + size;
    }

    used_ += consumed;
    openBytes_ += consumed;
    return Result<uint64>::Success(offset);
}

void UploadRingAllocator::CloseBatch(uint64 fence) noexcept {
    if (openBytes_ == 0) {
        return;
    }

    // Allocate가 칸을 확보했으므로 실패하지 않는다.
    auto pushed = batches_.TryPushBack({head_, openBytes_, fence});
    (void)pushed;
    openBytes_ = 0;
}

void UploadRingAllocator::Reclaim(uint64 completed) noexcept {
    usize count = 0;
    while (count < batches_.Size() && batches_[count].fence <= completed) {
        tail_ = batches_[count].end;
        used_ -= batches_[count].bytes;
        ++count;
    }

    if (count != 0) {
        auto removed = batches_.TryRemoveAt(0, count);
        (void)removed;
    }
}

uint64 UploadRingAllocator::OldestFence() const noexcept {
    return batches_.IsEmpty() ? 0u : batches_[0].fence;
}

} // namespace rocks::graphics
