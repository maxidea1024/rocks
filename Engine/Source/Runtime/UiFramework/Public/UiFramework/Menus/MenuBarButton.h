#pragma once

#include <UiCore/Styles/MenuBarStyle.h>
#include <UiFramework/Popups/MenuAnchor.h>

namespace rocks::ui {

class MenuBar;

// 메뉴 막대의 버튼 하나다(UE SMenuEntryBlock 막대 모드 — "WindowMenuBar.Button"). 누르는 순간
// 아래로 메뉴를 열고(열린 메뉴를 다시 누르면 닫는다), 열린 동안 openBrush(Primary) 바탕으로
// 그린다. 다른 막대 메뉴가 열린 동안 올리면 막대가 이 메뉴로 바꾼다(Docs/Design/UiToolkit 9.1절
// UW7c). 누른 채 끌어 메뉴 항목에서 놓으면 그 항목을 실행한다(R8). 키보드 초점(F10)은 외곽선 대신
// 올림 바탕으로 보이고, 아래·Enter는 메뉴 열기, 왼쪽·오른쪽은 옆 버튼, Esc는 막대에서 나가기다.
class ROCKS_UIFRAMEWORK_API MenuBarButton final : public MenuAnchor {
    ROCKS_UI_ELEMENT(MenuBarButton, MenuAnchor)

public:
    MenuBarButton(Allocator& allocator, MenuBar& bar,
                  const MenuBarStyle& style = DefaultMenuBarStyle()) noexcept;

    bool IsHighlighted() const noexcept override;
    void OnMouseEnter(uint32 user) noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;
    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override;
    KeyReply OnKeyEvent(const KeyEvent& key) noexcept override;
    void OnFocusReceived(uint32 user) noexcept override;
    void OnFocusLost(uint32 user) noexcept override;
    Optional<bool> OnQueryShowFocus(FocusCause cause) const noexcept override;

protected:
    void OnActivated() noexcept override;

private:
    MenuBar& bar_;
    MenuBarStyle style_;
};

} // namespace rocks::ui
