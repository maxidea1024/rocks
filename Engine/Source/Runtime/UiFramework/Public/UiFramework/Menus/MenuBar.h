#pragma once

#include <UiFramework/Application/Commands.h>
#include <UiFramework/Menus/MenuBarButton.h>
#include <UiFramework/Menus/MenuBuilder.h>

namespace rocks::ui {

// 창 주 메뉴 막대다(UE FMenuBarBuilder·SMultiBoxWidget 막대 — Docs/Design/UiToolkit 9.1절 UW7c).
// 직접 그린 창의 제목 막대 왼쪽(WindowTitleBar::Args::leftContent)이나 내용 위 줄에 둔다.
// - 누르는 순간 메뉴를 열고, 열린 동안 다른 막대 버튼에 올리면 그 메뉴로 바꾼다. 버튼을 누른 채
//   끌어 메뉴 항목에서 놓으면 그 항목을 실행한다(R8).
// - 메뉴 세션(R1): 열기 직전 초점을 기억했다가 메뉴가 모두 닫히면 되돌린다. 마우스로 열면 패널에,
//   키보드로 열면 첫 항목에 초점을 둔다.
// - 가속키(RegisterAccelerators — R4): F10은 막대 초점 켜고 끄기, Alt+글자는 그 글자로 시작하는
//   메뉴를 연다. 막대가 자기 명령 목록을 창 단위 목록으로 붙였다가 소멸할 때 뗀다.
// - 막대 초점에서 아래·Enter는 메뉴 열기, 왼쪽·오른쪽은 옆 버튼, Esc는 막대에서 나가기다.
// - 열린 메뉴 경로(막대·펼친 메뉴·하위 메뉴) 밖을 누르면 Application 누름 관찰자로 메뉴 전체를
//   닫고 그 누름을 삼킨다(UE 메뉴 스택 — 층이 창을 덮지 않으므로 막대 hover가 계속 닿는다).
// - 받은 폭에 다 들어가지 않으면 뒤 메뉴들을 숨기고 끝에 넘침 버튼(»)을 둔다. 넘침 메뉴는 숨은
//   메뉴들을 하위 메뉴로 보인다(R12).
//
// 사용 예:
//   auto bar = SharedPtr<MenuBar>::TryMake(allocator, allocator, context);
//   bar.Value()->AddMenu("File", MenuFill::CreateLambda([&](MenuBuilder& menu) noexcept {
//       menu.AddCommand(NewId, definitions, commands, bindings);
//       return Result<void>::Success();
//   }));
//   bar.Value()->RegisterAccelerators();
class ROCKS_UIFRAMEWORK_API MenuBar final : public Element {
public:
    MenuBar(Allocator& allocator, const SharedPtr<MenuContext>& context,
            const MenuBarStyle& style = DefaultMenuBarStyle()) noexcept;
    ~MenuBar() noexcept override;

    // 막대 끝에 메뉴 하나를 더한다(label은 막대 글자, fill은 열 때마다 메뉴를 채운다).
    Result<void> AddMenu(StringView label, MenuFill fill) noexcept;
    // 메뉴 수다(넘침 버튼은 세지 않는다).
    usize MenuCount() const noexcept;
    MenuBarButton* Button(usize index) const noexcept;
    // 넘침 버튼이다(메뉴가 하나도 없으면 null). 다 들어가면 접혀 있다.
    MenuBarButton* OverflowButton() const noexcept;
    // index 메뉴가 넘쳐 숨었는지다.
    bool IsMenuHidden(usize index) const noexcept;
    // 열린 막대 메뉴의 번호다. 없으면 IndexNone이다.
    usize OpenIndex() const noexcept;
    // index 메뉴를 연다. keyboard면 첫 항목에 키보드 초점, 아니면 패널에 초점(외곽선 없음)을 둔다.
    // 다른 메뉴는 닫는다. 메뉴 세션을 시작한다.
    Result<void> OpenMenu(usize index, bool keyboard) noexcept;
    // F10·Alt+글자 가속키를 이 창의 명령으로 붙인다(두 번 부르면 AlreadyExists).
    Result<void> RegisterAccelerators() noexcept;
    // 막대 초점 상태(F10)에서 나간다 — 메뉴를 닫고 기억한 초점으로 되돌린다.
    void LeaveMenuMode() noexcept;
    // 막대 초점을 옆 버튼으로 옮긴다(키보드).
    void FocusNeighbor(const MenuBarButton& from, int32 step) noexcept;
    // button이 키보드 초점(Navigation)을 들고 있는지다(막대 버튼 강조).
    bool HasKeyboardFocus(const MenuBarButton& button) const noexcept;

    // 막대 버튼이 알린다.
    void OnButtonHovered(MenuBarButton& button) noexcept;
    void OnButtonActivated(MenuBarButton& button) noexcept;
    // 막대 버튼을 누른 채 끌다 놓은 지점이 열린 메뉴 항목이면 그 항목을 실행한다(R8). 처리했으면
    // true다.
    bool ReleaseOverMenu(Vec2 windowPoint) noexcept;

    Vec2 ComputeDesiredSize(float) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;

private:
    usize IndexOf(const MenuBarButton& button) const noexcept;
    // 넘침 버튼을 만든다(첫 AddMenu에서).
    Result<void> EnsureOverflow() noexcept;
    // 넘침 메뉴를 채운다: 숨은 메뉴마다 하위 메뉴 하나.
    Result<void> FillOverflow(MenuBuilder& menu) const noexcept;
    bool Navigate(int32 step) noexcept;
    // 누름이 열린 메뉴 경로 밖이면 메뉴 전체를 닫고 true(누름 소비)다.
    bool CloseIfOutside(const Element* hit) noexcept;
    // 막대 메뉴가 닫혔다. 바꾸는 중이 아니고 열린 메뉴가 없으면 세션을 끝낸다(초점 복원).
    void OnMenuOpenChanged(bool open) noexcept;
    // F10이다. 막대가 초점을 들고 메뉴가 닫혀 있으면 나가고, 아니면 첫 버튼에 초점을 둔다.
    void ToggleMenuMode() noexcept;

    Allocator& allocator_;
    SharedPtr<MenuContext> context_;
    MenuBarStyle style_;
    // 메뉴 버튼들이고, 메뉴가 있으면 마지막이 넘침 버튼이다.
    Array<SharedPtr<MenuBarButton>> buttons_;
    Array<String> labels_;
    Array<SharedPtr<MenuFill>> fills_;
    WeakPtr<Application*> app_;
    uint64 pressObserver_ = 0;
    // 가속키는 막대가 소유한 정의·목록이다(창 단위 명령으로 붙인다).
    CommandSet acceleratorSet_;
    BindingManager acceleratorBindings_;
    SharedPtr<CommandList> accelerators_;
    bool switching_ = false;
};

} // namespace rocks::ui
