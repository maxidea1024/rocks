#include <InputActions/ActionEvent.h>

// 전이 이벤트다(Docs/Design/InputActions 3.4절 — UE EnhancedPlayerInput.cpp:120-176).
namespace rocks::input {

ActionEvent TransitionEvent(TriggerState previous, TriggerState current) noexcept {
    switch (previous) {
    case TriggerState::None:
        switch (current) {
        case TriggerState::Ongoing:
            return ActionEvent::Started;
        case TriggerState::Triggered:
            return ActionEvent::Started | ActionEvent::Triggered;
        case TriggerState::None:
            return ActionEvent::None;
        }

        break;
    case TriggerState::Ongoing:
        switch (current) {
        case TriggerState::None:
            return ActionEvent::Canceled;
        case TriggerState::Ongoing:
            return ActionEvent::Ongoing;
        case TriggerState::Triggered:
            return ActionEvent::Triggered;
        }

        break;
    case TriggerState::Triggered:
        switch (current) {
        case TriggerState::None:
            return ActionEvent::Completed;
        case TriggerState::Ongoing:
            return ActionEvent::Ongoing;
        case TriggerState::Triggered:
            return ActionEvent::Triggered;
        }

        break;
    }

    return ActionEvent::None;
}

} // namespace rocks::input
