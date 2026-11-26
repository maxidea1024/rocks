#pragma once

#include <UiFramework/Menus/MenuRow.h>
#include <UiFramework/Application/UiTimer.h>

namespace rocks::ui {

// 펼친 메뉴 하나다(UE SMultiBoxWidget 메뉴 — Docs/Design/UiToolkit 9.1절 UW7c). 항목(MenuRow)·
// 구분선·묶음 제목을 위에서 아래로 놓고 Dropdown 바탕과 1 px DropdownOutline 외곽선을 그린다.
// 키보드: 위·아래·Home·End는 사용 가능한 항목 사이 이동(끝에서 돌아감), 오른쪽은 하위 메뉴
// 열기(항목이 처리) 또는 루트 메뉴면 막대의 다음 메뉴, 왼쪽은 하위 메뉴면 닫기·루트면 막대의 이전
// 메뉴, Esc는 팝업 층이 한 단계 닫는다. 하위 메뉴는 hover 즉시 열고, 다른 하위 메뉴가 열려 있으면
// SubMenuClobberDelay 뒤 바꾼다(UE SubMenuOpenTime 0·SubMenuClobberTime 0.5 — MultiBox.h 34·39행).
class ROCKS_UIFRAMEWORK_API MenuPanel final : public Element {
public:
    // owner는 이 메뉴를 연 anchor다(루트면 막대 버튼).
    MenuPanel(Allocator& allocator, const SharedPtr<MenuContext>& context,
              MenuAnchor* owner) noexcept;

    // 항목·구분선·묶음 제목을 끝에 더한다. MenuRow는 이 패널에 연결된다.
    Result<void> AddItem(const SharedPtr<Element>& item) noexcept;
    bool IsRoot() const noexcept;
    MenuAnchor* Owner() const noexcept;

    // 항목 hover(MenuRow가 알린다)다. 하위 메뉴 열기·바꾸기·닫기를 정한다.
    void OnRowHovered(MenuRow& row) noexcept;
    // 지금 열린 하위 메뉴 항목이다(없으면 null).
    MenuRow* OpenSubMenu() const noexcept;

    Vec2 ComputeDesiredSize(float) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;
    Result<void> OnPaintOverlay(PaintList& list, int32 layer) const noexcept override;
    bool OnKeyEvent(uint32 user, const input::InputEvent& event) noexcept override;

private:
    void SwitchSubMenu(MenuRow* next) noexcept;
    bool MoveFocus(uint32 user, int32 step, bool toEnd) noexcept;

    SharedPtr<MenuContext> context_;
    MenuAnchor* owner_;
    MenuRow* openSub_ = nullptr;
    SharedPtr<UiTimer> clobber_;
};

// 다른 하위 메뉴가 열려 있을 때 hover한 항목으로 바꾸기까지의 지연(초)이다(UE SubMenuClobberTime).
inline constexpr double SubMenuClobberDelay = 0.5;

} // namespace rocks::ui
