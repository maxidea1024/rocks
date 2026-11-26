#include <UiCore/Styles/CoreStyles.h>
#include <UiFramework/WindowControlButton.h>

namespace rocks::ui {

WindowControlButton::WindowControlButton(Allocator& allocator, WindowButtonRole role,
                                         const WindowStyle& style) noexcept
    : Button(allocator), style_(style), role_(role) {
    (void)SetStyle(role == WindowButtonRole::Close ? style.closeButton : style.controlButton);
    SetDesiredSize(style.buttonSize);
    SetFocusable(false);
}

WindowButtonRole WindowControlButton::Role() const noexcept {
    return role_;
}

void WindowControlButton::SetMaximized(bool maximized) noexcept {
    if (maximized_ != maximized) {
        maximized_ = maximized;
        InvalidatePaint();
    }
}

bool WindowControlButton::IsMaximized() const noexcept {
    return maximized_;
}

void WindowControlButton::SetFullscreen(bool fullscreen) noexcept {
    if (fullscreen_ != fullscreen) {
        fullscreen_ = fullscreen;
        InvalidatePaint();
    }
}

bool WindowControlButton::IsFullscreen() const noexcept {
    return fullscreen_;
}

input::WindowRegion WindowControlButton::GetWindowRegion() const noexcept {
    switch (role_) {
    case WindowButtonRole::Minimize:
        return input::WindowRegion::MinimizeButton;
    case WindowButtonRole::Maximize:
        return input::WindowRegion::MaximizeButton;
    case WindowButtonRole::Close:
        return input::WindowRegion::CloseButton;
    }

    return input::WindowRegion::Client;
}

VectorImageKey WindowControlButton::CurrentIcon() const noexcept {
    switch (role_) {
    case WindowButtonRole::Minimize:
        return style_.minimizeIcon;
    case WindowButtonRole::Maximize:
        return fullscreen_  ? style_.exitFullscreenIcon
               : maximized_ ? style_.restoreIcon
                            : style_.maximizeIcon;
    case WindowButtonRole::Close:
        return style_.closeIcon;
    }

    return style_.closeIcon;
}

Result<void> WindowControlButton::OnPaint(PaintList& list, int32 layer) const noexcept {
    auto painted = Button::OnPaint(list, layer);
    if (!painted) {
        return painted;
    }

    // 아이콘은 버튼 판 크기 그대로 가운데에 놓고 버튼 전경색(올림 ForegroundHover)으로 tint한다.
    const auto& geometry = GetGeometry();
    const Vec2 size = style_.buttonSize;
    const Vec2 offset = (geometry.size - size) * 0.5f;
    const bool disabled = !IsEnabled() || list.IsDisabled();
    const LinearColor ink =
        PaintForegroundColor(disabled).Resolve(list.Theme(), list.ColorContext());
    const auto icon = geometry.MakeChild(size, {1, 0, 0, 1, offset.x, offset.y}, {}, {});
    return list.TryAddBrush(icon, Brush::Vector(CurrentIcon(), size), ink, layer);
}

} // namespace rocks::ui
