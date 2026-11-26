#pragma once

#include <UiFramework/Menus/MenuAction.h>
#include <UiFramework/Menus/MenuContext.h>
#include <UiFramework/Popups/MenuAnchor.h>

namespace rocks::ui {

class MenuPanel;

// 펼친 메뉴의 한 항목이다(UE SMenuEntryBlock — Docs/Design/UiToolkit 9.1절 UW7c). 한 줄은
// [아이콘·체크 칸][라벨][단축키][하위 메뉴 표시]이고 강조는 Primary 둥근 바탕이다. 하위 메뉴가
// 있으면 오른쪽에 하위 메뉴를 열고(MenuAnchor), 없으면 누름·Enter가 동작을 실행한 뒤 메뉴 전체를
// 닫는다. canExecute가 거짓이면 비활성으로 그린다(열 때 평가한다). 체크·라디오는 그릴 때
// isChecked를 묻는다.
//
// 강조 하나(R2): 포인터를 올리면 초점이 이 항목으로 오고(Pointer), 강조는 hover·키보드 초점·열린
// 하위 메뉴(패널에서 다른 항목을 hover하지 않는 동안 — UE ShouldSubMenuAppearHovered) 중 하나다.
// 초점은 외곽선 대신 이 강조로 보인다(Windows 메뉴).
class ROCKS_UIFRAMEWORK_API MenuRow final : public MenuAnchor {
public:
    struct Args {
        SharedPtr<TextLabel> label{};
        // 단축키 글자(Small)다. 없으면 칸을 비운다.
        SharedPtr<TextLabel> keybinding{};
        MenuCheckKind check = MenuCheckKind::None;
        Delegate<bool()> isChecked{};
        Delegate<bool()> canExecute{};
        Delegate<void()> execute{};
        // 있으면 하위 메뉴 항목이다(그 메뉴를 만드는 factory).
        MenuFactory subMenu{};
        // 아이콘 칸 그림(체크 표시가 없을 때)이다.
        VectorImageKey icon{};
        // 항목에 머무르면 보이는 설명이다(명령 설명). 문맥의 레이블 factory로 글자 툴팁을 만든다.
        StringView toolTip{};
    };

    MenuRow(Allocator& allocator, const SharedPtr<MenuContext>& context) noexcept;
    Result<void> Construct(Args&& args) noexcept;

    bool HasSubMenu() const noexcept;
    bool IsChecked() const noexcept;
    MenuCheckKind CheckKind() const noexcept;
    StringView LabelText() const noexcept;
    VectorImageKey Icon() const noexcept;
    bool IsHighlighted() const noexcept override;
    // 메뉴 패널이 hover·키보드 처리를 위해 연결한다.
    void SetPanel(MenuPanel* panel) noexcept;
    // 누름·Enter와 같다(하위 메뉴면 열고, 아니면 메뉴 전체를 닫고 실행). 막대 버튼을 누른 채
    // 끌어 이 항목에서 놓을 때(R8)·글자 점프(R10)가 부른다.
    void Invoke() noexcept;
    // 하위 메뉴를 열고 그 첫 항목에 키보드 초점을 둔다(오른쪽·Enter).
    Result<void> OpenSubMenuFocused(uint32 user) noexcept;

    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    Vec2 ComputeDesiredSize(float) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;
    Result<void> OnPaintOverlay(PaintList& list, int32 layer) const noexcept override;
    void OnMouseEnter(uint32 user) noexcept override;
    void OnMouseLeave(uint32 user) noexcept override;
    bool OnKeyEvent(uint32 user, const input::InputEvent& event) noexcept override;
    void OnFocusReceived(uint32 user) noexcept override;
    void OnFocusLost(uint32 user) noexcept override;
    Optional<bool> OnQueryShowFocus(FocusCause cause) const noexcept override;

protected:
    void OnActivated() noexcept override;

private:
    float IconColumnWidth() const noexcept;
    float TrailingWidth() const noexcept;
    float ContentOffsetY() const noexcept;
    // 하위 메뉴가 닫힐 때 초점이 그 안에 있었으면 이 항목으로 되돌린다.
    void OnSubMenuOpenChanged(bool open) noexcept;

    SharedPtr<MenuContext> context_;
    SharedPtr<TextLabel> label_;
    SharedPtr<TextLabel> keybinding_;
    MenuCheckKind check_ = MenuCheckKind::None;
    Delegate<bool()> isChecked_;
    Delegate<void()> execute_;
    VectorImageKey icon_{};
    String toolTip_;
    bool hasSubMenu_ = false;
    MenuPanel* panel_ = nullptr;
};

} // namespace rocks::ui
