#include <Input/TouchIndexAllocator.h>

namespace rocks::input {

uint8 TouchIndexAllocator::Acquire(int64 pointerId) noexcept {
    const uint8 existing = Find(pointerId);
    if (existing != NoTouchIndex) {
        return existing;
    }

    for (uint32 index = 0; index < MaxTouches; ++index) {
        if (slots_[index].state == SlotState::Free) {
            slots_[index].pointerId = pointerId;
            slots_[index].state = SlotState::Active;
            return static_cast<uint8>(index);
        }
    }

    ++dropped_;
    return NoTouchIndex;
}

uint8 TouchIndexAllocator::Find(int64 pointerId) const noexcept {
    for (uint32 index = 0; index < MaxTouches; ++index) {
        if (slots_[index].state == SlotState::Active && slots_[index].pointerId == pointerId) {
            return static_cast<uint8>(index);
        }
    }

    return NoTouchIndex;
}

uint8 TouchIndexAllocator::Release(int64 pointerId) noexcept {
    const uint8 index = Find(pointerId);
    if (index != NoTouchIndex) {
        slots_[index].state = SlotState::Ended;
    }

    return index;
}

uint32 TouchIndexAllocator::ReleaseAll(uint8 (&indices)[MaxTouches]) noexcept {
    uint32 count = 0;
    for (uint32 index = 0; index < MaxTouches; ++index) {
        if (slots_[index].state == SlotState::Active) {
            slots_[index].state = SlotState::Ended;
            indices[count++] = static_cast<uint8>(index);
        }
    }

    return count;
}

void TouchIndexAllocator::EndFrame() noexcept {
    for (Slot& slot : slots_) {
        if (slot.state == SlotState::Ended) {
            slot.state = SlotState::Free;
        }
    }
}

} // namespace rocks::input
