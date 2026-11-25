#include <UiCore/KeyChord.h>

namespace rocks::ui {
namespace {

constexpr const char* mouseNames[] = {"MouseLeft", "MouseRight", "MouseMiddle", "MouseX1",
                                      "MouseX2"};
constexpr const char* gamepadNames[] = {
    "GamepadSouth",        "GamepadEast",          "GamepadWest",   "GamepadNorth",
    "GamepadLeftShoulder", "GamepadRightShoulder", "GamepadBack",   "GamepadStart",
    "GamepadLeftThumb",    "GamepadRightThumb",    "GamepadDPadUp", "GamepadDPadDown",
    "GamepadDPadLeft",     "GamepadDPadRight"};
static_assert(sizeof(mouseNames) / sizeof(*mouseNames) == input::MouseButtonCount);
static_assert(sizeof(gamepadNames) / sizeof(*gamepadNames) == input::GamepadButtonCount);

} // namespace

bool KeyChord::IsValid() const noexcept {
    if (key >= input::Key::Count || mouse > input::MouseButton::Count ||
        gamepad > input::GamepadButton::Count) {
        return false;
    }
    const int sources = (key != input::Key::None) + (mouse != input::MouseButton::Count) +
                        (gamepad != input::GamepadButton::Count);
    return sources == 1 || *this == KeyChord{};
}

const char* KeyChord::InputName() const noexcept {
    if (mouse < input::MouseButton::Count) {
        return mouseNames[static_cast<uint32>(mouse)];
    }
    if (gamepad < input::GamepadButton::Count) {
        return gamepadNames[static_cast<uint32>(gamepad)];
    }
    return input::KeyName(key);
}

Result<KeyChord> KeyChord::FromInputName(StringView name) noexcept {
    for (uint32 i = 0; i < input::KeyCount; ++i) {
        const auto key = static_cast<input::Key>(i);
        if (name == input::KeyName(key)) {
            return Result<KeyChord>::Success({key});
        }
    }
    for (uint32 i = 0; i < input::MouseButtonCount; ++i) {
        if (name == mouseNames[i]) {
            return Result<KeyChord>::Success({.mouse = static_cast<input::MouseButton>(i)});
        }
    }
    for (uint32 i = 0; i < input::GamepadButtonCount; ++i) {
        if (name == gamepadNames[i]) {
            return Result<KeyChord>::Success({.gamepad = static_cast<input::GamepadButton>(i)});
        }
    }
    return Result<KeyChord>::Failure(ErrorCode::NotFound);
}

} // namespace rocks::ui
