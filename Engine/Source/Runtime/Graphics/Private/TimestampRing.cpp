#include <Graphics/TimestampRing.h>

namespace rocks::graphics {

TimestampRing::TimestampRing(Allocator& allocator) noexcept
    : slots_(allocator), samples_(allocator), scratch_(allocator) {
}

Result<void> TimestampRing::Reset(uint32 slots, uint32 queriesPerSlot) noexcept {
    if (slots == 0 || queriesPerSlot == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    slots_.Clear();
    samples_.Clear();
    auto sized = slots_.TryResize(slots);
    if (!sized) {
        return sized;
    }

    auto scratch = scratch_.TryResize(queriesPerSlot);
    if (!scratch) {
        slots_.Clear();
        return scratch;
    }

    perSlot_ = queriesPerSlot;
    next_ = 0;
    oldest_ = 0;
    inUse_ = 0;
    nextQuery_ = 1;
    dropped_ = 0;
    return Result<void>::Success();
}

Result<TimestampWrite> TimestampRing::Allocate() noexcept {
    if (slots_.IsEmpty()) {
        return Result<TimestampWrite>::Failure(ErrorCode::Unsupported);
    }

    const uint32 slotCount = static_cast<uint32>(slots_.Size());
    // 열린 slot은 언제나 가장 최근에 연 slot(next_ 바로 앞)이다.
    const uint32 last = (next_ + slotCount - 1) % slotCount;
    Slot* slot = inUse_ != 0 && slots_[last].state == SlotState::Open ? &slots_[last] : nullptr;
    uint32 position = last;
    if (slot == nullptr) {
        if (inUse_ == slotCount) {
            return Result<TimestampWrite>::Failure(ErrorCode::Busy);
        }

        position = next_;
        slot = &slots_[position];
        slot->state = SlotState::Open;
        slot->valid = true;
        slot->count = 0;
        slot->fence = 0;
        slot->firstQuery = nextQuery_;
        next_ = (next_ + 1) % slotCount;
        ++inUse_;
    }

    if (slot->count == perSlot_) {
        return Result<TimestampWrite>::Failure(ErrorCode::CapacityExceeded);
    }

    TimestampWrite write;
    write.query = nextQuery_++;
    write.index = position * perSlot_ + slot->count;
    ++slot->count;
    return Result<TimestampWrite>::Success(write);
}

TimestampRange TimestampRing::OpenRange() const noexcept {
    TimestampRange range;
    if (inUse_ == 0) {
        return range;
    }

    const uint32 slotCount = static_cast<uint32>(slots_.Size());
    const uint32 last = (next_ + slotCount - 1) % slotCount;
    if (slots_[last].state == SlotState::Open) {
        range.first = last * perSlot_;
        range.count = slots_[last].count;
    }

    return range;
}

void TimestampRing::Close(uint64 fence, bool valid) noexcept {
    if (inUse_ == 0) {
        return;
    }

    const uint32 slotCount = static_cast<uint32>(slots_.Size());
    Slot& slot = slots_[(next_ + slotCount - 1) % slotCount];
    if (slot.state != SlotState::Open) {
        return;
    }

    slot.state = SlotState::Closed;
    slot.fence = fence;
    slot.valid = valid;
}

void TimestampRing::Harvest(uint64 completed, TimestampSource& source) noexcept {
    const uint32 slotCount = static_cast<uint32>(slots_.Size());
    while (inUse_ != 0) {
        Slot& slot = slots_[oldest_];
        if (slot.state != SlotState::Closed || slot.fence > completed) {
            return;
        }

        TimestampReadState state = TimestampReadState::Disjoint;
        if (slot.valid) {
            state = source.ReadTimestamps(oldest_ * perSlot_, slot.count, scratch_.Data());
            if (state == TimestampReadState::NotReady) {
                return;
            }
        }

        for (uint32 index = 0; index < slot.count; ++index) {
            TimestampSample sample;
            sample.query = slot.firstQuery + index;
            sample.ticks = state == TimestampReadState::Ready ? scratch_[index] : 0;
            sample.disjoint = state != TimestampReadState::Ready;
            if (!samples_.TryPushBack(sample)) {
                ++dropped_;
            }
        }

        slot.state = SlotState::Free;
        slot.count = 0;
        oldest_ = (oldest_ + 1) % slotCount;
        --inUse_;
    }
}

usize TimestampRing::Collect(Span<TimestampSample> out) noexcept {
    const usize count = out.Size() < samples_.Size() ? out.Size() : samples_.Size();
    for (usize index = 0; index < count; ++index) {
        out[index] = samples_[index];
    }

    if (count != 0) {
        auto removed = samples_.TryRemoveAt(0, count);
        (void)removed;
    }

    return count;
}

} // namespace rocks::graphics
