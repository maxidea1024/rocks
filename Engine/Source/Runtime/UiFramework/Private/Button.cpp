#include <UiFramework/Button.h>

namespace rocks::ui {

Result<void> Button::Construct(Args&& args) noexcept {
    auto status = Frame::Construct(
        {.content = args.content, .color = Move(args.color), .padding = args.padding});
    if (status) {
        SetPressedColor(args.pressedColor);
        SetOnClicked(Move(args.onClicked));
        SetOnPressed(Move(args.onPressed));
        SetOnReleased(Move(args.onReleased));
        SetOnHovered(Move(args.onHovered));
        SetOnUnhovered(Move(args.onUnhovered));
        if (args.hoveredColor) {
            SetHoveredColor(*args.hoveredColor);
        }
        SetFocusable(args.focusable);
        status = SetInputMethods(args.clickMethod, args.touchMethod, args.pressMethod);
    }
    return status;
}

Button::Button(Allocator& allocator) noexcept : Frame(allocator) {
}

void Button::SetOnClicked(Delegate<void()>&& callback) noexcept {
    onClicked_ = Move(callback);
}

void Button::SetOnPressed(Delegate<void()>&& callback) noexcept {
    onPressed_ = Move(callback);
}
void Button::SetOnReleased(Delegate<void()>&& callback) noexcept {
    onReleased_ = Move(callback);
}
void Button::SetOnHovered(Delegate<void()>&& callback) noexcept {
    onHovered_ = Move(callback);
}
void Button::SetOnUnhovered(Delegate<void()>&& callback) noexcept {
    onUnhovered_ = Move(callback);
}
Result<void> Button::SetInputMethods(ButtonClickMethod click, ButtonClickMethod touch,
                                     ButtonPressMethod press) noexcept {
    if (click > ButtonClickMethod::Precise || touch > ButtonClickMethod::Precise ||
        press > ButtonPressMethod::Up) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    clickMethod_ = click;
    touchMethod_ = touch;
    pressMethod_ = press;
    return Result<void>::Success();
}
void Button::SetFocusable(bool enabled) noexcept {
    focusable_ = enabled;
    if (!enabled && keyPressed_) {
        OnKeyCancelled(keyUserId_, pressedKey_);
    }
}
Result<void> Button::SetStateBrushes(Optional<Brush> hovered, Optional<Brush> pressed) noexcept {
    if ((hovered && !hovered->IsValid()) || (pressed && !pressed->IsValid())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    const bool changed = hovered.HasValue() != hoveredBrush_.HasValue() ||
                         pressed.HasValue() != pressedBrush_.HasValue() ||
                         (hovered && *hovered != *hoveredBrush_) ||
                         (pressed && *pressed != *pressedBrush_);
    if (changed) {
        hoveredBrush_ = Move(hovered);
        pressedBrush_ = Move(pressed);
        InvalidatePaint();
    }
    return Result<void>::Success();
}

void Button::SetHoveredColor(LinearColor color) noexcept {
    hoveredColor_ = color;
    hasHoveredColor_ = true;
    if (IsHovered()) {
        InvalidatePaint();
    }
}
bool Button::IsHovered() const noexcept {
    return hoverCount_ != 0;
}
void Button::OnMouseEnter(uint32) noexcept {
    if (++hoverCount_ == 1) {
        InvalidatePaint();
        onHovered_.ExecuteIfBound();
    }
}
void Button::OnMouseLeave(uint32 user) noexcept {
    if (pressed_ && userId_ == user && pointerSource_ == PointerSource::Mouse &&
        activeMethod_ == ButtonClickMethod::Precise) {
        ReleasePointer();
    }
    if (hoverCount_ && --hoverCount_ == 0) {
        InvalidatePaint();
        onUnhovered_.ExecuteIfBound();
    }
}
void Button::ReleasePointer() noexcept {
    pressed_ = false;
    InvalidatePaint();
    onReleased_.ExecuteIfBound();
}

void Button::SetPressedColor(LinearColor color) noexcept {
    if (pressedColor_ != color) {
        pressedColor_ = color;
        InvalidatePaint();
    }
}

bool Button::IsPressed() const noexcept {
    return pressed_ || keyPressed_;
}

bool Button::SupportsKeyboardFocus() const noexcept {
    return focusable_;
}

bool Button::OnKeyEvent(uint32 userId, const input::InputEvent& event) noexcept {
    if (!focusable_ || (event.key != input::Key::Enter && event.key != input::Key::Space)) {
        return false;
    }

    if (event.type == input::InputEventType::KeyDown) {
        if (!keyPressed_ && !event.repeat) {
            keyPressed_ = true;
            InvalidatePaint();
            keyUserId_ = userId;
            pressedKey_ = event.key;
            activePress_ = pressMethod_;
            onPressed_.ExecuteIfBound();
            if (keyPressed_ && activePress_ == ButtonPressMethod::Down) {
                Activate();
            }
        }
        return true;
    }

    if (event.type == input::InputEventType::KeyUp && keyPressed_ && keyUserId_ == userId &&
        pressedKey_ == event.key) {
        keyPressed_ = false;
        InvalidatePaint();
        const bool activate = activePress_ != ButtonPressMethod::Down;
        onReleased_.ExecuteIfBound();
        if (activate) {
            Activate();
        }
        return true;
    }

    if (event.type == input::InputEventType::KeyUp && !keyPressed_ &&
        pressMethod_ == ButtonPressMethod::Up) {
        Activate();
        return true;
    }
    return false;
}

void Button::OnFocusLost(uint32 userId) noexcept {
    OnKeyCancelled(userId, pressedKey_);
}

void Button::OnKeyCancelled(uint32 userId, input::Key key) noexcept {
    if (keyPressed_ && keyUserId_ == userId && key == pressedKey_) {
        keyPressed_ = false;
        InvalidatePaint();
        onReleased_.ExecuteIfBound();
    }
}

PointerReply Button::OnPointerEvent(const PointerEvent& event) noexcept {
    if (event.source == PointerSource::Mouse && event.button != input::MouseButton::Left &&
        (event.type == PointerEventType::Down || event.type == PointerEventType::Up)) {
        return {};
    }

    if (event.type == PointerEventType::Down && !pressed_) {
        pressed_ = true;
        InvalidatePaint();
        pointerSource_ = event.source;
        pointerId_ = event.pointerId;
        userId_ = event.userId;
        pressOrigin_ = event.position;
        activeMethod_ = event.source == PointerSource::Mouse ? clickMethod_ : touchMethod_;
        onPressed_.ExecuteIfBound();
        if (pressed_ && activeMethod_ == ButtonClickMethod::Down) {
            Activate();
        }
        return {.handled = true,
                .capture = pressed_ && (activeMethod_ != ButtonClickMethod::Precise ||
                                        event.source == PointerSource::Touch),
                .allowTouchPan = event.source == PointerSource::Touch};
    }

    if (!pressed_ && event.type == PointerEventType::Up && GetGeometry().Contains(event.position) &&
        (event.source == PointerSource::Mouse ? clickMethod_ : touchMethod_) ==
            ButtonClickMethod::Up) {
        Activate();
        return {.handled = true};
    }
    if (!pressed_ || pointerSource_ != event.source || pointerId_ != event.pointerId ||
        userId_ != event.userId) {
        return {};
    }

    if (event.type == PointerEventType::Move && activeMethod_ == ButtonClickMethod::Precise &&
        (event.position - pressOrigin_).Length() > 5.0f) {
        ReleasePointer();
        return {.handled = true, .releaseCapture = true};
    }
    if (event.type == PointerEventType::Up) {
        const bool activate =
            activeMethod_ != ButtonClickMethod::Down && GetGeometry().Contains(event.position);
        ReleasePointer();
        if (activate) {
            Activate();
        }
        return {.handled = true, .releaseCapture = true};
    }

    if (event.type == PointerEventType::Cancel) {
        ReleasePointer();
        return {.handled = true, .releaseCapture = true};
    }

    return {.handled = true};
}

void Button::OnPointerCaptureLost(PointerSource source, uint32 userId, uint32 pointerId) noexcept {
    if (pressed_ && pointerSource_ == source && userId_ == userId && pointerId_ == pointerId) {
        ReleasePointer();
    }
}

Result<void> Button::OnPaint(PaintList& list, int32 layer) const noexcept {
    const auto& brush = IsPressed() && pressedBrush_   ? *pressedBrush_
                        : IsHovered() && hoveredBrush_ ? *hoveredBrush_
                                                       : GetBrush();
    return list.TryAddBrush(GetGeometry(), brush, CurrentColor(), layer);
}

LinearColor Button::CurrentColor() const noexcept {
    return IsPressed()                       ? pressedColor_
           : IsHovered() && hasHoveredColor_ ? hoveredColor_
                                             : GetColor();
}

void Button::OnActivated() noexcept {
}

void Button::Activate() noexcept {
    OnActivated();
    onClicked_.ExecuteIfBound();
}

AccessibleRole Button::GetAccessibleRole() const noexcept {
    return AccessibleRole::Button;
}

} // namespace rocks::ui
