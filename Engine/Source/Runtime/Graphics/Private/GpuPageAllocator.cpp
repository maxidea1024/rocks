#include <Graphics/GpuPageAllocator.h>
#include <Platform/Fatal.h>

namespace rocks::graphics {

namespace {

bool IsPowerOfTwo(uint64 value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

} // namespace

GpuPageAllocator::GpuPageAllocator(Allocator& allocator) noexcept : free_(allocator) {
}

Result<void> GpuPageAllocator::Reset(uint64 capacity) noexcept {
    if (capacity == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    free_.Clear();
    auto pushed = free_.TryPushBack(Range{0, capacity});
    if (!pushed) {
        return pushed;
    }

    capacity_ = capacity;
    used_ = 0;
    return Result<void>::Success();
}

Result<GpuPageAllocation> GpuPageAllocator::Allocate(uint64 size, uint64 alignment) noexcept {
    if (size == 0 || !IsPowerOfTwo(alignment)) {
        return Result<GpuPageAllocation>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < free_.Size(); ++index) {
        const Range range = free_[index];
        const uint64 aligned = (range.offset + alignment - 1) & ~(alignment - 1);
        if (aligned < range.offset || aligned - range.offset > range.size ||
            size > range.size - (aligned - range.offset)) {
            continue;
        }

        GpuPageAllocation allocation;
        allocation.offset = aligned;
        allocation.size = size;
        allocation.reservedOffset = range.offset;
        allocation.reservedSize = aligned - range.offset + size;
        const uint64 rest = range.size - allocation.reservedSize;
        if (rest == 0) {
            auto removed = free_.TryRemoveAt(index);
            (void)removed;
        } else {
            free_[index] = Range{range.offset + allocation.reservedSize, rest};
        }

        used_ += allocation.reservedSize;
        return Result<GpuPageAllocation>::Success(allocation);
    }

    return Result<GpuPageAllocation>::Failure(ErrorCode::CapacityExceeded);
}

void GpuPageAllocator::Free(const GpuPageAllocation& allocation) noexcept {
    const uint64 begin = allocation.reservedOffset;
    const uint64 size = allocation.reservedSize;
    if (size == 0 || begin > capacity_ || size > capacity_ - begin || size > used_) {
        platform::Fatal("GPU page range freed out of range");
    }

    // 넣을 자리(begin보다 뒤에서 시작하는 첫 빈 구간)를 찾는다.
    usize index = 0;
    while (index < free_.Size() && free_[index].offset < begin) {
        ++index;
    }

    const bool overlapsPrevious =
        index > 0 && free_[index - 1].offset + free_[index - 1].size > begin;
    const bool overlapsNext = index < free_.Size() && begin + size > free_[index].offset;
    if (overlapsPrevious || overlapsNext) {
        platform::Fatal("GPU page range freed twice or overlapping a free range");
    }

    const bool joinsPrevious =
        index > 0 && free_[index - 1].offset + free_[index - 1].size == begin;
    const bool joinsNext = index < free_.Size() && begin + size == free_[index].offset;
    if (joinsPrevious || joinsNext) {
        used_ -= size;
    }

    if (joinsPrevious && joinsNext) {
        free_[index - 1].size += size + free_[index].size;
        auto removed = free_.TryRemoveAt(index);
        (void)removed;
        return;
    }

    if (joinsPrevious) {
        free_[index - 1].size += size;
        return;
    }

    if (joinsNext) {
        free_[index].offset = begin;
        free_[index].size += size;
        return;
    }

    // 빈 구간이 하나 늘어난다. 늘리지 못하면 그 구간을 쓰는 중으로 남긴다(누수지만 겹쳐 주지는
    // 않는다).
    if (free_.TryInsert(Range{begin, size}, index)) {
        used_ -= size;
    }
}

} // namespace rocks::graphics
