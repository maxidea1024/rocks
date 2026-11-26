#include <UiFramework/MenuBar.h>
#include <UiFramework/MenuBarButton.h>

namespace rocks::ui {

MenuBarButton::MenuBarButton(Allocator& allocator, MenuBar& bar, const MenuBarStyle& style) noexcept
    : MenuAnchor(allocator), bar_(bar), style_(style) {
    (void)SetStyle(style.button);
    SetPlacement(MenuPlacement::Below);
    // 누르는 순간 연다(Windows·UE 메뉴 막대 — 눌림 색이 들어갔다 빠지는 깜빡임이 없다). 열린 메뉴의
    // 층은 창을 덮지 않아 막대 hover로 옆 메뉴로 넘어갈 수 있고, 바깥 누름은 막대의 누름 관찰자가
    // 닫는다.
    (void)SetInputMethods(ButtonClickMethod::Down, ButtonClickMethod::Down,
                          ButtonPressMethod::DownAndUp);
    SetCatchOutsidePresses(false);
    // 막대 높이가 글자보다 높아도 글자는 세로 가운데다.
    SetContentAlignment(StackAlignment::Fill, StackAlignment::Center);
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
