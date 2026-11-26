#pragma once

#include <UiFramework/Menus/MenuRow.h>
#include <UiFramework/Application/UiTimer.h>

namespace rocks::ui {

// 펼친 메뉴 하나다(UE SMultiBoxWidget 메뉴 — Docs/Design/UiToolkit 9.1절 UW7c). 항목(MenuRow)·
// 구분선·묶음 제목을 위에서 아래로 놓고 Dropdown 바탕과 1 px DropdownOutline 외곽선을 그린다.
// 허용 높이(팝업 층이 화면 안으로 줄인 높이)보다 길면 패널 안에서 스크롤한다: 휠, 키보드 이동 때
// 고른 항목이 보이게 맞춤, 오른쪽 얇은 위치 막대(R9).
// 글자를 누르면 그 글자로 시작하는 다음 항목으로 가고, 하나뿐이면 실행한다(Windows 메뉴).
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
    // 항목에서 포인터가 떠났다. 대기 중인 하위 메뉴 바꾸기를 취소한다(UE OnMouseLeave
    // CancelPendingSubMenu — 하위 메뉴로 가는 길에 형제 항목을 스쳐도 닫히지 않는다).
    void OnRowUnhovered(MenuRow& row) noexcept;
    // 지금 포인터가 올라간 항목이다(없으면 null).
    MenuRow* HoveredRow() const noexcept;
    // 지금 열린 하위 메뉴 항목이다(없으면 null).
    MenuRow* OpenSubMenu() const noexcept;
    // 열린 하위 메뉴를 next로 바꾼다(null이면 닫기만). 대기 중인 바꾸기는 취소한다.
    void SwitchSubMenu(MenuRow* next) noexcept;
    // 첫 사용 가능 항목에 키보드 초점(Navigation)을 둔다. 항목이 없으면 false다.
    bool FocusFirstItem(uint32 user) noexcept;

    // 패널 자체도 초점을 받는다 — 마우스로 연 메뉴는 패널에 초점을 두어 첫 방향키가 첫 항목으로
    // 간다(R1).
    bool SupportsKeyboardFocus() const noexcept override;

    // 스크롤 위치(위에서 내린 거리)와 최대값이다. 넘치지 않으면 둘 다 0이다.
    float ScrollOffset() const noexcept;
    float MaxScrollOffset() const noexcept;

    Vec2 ComputeDesiredSize(float) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;
    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;
    Result<void> OnPaintOverlay(PaintList& list, int32 layer) const noexcept override;
    bool OnKeyEvent(uint32 user, const input::InputEvent& event) noexcept override;

private:
    bool MoveFocus(uint32 user, int32 step, bool toEnd) noexcept;
    // 글자 점프(R10 — Windows): key 글자로 시작하는 다음 항목으로 초점을 옮기고, 그런 항목이
    // 하나뿐이면 실행한다. 맞는 항목이 없으면 false다.
    bool JumpToLetter(uint32 user, char letter) noexcept;
    // index 항목이 보이도록 스크롤을 맞춘다.
    void ScrollIntoView(usize index) noexcept;
    // 항목이 놓이는 위쪽 여백(외곽선 + 패널 여백)과 보이는 높이다.
    float ContentTop() const noexcept;
    float ViewHeight() const noexcept;

    SharedPtr<MenuContext> context_;
    MenuAnchor* owner_;
    MenuRow* openSub_ = nullptr;
    MenuRow* hoveredRow_ = nullptr;
    float scroll_ = 0;
    float maxScroll_ = 0;
    SharedPtr<UiTimer> clobber_;
};

// 다른 하위 메뉴가 열려 있을 때 hover한 항목으로 바꾸기까지의 지연(초)이다(UE SubMenuClobberTime).
inline constexpr double SubMenuClobberDelay = 0.5;
// 긴 메뉴 휠 한 칸의 스크롤 거리다(ScrollView와 같은 48 px).
inline constexpr float MenuWheelScrollStep = 48.0f;

} // namespace rocks::ui
