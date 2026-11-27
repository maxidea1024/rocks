#include <UiCore/InputMetrics.h>
#include <UiFramework/Controls/Button.h>

namespace rocks::ui {

Result<void> Button::Construct(Args&& args) noexcept {
    auto status = Frame::Construct({.content = args.content,
                                    .color = Move(args.color),
                                    .horizontalAlignment = args.horizontalAlignment,
                                    .verticalAlignment = args.verticalAlignment});
    if (status) {
        status = args.style ? SetStyle(*args.style) : ApplyStyle(style_);
    }

    if (status) {
        status = SetContentPadding(args.contentPadding);
    }

    // Frame::Construct가 여백을 Args 기본값(0)으로 덮으므로, 스타일·내용 여백이 이전과 같아
    // 위 setter가 건너뛰어도 스타일 여백을 다시 넣는다.
    if (status) {
        UpdatePadding();
    }

    if (status && args.foregroundColor) {
        status = SetForegroundColor(*args.foregroundColor);
    }

    if (status) {
        SetOnClicked(Move(args.onClicked));
        SetOnPressed(Move(args.onPressed));
        SetOnReleased(Move(args.onReleased));
        SetOnHovered(Move(args.onHovered));
        SetOnUnhovered(Move(args.onUnhovered));
        SetFocusable(args.focusable);
        status = SetInputMethods(args.clickMethod, args.touchMethod, args.pressMethod);
    }

    return status;
}

const StyleBrush* Button::FocusOutlineBrush() const noexcept {
    const ColorRef fill = style_.normal.fill;
    const bool accent = fill == ColorRef::FromTheme(ThemeSlot::Primary) ||
                        fill == ColorRef::FromTheme(ThemeSlot::PrimaryHover) ||
                        fill == ColorRef::FromTheme(ThemeSlot::PrimaryPress);
    return accent ? &FocusOutlineOnAccentStyle() : &FocusOutlineStyle();
}

Button::Button(Allocator& allocator) noexcept : Frame(allocator) {
    // UE SButton의 ForegroundColor 기본값 UseStyle이다.
    (void)SetForegroundColor(ColorRef{ColorSource::Style});
    UpdatePadding();
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

bool Button::IsValidStyle(const ButtonStyle& style) noexcept {
    const auto validPadding = [](Vec4 padding) noexcept {
        return padding.IsFinite() && padding.x >= 0 && padding.y >= 0 && padding.z >= 0 &&
               padding.w >= 0;
    };
    const ThemeColors& theme = DefaultThemeColors();
    const StyleBrush* brushes[] = {&style.normal, &style.hovered, &style.pressed, &style.disabled};
    for (const StyleBrush* brush : brushes) {
        if (!brush->fill.IsValid() || !brush->outlineColor.IsValid() ||
            !brush->Resolve(theme, {}).IsValid()) {
            return false;
        }
    }

    return validPadding(style.normalPadding) && validPadding(style.pressedPadding) &&
           style.normalForeground.IsValid() && style.hoveredForeground.IsValid() &&
           style.pressedForeground.IsValid() && style.disabledForeground.IsValid();
}

Result<void> Button::SetStyle(const ButtonStyle& style) noexcept {
    auto applied = ApplyStyle(style);
    if (applied) {
        styleExplicit_ = true;
    }

    return applied;
}

void Button::ApplyStyles(const UiHostServices& host) noexcept {
    if (!styleExplicit_) {
        // 활성 집합에 키가 없으면(등록부 없음 포함) 내장 기본값으로 돌린다.
        const ButtonStyle* style = host.FindStyle<ButtonStyle>("Button");
        (void)ApplyStyle(style ? *style : DefaultButtonStyle());
    }
}

Result<void> Button::ApplyStyle(const ButtonStyle& style) noexcept {
    if (!IsValidStyle(style)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (style_ != style) {
        style_ = style;
        InvalidatePaint();
        UpdatePadding();
    }

    return Result<void>::Success();
}

const ButtonStyle& Button::GetStyle() const noexcept {
    return style_;
}

Result<void> Button::SetContentPadding(Vec4 padding) noexcept {
    if (!padding.IsFinite() || padding.x < 0 || padding.y < 0 || padding.z < 0 || padding.w < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (contentPadding_ != padding) {
        contentPadding_ = padding;
        UpdatePadding();
    }

    return Result<void>::Success();
}

Vec4 Button::GetContentPadding() const noexcept {
    return contentPadding_;
}

void Button::UpdatePadding() noexcept {
    const Vec4 state = IsPressed() ? style_.pressedPadding : style_.normalPadding;
    (void)SetPadding(state + contentPadding_);
}

bool Button::IsHovered() const noexcept {
    return hoverCount_ != 0;
}

bool Button::IsHighlighted() const noexcept {
    return IsHovered();
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
    UpdatePadding();
    onReleased_.ExecuteIfBound();
}

bool Button::IsPressed() const noexcept {
    return pressed_ || keyPressed_;
}

bool Button::SupportsKeyboardFocus() const noexcept {
    return focusable_;
}

KeyReply Button::OnKeyEvent(const KeyEvent& key) noexcept {
    const uint32 userId = key.user;
    const input::InputEvent& event = key.input;
    // 확인 동작(Enter·Space·패드 확인)이 누름이다(UE SButton::OnKeyDown —
    // GetNavigationActionFromKey).
    if (!focusable_ || key.action != NavigationAction::Accept) {
        return Element::OnKeyEvent(key);
    }

    if (event.type == input::InputEventType::KeyDown) {
        if (!keyPressed_ && !event.repeat) {
            keyPressed_ = true;
            InvalidatePaint();
            UpdatePadding();
            keyUserId_ = userId;
            pressedKey_ = event.key;
            activePress_ = pressMethod_;
            onPressed_.ExecuteIfBound();
            if (keyPressed_ && activePress_ == ButtonPressMethod::Down) {
                Activate();
            }
        }

        return KeyReply::Handled();
    }

    if (event.type == input::InputEventType::KeyUp && keyPressed_ && keyUserId_ == userId &&
        pressedKey_ == event.key) {
        keyPressed_ = false;
        InvalidatePaint();
        UpdatePadding();
        const bool activate = activePress_ != ButtonPressMethod::Down;
        onReleased_.ExecuteIfBound();
        if (activate) {
            Activate();
        }

        return KeyReply::Handled();
    }

    if (event.type == input::InputEventType::KeyUp && !keyPressed_ &&
        pressMethod_ == ButtonPressMethod::Up) {
        Activate();
        return KeyReply::Handled();
    }

    return Element::OnKeyEvent(key);
}

void Button::OnFocusLost(uint32 userId) noexcept {
    OnKeyCancelled(userId, pressedKey_);
}

void Button::OnKeyCancelled(uint32 userId, input::Key key) noexcept {
    if (keyPressed_ && keyUserId_ == userId && key == pressedKey_) {
        keyPressed_ = false;
        InvalidatePaint();
        UpdatePadding();
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
        UpdatePadding();
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
        (event.position - pressOrigin_).Length() > PointerDragThreshold) {
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
    const StyleBrush& style = CurrentStyleBrush(!IsEnabled() || list.IsDisabled());
    if (style.mode == BrushMode::None) {
        return Result<void>::Success();
    }

    // brush 색은 테마로 풀고 ButtonColorAndOpacity를 곱한다(UE SButton::OnPaint).
    const auto draw = [&]() noexcept {
        return list.TryAddBrush(GetGeometry(), style.Resolve(list.Theme(), list.ColorContext()),
                                CurrentColor(), layer);
    };
    if (&style == &style_.disabled) {
        // 비활성 전용 brush는 비활성 효과 없이 그린다(UE bShowDisabledEffect — 자식에는 효과가
        // 간다).
        PaintList::DisabledEffectExemption exemption(list);
        return draw();
    }

    return draw();
}

LinearColor Button::CurrentColor() const noexcept {
    return GetColor();
}

const StyleBrush& Button::CurrentStyleBrush(bool disabled) const noexcept {
    // UE는 비활성 brush가 있으면 그것을, 없으면 보통 상태 brush에 비활성 효과를 쓴다.
    if (disabled && style_.disabled.mode != BrushMode::None) {
        return style_.disabled;
    }

    return IsPressed() ? style_.pressed : IsHighlighted() ? style_.hovered : style_.normal;
}

ColorRef Button::PaintForegroundColor(bool disabled) const noexcept {
    // UE SBorder는 비활성이면 DisabledForegroundColor(버튼 스타일의 disabledForeground)를 쓴다.
    return disabled ? style_.disabledForeground : GetForegroundColor();
}

ColorRef Button::GetForegroundColor() const noexcept {
    const ColorRef assigned = Element::GetForegroundColor();
    if (!IsEnabled()) {
        return style_.disabledForeground;
    }

    if (assigned.source != ColorSource::Style) {
        return assigned;
    }

    return IsPressed()       ? style_.pressedForeground
           : IsHighlighted() ? style_.hoveredForeground
                             : style_.normalForeground;
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
