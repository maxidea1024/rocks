#pragma once

#include <Base/Types.h>
#include <InputActions/TriggerTypes.h>

namespace rocks::input {

// 액션 이벤트다(UE ETriggerEvent — 비트 플래그, 3.4절).
enum class ActionEvent : uint8 {
    None = 0,
    Triggered = 1,
    Started = 2,
    Ongoing = 4,
    Canceled = 8,
    Completed = 16
};

constexpr ActionEvent operator|(ActionEvent a, ActionEvent b) noexcept {
    return static_cast<ActionEvent>(static_cast<uint8>(a) | static_cast<uint8>(b));
}

constexpr ActionEvent operator&(ActionEvent a, ActionEvent b) noexcept {
    return static_cast<ActionEvent>(static_cast<uint8>(a) & static_cast<uint8>(b));
}

constexpr bool HasAnyEvent(ActionEvent events, ActionEvent mask) noexcept {
    return (events & mask) != ActionEvent::None;
}

// 이전 상태 → 지금 상태의 이벤트다(UE EnhancedPlayerInput.cpp:120-176). None → Triggered는
// Started | Triggered다.
ROCKS_INPUTACTIONS_API ActionEvent TransitionEvent(TriggerState previous,
                                                   TriggerState current) noexcept;

} // namespace rocks::input
