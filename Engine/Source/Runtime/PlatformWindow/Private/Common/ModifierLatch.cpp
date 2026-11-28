#include "ModifierLatch.h"

namespace rocks::platform {

namespace {

// 비트 순서의 modifier 키다(Bit·KeyOf가 같은 표를 쓴다).
constexpr input::Key LatchKeys[ModifierLatch::Capacity] = {
    input::Key::LeftShift,    input::Key::RightShift, input::Key::LeftControl,
    input::Key::RightControl, input::Key::LeftAlt,    input::Key::RightAlt,
    input::Key::LeftSuper,    input::Key::RightSuper};

} // namespace

uint8 ModifierLatch::Bit(input::Key key) noexcept {
    for (uint32 i = 0; i < Capacity; ++i) {
        if (LatchKeys[i] == key) {
            return static_cast<uint8>(1u << i);
        }
    }

    return 0;
}

input::Key ModifierLatch::KeyOf(uint8 bit) noexcept {
    for (uint32 i = 0; i < Capacity; ++i) {
        if (bit == static_cast<uint8>(1u << i)) {
            return LatchKeys[i];
        }
    }

    return input::Key::None;
}

void ModifierLatch::NoteDown(input::Key key) noexcept {
    held_ = static_cast<uint8>(held_ | Bit(key));
}

bool ModifierLatch::NoteUp(input::Key key) noexcept {
    const uint8 bit = Bit(key);
    if (bit == 0) {
        return true;
    }

    if ((held_ & bit) == 0) {
        return false;
    }

    held_ = static_cast<uint8>(held_ & ~bit);
    return true;
}

uint8 ModifierLatch::Held() const noexcept {
    return held_;
}

uint32 ModifierLatch::Reconcile(bool keyMessagesPending, uint8 physicalDown,
                                input::Key (&released)[Capacity]) noexcept {
    if (keyMessagesPending) {
        return 0;
    }

    uint32 count = 0;
    for (uint32 i = 0; i < Capacity; ++i) {
        const auto bit = static_cast<uint8>(1u << i);
        if ((held_ & bit) != 0 && (physicalDown & bit) == 0) {
            held_ = static_cast<uint8>(held_ & ~bit);
            released[count++] = LatchKeys[i];
        }
    }

    return count;
}

void ModifierLatch::Clear() noexcept {
    held_ = 0;
}

} // namespace rocks::platform
