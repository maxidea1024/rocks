#pragma once

#include <UiCore/Styles/MenuBarStyle.h>
#include <UiFramework/Popups/MenuAnchor.h>

namespace rocks::ui {

class MenuBar;

// 메뉴 막대의 버튼 하나다(UE SMenuEntryBlock 막대 모드 — "WindowMenuBar.Button"). 누르는 순간
// 아래로 메뉴를 열고(열린 메뉴를 다시 누르면 닫는다), 열린 동안 openBrush(Primary) 바탕으로 그린다.
// 다른 막대 메뉴가 열린 동안 올리면 막대가 이 메뉴로 바꾼다(Docs/Design/UiToolkit 9.1절 UW7c).
class ROCKS_UIFRAMEWORK_API MenuBarButton final : public MenuAnchor {
public:
    MenuBarButton(Allocator& allocator, MenuBar& bar,
                  const MenuBarStyle& style = DefaultMenuBarStyle()) noexcept;

    void OnMouseEnter(uint32 user) noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

protected:
    void OnActivated() noexcept override;

private:
    MenuBar& bar_;
    MenuBarStyle style_;
};

} // namespace rocks::ui
