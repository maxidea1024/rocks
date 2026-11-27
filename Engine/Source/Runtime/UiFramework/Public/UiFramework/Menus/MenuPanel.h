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
    ROCKS_UI_ELEMENT(MenuPanel, Element)

public:
    // owner는 이 메뉴를 연 anchor다(루트면 막대 버튼).
    MenuPanel(Allocator& allocator, const SharedPtr<MenuContext>& context,
              MenuAnchor* owner) noexcept;

    // 구분선·묶음 제목 같은 항목 아닌 요소를 끝에 더한다.
    Result<void> AddItem(const SharedPtr<Element>& item) noexcept;
    // 항목을 끝에 더하고 이 패널에 연결한다. 항목 판정(글자 점프·끌어 놓기)은 이렇게 더한 항목만
    // 본다(초점 받는 다른 요소를 항목으로 오인하지 않는다 — R16).
    Result<void> AddRow(const SharedPtr<MenuRow>& row) noexcept;
    // element가 이 패널의 항목이면 그 항목이다(아니면 null).
    MenuRow* FindRow(const Element* element) const noexcept;
    // 키보드로 옮긴 뒤 포인터가 다시 움직이기 전까지 참이다 — 그동안 hover 강조를 끈다(Windows —
    // 강조 하나). 포인터가 움직이면 항목·패널이 OnPointerMoved로 푼다.
    bool IsKeyboardNavigating() const noexcept;
    void OnPointerMoved() noexcept;
    // 패널 자체에 초점을 둔다(Pointer — 강조 없음). 항목을 떠날 때 쓴다.
    void FocusPanel(uint32 user) noexcept;
    bool IsRoot() const noexcept;
    MenuAnchor* Owner() const noexcept;

    // 항목 hover(MenuRow가 알린다)다. 하위 메뉴 열기·바꾸기·닫기를 정한다.
    void OnRowHovered(MenuRow& row) noexcept;
    // 항목에서 포인터가 떠났다. 대기 중인 하위 메뉴 바꾸기를 취소한다(UE OnMouseLeave
    // CancelPendingSubMenu — 하위 메뉴로 가는 길에 형제 항목을 스쳐도 닫히지 않는다).
    void OnRowUnhovered(MenuRow& row) noexcept;
    // 지금 포인터가 올라간 항목이다(없으면 null).
    MenuRow* HoveredRow() const noexcept;
    // 포인터가 패널 위(항목·구분선·여백 어디든)에 있는지다(UE OwnerMultiBoxWidget->IsHovered —
    // 열린 하위 메뉴 항목은 이것이 거짓일 때만 강조한다, Docs/Design/UiToolkit 19.5절).
    bool IsPointerInside() const noexcept;
    void OnMouseEnter(uint32 user) noexcept override;
    void OnMouseLeave(uint32 user) noexcept override;
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
    KeyReply OnKeyEvent(const KeyEvent& key) noexcept override;
    NavigationBoundary OnNavigation(uint32 user, FocusDirection direction) noexcept override;

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
    // 패널 위에 포인터를 둔 사용자 수다(hover 경로의 진입·이탈 — 조상도 받는다).
    uint32 pointerUsers_ = 0;
    Array<MenuRow*> rows_;
    float scroll_ = 0;
    float maxScroll_ = 0;
    // 배치 전에 들어온 보이게 맞춤 요청이다(배치에서 적용한다).
    usize pendingScroll_ = IndexNone;
    bool arranged_ = false;
    bool keyboardNavigating_ = false;
    // 키보드 이동을 표시하고 항목 강조를 다시 그린다.
    void SetKeyboardNavigating(bool navigating) noexcept;
    // index 항목이 보이게 하는 스크롤 위치다.
    float ScrollTarget(usize index) const noexcept;
    SharedPtr<UiTimer> clobber_;
};

// 다른 하위 메뉴가 열려 있을 때 hover한 항목으로 바꾸기까지의 지연(초)이다(UE SubMenuClobberTime).
inline constexpr double SubMenuClobberDelay = 0.5;
// 긴 메뉴 휠 한 칸의 스크롤 거리다(ScrollView와 같은 48 px).
inline constexpr float MenuWheelScrollStep = 48.0f;

} // namespace rocks::ui
