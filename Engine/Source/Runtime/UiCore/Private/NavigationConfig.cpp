#include <UiCore/NavigationConfig.h>

namespace rocks::ui {

NavigationConfig::NavigationConfig() noexcept {
    // UE FNavigationConfig 기본 규칙(NavigationConfig.cpp 19–29행). D-pad는 패드 가상 키가 생기는
    // UW8e2에서 더한다.
    (void)SetKeyRule(input::Key::Left, FocusDirection::Left);
    (void)SetKeyRule(input::Key::Right, FocusDirection::Right);
    (void)SetKeyRule(input::Key::Up, FocusDirection::Up);
    (void)SetKeyRule(input::Key::Down, FocusDirection::Down);
}

const NavigationConfig& NavigationConfig::Default() noexcept {
    static const NavigationConfig config;
    return config;
}

Result<void> NavigationConfig::SetKeyRule(input::Key key,
                                          Optional<FocusDirection> direction) noexcept {
    for (usize i = 0; i < ruleCount_; ++i) {
        if (rules_[i].key == key) {
            if (direction) {
                rules_[i].direction = *direction;
            } else {
                rules_[i] = rules_[--ruleCount_];
            }

            return Result<void>::Success();
        }
    }

    if (!direction) {
        return Result<void>::Success();
    }

    if (ruleCount_ == MaxKeyRules) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    rules_[ruleCount_++] = {key, *direction};
    return Result<void>::Success();
}

Optional<FocusDirection> NavigationConfig::DirectionFromKey(const input::InputEvent& event,
                                                            const KeyChord& chord) const noexcept {
    if (event.type != input::InputEventType::KeyDown) {
        return {};
    }

    for (usize i = 0; i < ruleCount_; ++i) {
        if (rules_[i].key == event.key) {
            return keyNavigation ? Optional<FocusDirection>(rules_[i].direction)
                                 : Optional<FocusDirection>{};
        }
    }

    // Tab은 Ctrl·Alt·Command가 없을 때만 초점을 옮긴다(그 조합은 단축키로 남긴다 — UE 70–77행).
    if (tabNavigation && event.key == input::Key::Tab && !chord.control && !chord.alt &&
        !chord.super) {
        return chord.shift ? FocusDirection::Previous : FocusDirection::Next;
    }

    return {};
}

NavigationAction NavigationConfig::ActionFromKey(const input::InputEvent& event) const noexcept {
    if (event.type != input::InputEventType::KeyDown &&
        event.type != input::InputEventType::KeyUp) {
        return NavigationAction::None;
    }

    switch (event.key) {
    case input::Key::Enter:
    case input::Key::Space:
        return NavigationAction::Accept;
    case input::Key::Escape:
        return NavigationAction::Back;
    default:
        return NavigationAction::None;
    }
}

} // namespace rocks::ui
