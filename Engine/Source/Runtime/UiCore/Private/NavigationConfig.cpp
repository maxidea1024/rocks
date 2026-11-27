#include <UiCore/NavigationConfig.h>

namespace rocks::ui {

namespace {

// 아날로그 반복 간격(초)이다(UE GetRepeatRateForPressure 153–162행): 첫 반복 전 0.5초, 그 뒤
// 0.25초, 0.9보다 세게 밀면 절반.
constexpr double AnalogFirstRepeatSeconds = 0.5;
constexpr double AnalogRepeatSeconds = 0.25;
constexpr float AnalogFastPressure = 0.9f;
constexpr double AnalogFastRepeatScale = 0.5;

} // namespace

NavigationConfig::NavigationConfig() noexcept {
    // UE FNavigationConfig 기본 규칙(NavigationConfig.cpp 19–29행). 패드 D-pad는
    // DirectionFromGamepad가 본다(Rocks 패드 버튼은 키가 아니다).
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

Optional<FocusDirection>
NavigationConfig::DirectionFromGamepad(input::GamepadButton button) const noexcept {
    if (!keyNavigation) {
        return {};
    }

    switch (button) {
    case input::GamepadButton::DPadLeft:
        return FocusDirection::Left;
    case input::GamepadButton::DPadRight:
        return FocusDirection::Right;
    case input::GamepadButton::DPadUp:
        return FocusDirection::Up;
    case input::GamepadButton::DPadDown:
        return FocusDirection::Down;
    default:
        return {};
    }
}

NavigationAction NavigationConfig::ActionFromGamepad(input::GamepadButton button) const noexcept {
    // UE Virtual_Gamepad_Accept·Back(서양 배치 South·East).
    switch (button) {
    case input::GamepadButton::South:
        return NavigationAction::Accept;
    case input::GamepadButton::East:
        return NavigationAction::Back;
    default:
        return NavigationAction::None;
    }
}

Optional<FocusDirection> NavigationConfig::DirectionFromAnalog(float x, float y) const noexcept {
    if (!analogNavigation) {
        return {};
    }

    const bool horizontal = x < -analogHorizontalThreshold || x > analogHorizontalThreshold;
    const bool vertical = y < -analogVerticalThreshold || y > analogVerticalThreshold;
    const float ax = x < 0 ? -x : x;
    const float ay = y < 0 ? -y : y;
    if (horizontal && (!vertical || ax >= ay)) {
        return x < 0 ? FocusDirection::Left : FocusDirection::Right;
    }

    if (vertical) {
        return y > 0 ? FocusDirection::Up : FocusDirection::Down;
    }

    return {};
}

double NavigationConfig::AnalogRepeatInterval(float pressure, uint32 repeats) noexcept {
    const double interval = repeats == 0 ? AnalogFirstRepeatSeconds : AnalogRepeatSeconds;
    return pressure > AnalogFastPressure ? interval * AnalogFastRepeatScale : interval;
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
