#pragma once

#include <UiFramework/Menus/MenuBarButton.h>
#include <UiFramework/Menus/MenuBuilder.h>

namespace rocks::ui {

// 창 주 메뉴 막대다(UE FMenuBarBuilder·SMultiBoxWidget 막대 — Docs/Design/UiToolkit 9.1절 UW7c).
// 직접 그린 창의 제목 막대 왼쪽(WindowTitleBar::Args::leftContent)에 둔다. 메뉴 하나가 열린 동안
// 다른 막대 버튼에 올리면 그 메뉴로 바꾸고, 열린 메뉴의 루트에서 왼쪽·오른쪽은 옆 메뉴로 옮긴다.
// 가속키(RegisterAccelerators): F10은 첫 버튼에 키보드 초점, Alt+글자는 그 글자로 시작하는 메뉴를
// 연다. 열린 메뉴 경로(막대·펼친 메뉴·하위 메뉴) 밖을 누르면 Application 누름 관찰자로 메뉴 전체를
// 닫고 그 누름을 삼킨다(UE 메뉴 스택 — 층이 창을 덮지 않으므로 막대 hover가 계속 닿는다).
//
// 사용 예:
//   auto bar = SharedPtr<MenuBar>::TryMake(allocator, allocator, context);
//   bar.Value()->AddMenu("File", MenuFill::CreateLambda([&](MenuBuilder& menu) noexcept {
//       menu.AddCommand(NewId, definitions, commands, bindings);
//       return Result<void>::Success();
//   }));
class ROCKS_UIFRAMEWORK_API MenuBar final : public Element {
public:
    MenuBar(Allocator& allocator, const SharedPtr<MenuContext>& context,
            const MenuBarStyle& style = DefaultMenuBarStyle()) noexcept;
    ~MenuBar() noexcept override;

    // 막대 끝에 메뉴 하나를 더한다(label은 막대 글자, fill은 열 때마다 메뉴를 채운다).
    Result<void> AddMenu(StringView label, MenuFill fill) noexcept;
    usize MenuCount() const noexcept;
    MenuBarButton* Button(usize index) const noexcept;
    // 열린 막대 메뉴의 번호다. 없으면 IndexNone이다.
    usize OpenIndex() const noexcept;
    // index 메뉴를 연다. focus면 첫 항목에 키보드 초점(외곽선)을 둔다. 다른 메뉴는 닫는다.
    Result<void> OpenMenu(usize index, bool focus) noexcept;
    // F10·Alt+글자 가속키를 전역 명령으로 등록한다(definitions·commands는 막대보다 오래 산다).
    Result<void> RegisterAccelerators(CommandSet& definitions, CommandList& commands) noexcept;

    // 막대 버튼이 알린다.
    void OnButtonHovered(MenuBarButton& button) noexcept;
    void OnButtonActivated(MenuBarButton& button) noexcept;

    Vec2 ComputeDesiredSize(float) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;

private:
    usize IndexOf(const MenuBarButton& button) const noexcept;
    bool Navigate(int32 step) noexcept;
    // 누름이 열린 메뉴 경로 밖이면 메뉴 전체를 닫고 true(누름 소비)다.
    bool CloseIfOutside(const Element* hit) noexcept;

    Allocator& allocator_;
    SharedPtr<MenuContext> context_;
    MenuBarStyle style_;
    Array<SharedPtr<MenuBarButton>> buttons_;
    Array<String> labels_;
    WeakPtr<Application*> app_;
    uint64 pressObserver_ = 0;
};

} // namespace rocks::ui
