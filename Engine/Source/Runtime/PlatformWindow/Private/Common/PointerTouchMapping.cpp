#include "PointerTouchMapping.h"

namespace rocks::platform::pointer {

PointerTouchAction TouchActionOf(uint32 message, uint32 pointerFlags) noexcept {
    switch (message) {
    case PointerDownMessage:
        return PointerTouchAction::Began;
    case PointerUpdateMessage:
        return PointerTouchAction::Moved;
    case PointerUpMessage:
        return (pointerFlags & PointerFlagCanceled) != 0 ? PointerTouchAction::Cancelled
                                                         : PointerTouchAction::Ended;
    case PointerCaptureChangedMessage:
        return PointerTouchAction::Cancelled;
    default:
        return PointerTouchAction::None;
    }
}

bool IsTouchPointerType(uint32 pointerType) noexcept {
    return pointerType == PointerTypeTouch || pointerType == PointerTypePen;
}

float NormalizePointerPressure(uint32 pressure) noexcept {
    return pressure >= MaxPointerPressure
               ? 1.0f
               : static_cast<float>(pressure) / static_cast<float>(MaxPointerPressure);
}

} // namespace rocks::platform::pointer
