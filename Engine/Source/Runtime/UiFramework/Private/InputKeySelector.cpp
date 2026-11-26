#include <UiFramework/InputKeySelector.h>
#include <Core/Text/StringBuilder.h>

namespace rocks::ui {

Result<void> InputKeySelector::Construct(Args&& args) noexcept {
    // 모양은 UE SInputKeySelector처럼 핵심 스타일 "Button"이다.
    auto result = Button::Construct({.content = args.content, .color = Move(args.color)});
    if (result) {
        SetDesiredSize({100, 24});
        result = SetSelectedKey(args.selectedKey);
    }

    if (result) {
        result = SetCaption(Move(args.caption));
    }

    if (result) {
        SetAllowModifiers(args.allowModifiers);
        SetAllowGamepad(args.allowGamepad);
        SetOnSelected(Move(args.onSelected));
    }

    return result;
}

InputKeySelector::InputKeySelector(Allocator& allocator) noexcept
    : Button(allocator), allocator_(allocator) {
    SetDesiredSize({100, 24});
    // 키 이름은 가운데다(UE SInputKeySelector의 ETextJustify::Center).
    SetContentAlignment(StackAlignment::Center, StackAlignment::Center);
}

Result<void> InputKeySelector::RefreshCaption(KeyChord chord, bool selecting) noexcept {
    if (!caption_) {
        return Result<void>::Success();
    }

    if (selecting) {
        return caption_.Execute("...");
    }

    if (chord == KeyChord{}) {
        return caption_.Execute("None");
    }

    String label(allocator_);
    auto result = FormatKeyChord(chord, label);
    if (!result) {
        return result;
    }

    return caption_.Execute(label.View());
}

Result<void> InputKeySelector::SetSelectedKey(KeyChord chord) noexcept {
    if (!chord.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto result = RefreshCaption(chord, false);
    if (result) {
        selected_ = chord;
        selecting_ = false;
    }

    return result;
}

KeyChord InputKeySelector::SelectedKey() const noexcept {
    return selected_;
}

bool InputKeySelector::IsSelecting() const noexcept {
    return selecting_;
}

void InputKeySelector::SetAllowModifiers(bool enabled) noexcept {
    allowModifiers_ = enabled;
}

void InputKeySelector::SetAllowGamepad(bool enabled) noexcept {
    allowGamepad_ = enabled;
}

void InputKeySelector::SetOnSelected(Delegate<void(KeyChord)>&& callback) noexcept {
    selectedEvent_ = Move(callback);
}

Result<void> InputKeySelector::SetCaption(Delegate<Result<void>(StringView)>&& callback) noexcept {
    caption_ = Move(callback);
    return RefreshCaption(selected_, selecting_);
}

void InputKeySelector::OnActivated() noexcept {
    if (RefreshCaption(selected_, true)) {
        selecting_ = true;
        selectingUser_ = activationUser_;
    }
}

void InputKeySelector::Cancel() noexcept {
    if (selecting_) {
        selecting_ = false;
        (void)RefreshCaption(selected_, false);
    }
}

bool InputKeySelector::OnPreviewKeyChord(uint32 user, const input::InputEvent& event,
                                         KeyChord chord) noexcept {
    if (!selecting_ || selectingUser_ != user) {
        return false;
    }

    if (event.repeat || event.key == input::Key::None || KeyModifierBit(event.key)) {
        return true;
    }

    if (event.key == input::Key::Escape) {
        Cancel();
        return true;
    }

    if (!allowModifiers_) {
        chord = {event.key};
    }

    Select(chord);
    return true;
}

void InputKeySelector::Select(KeyChord chord) noexcept {
    if (SetSelectedKey(chord)) {
        selectedEvent_.ExecuteIfBound(chord);
    }
}

PointerReply InputKeySelector::OnPreviewPointerDown(const PointerEvent& event) noexcept {
    if (selecting_ && selectingUser_ == event.userId && event.source == PointerSource::Mouse &&
        event.button < input::MouseButton::Count) {
        Select({.mouse = event.button});
        return {.handled = true};
    }

    return {};
}

PointerReply InputKeySelector::OnPointerEvent(const PointerEvent& event) noexcept {
    if (selecting_) {
        return {};
    }

    if (event.source == PointerSource::Mouse && event.type == PointerEventType::Down &&
        event.button == input::MouseButton::Right && selected_ != KeyChord{}) {
        Select({});
        return {.handled = true};
    }

    activationUser_ = event.userId;
    return Button::OnPointerEvent(event);
}

bool InputKeySelector::OnGamepadButtonDown(uint32 user, input::GamepadButton button) noexcept {
    if (selecting_ && user == selectingUser_) {
        if (allowGamepad_) {
            Select({.gamepad = button});
        }

        return true;
    }

    if (!selecting_ && allowGamepad_ && button == input::GamepadButton::West &&
        selected_ != KeyChord{}) {
        Select({});
        return true;
    }

    return false;
}

bool InputKeySelector::OnKeyEvent(uint32 user, const input::InputEvent& event) noexcept {
    if (selecting_) {
        return true;
    }

    activationUser_ = user;
    return Button::OnKeyEvent(user, event);
}

void InputKeySelector::OnFocusLost(uint32 user) noexcept {
    Button::OnFocusLost(user);
    if (user == selectingUser_) {
        Cancel();
    }
}

} // namespace rocks::ui
