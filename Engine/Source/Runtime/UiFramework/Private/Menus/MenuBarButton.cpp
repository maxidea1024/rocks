#include <UiFramework/Menus/MenuBar.h>
#include <UiFramework/Menus/MenuBarButton.h>

namespace rocks::ui {

MenuBarButton::MenuBarButton(Allocator& allocator, MenuBar& bar, const MenuBarStyle& style) noexcept
    : MenuAnchor(allocator), bar_(bar), style_(style) {
    // 기본 막대 스타일 값은 늘 유효하다.
    (void)SetStyle(style.button);
    SetPlacement(MenuPlacement::Below);
    // 누르는 순간 연다(Windows·UE 메뉴 막대 — 눌림 색이 들어갔다 빠지는 깜빡임이 없다). 열린 메뉴의
    // 층은 창을 덮지 않아 막대 hover로 옆 메뉴로 넘어갈 수 있고, 바깥 누름은 막대의 누름 관찰자가
    // 닫는다.
    // 이 조합은 늘 유효하다(열거 범위 안).
    (void)SetInputMethods(ButtonClickMethod::Down, ButtonClickMethod::Down,
                          ButtonPressMethod::DownAndUp);
    SetCatchOutsidePresses(false);
    // 막대 높이가 글자보다 높아도 글자는 세로 가운데다.
    SetContentAlignment(StackAlignment::Fill, StackAlignment::Center);
}

bool MenuBarButton::IsHighlighted() const noexcept {
    // 키보드 초점(F10·Alt)은 외곽선 대신 올림 바탕으로 보인다(Windows 메뉴 막대).
    return MenuAnchor::IsHighlighted() || bar_.HasKeyboardFocus(*this);
}

PointerReply MenuBarButton::OnPointerEvent(const PointerEvent& event) noexcept {
    // 누른 채 끌어 메뉴 항목에서 놓으면 그 항목을 실행한다(R8 — Windows·macOS). 버튼이 누름을
    // 잡고 있으므로 놓음은 여기로 온다. 먼저 버튼이 잡음을 풀게 한다.
    const bool release = event.type == PointerEventType::Up && IsPressed() &&
                         !GetGeometry().Contains(event.position);
    const PointerReply reply = MenuAnchor::OnPointerEvent(event);
    if (release && bar_.ReleaseOverMenu(event.position)) {
        return {.handled = true};
    }

    return reply;
}

bool MenuBarButton::OnKeyEvent(uint32 user, const input::InputEvent& event) noexcept {
    if (event.type == input::InputEventType::KeyDown && !IsOpen()) {
        switch (event.key) {
        case input::Key::Down:
            OnActivated();
            return true;
        case input::Key::Left:
            bar_.FocusNeighbor(*this, -1);
            return true;
        case input::Key::Right:
            bar_.FocusNeighbor(*this, 1);
            return true;
        case input::Key::Escape:
            bar_.LeaveMenuMode();
            return true;
        default:
            break;
        }
    }

    return MenuAnchor::OnKeyEvent(user, event);
}

void MenuBarButton::OnFocusReceived(uint32 user) noexcept {
    MenuAnchor::OnFocusReceived(user);
    InvalidatePaint();
}

void MenuBarButton::OnFocusLost(uint32 user) noexcept {
    MenuAnchor::OnFocusLost(user);
    InvalidatePaint();
}

Optional<bool> MenuBarButton::OnQueryShowFocus(FocusCause) const noexcept {
    return false;
}

void MenuBarButton::OnMouseEnter(uint32 user) noexcept {
    MenuAnchor::OnMouseEnter(user);
    bar_.OnButtonHovered(*this);
}

Result<void> MenuBarButton::OnPaint(PaintList& list, int32 layer) const noexcept {
    // 메뉴가 열린 버튼은 Primary 바탕이다(UE "WindowMenuBar.Button.SubMenuOpen"). 강조는 글자
    // 높이(버튼 여백 포함)만큼 세로 가운데에 둥근 상자로 그린다 — 막대가 높아도 띠처럼 보이지
    // 않는다.
    const bool disabled = !IsEnabled() || list.IsDisabled();
    const StyleBrush& brush =
        IsOpen() && !disabled ? style_.openBrush : CurrentStyleBrush(disabled);
    if (brush.mode == BrushMode::None) {
        return Result<void>::Success();
    }

    const Geometry& whole = GetGeometry();
    const float height = math::Min(whole.size.y, GetDesiredSize().y);
    const Geometry band = whole.MakeChild({whole.size.x, height},
                                          {1, 0, 0, 1, 0, (whole.size.y - height) * 0.5f}, {}, {});
    return list.TryAddBrush(band, brush.Resolve(list.Theme(), list.ColorContext()), CurrentColor(),
                            layer);
}

void MenuBarButton::OnActivated() noexcept {
    bar_.OnButtonActivated(*this);
}

} // namespace rocks::ui
