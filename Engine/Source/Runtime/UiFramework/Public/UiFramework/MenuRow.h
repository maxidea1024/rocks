#pragma once

#include <UiFramework/MenuAnchor.h>
#include <UiFramework/MenuContext.h>

namespace rocks::ui {

class MenuPanel;

// 메뉴 항목의 체크 표시 종류다(UE EUserInterfaceActionType ToggleButton·RadioButton).
enum class MenuCheckKind : uint8 {
    None,
    Check,
    Radio
};

// 펼친 메뉴의 한 항목이다(UE SMenuEntryBlock — Docs/Design/UiToolkit 9.1절 UW7c). 한 줄은
// [아이콘·체크 칸][라벨][단축키][하위 메뉴 표시]이고 올림·눌림은 Primary 바탕이다. 하위 메뉴가
// 있으면 오른쪽에 하위 메뉴를 열고(MenuAnchor), 없으면 누름·Enter가 동작을 실행한 뒤 메뉴 전체를
// 닫는다. canExecute가 거짓이면 비활성으로 그린다(열 때 평가한다). 체크·라디오는 그릴 때
// isChecked를 묻는다.
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
    };

    MenuRow(Allocator& allocator, const SharedPtr<MenuContext>& context) noexcept;
    Result<void> Construct(Args&& args) noexcept;

    bool HasSubMenu() const noexcept;
    bool IsChecked() const noexcept;
    MenuCheckKind CheckKind() const noexcept;
    // 하위 메뉴가 열린 동안에는 포인터가 하위 메뉴로 옮겨 가도 강조를 유지한다(열린 경로의 단계마다
    // 강조 — UE·Windows 메뉴).
    bool IsHighlighted() const noexcept override;
    // 메뉴 패널이 hover·키보드 처리를 위해 연결한다.
    void SetPanel(MenuPanel* panel) noexcept;

    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    Vec2 ComputeDesiredSize(float) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;
    Result<void> OnPaintOverlay(PaintList& list, int32 layer) const noexcept override;
    void OnMouseEnter(uint32 user) noexcept override;
    bool OnKeyEvent(uint32 user, const input::InputEvent& event) noexcept override;

protected:
    void OnActivated() noexcept override;

private:
    float IconColumnWidth() const noexcept;
    float TrailingWidth() const noexcept;
    float ContentOffsetY() const noexcept;

    SharedPtr<MenuContext> context_;
    SharedPtr<TextLabel> label_;
    SharedPtr<TextLabel> keybinding_;
    MenuCheckKind check_ = MenuCheckKind::None;
    Delegate<bool()> isChecked_;
    Delegate<void()> execute_;
    bool hasSubMenu_ = false;
    MenuPanel* panel_ = nullptr;
};

} // namespace rocks::ui
