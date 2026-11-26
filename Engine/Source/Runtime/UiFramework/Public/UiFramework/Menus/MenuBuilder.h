#pragma once

#include <UiFramework/Application/Commands.h>
#include <UiFramework/Menus/MenuPanel.h>

namespace rocks::ui {

class MenuBuilder;

// 하위 메뉴를 채우는 delegate다(UE FNewMenuDelegate). 열 때마다 새로 부른다.
using MenuFill = Delegate<Result<void>(MenuBuilder&)>;

// 메뉴 항목 하나의 동작이다(UE FUIAction). 체크·라디오 항목은 isChecked로 표시를 정한다.
struct MenuAction {
    Delegate<void()> execute{};
    Delegate<bool()> canExecute{};
    Delegate<bool()> isChecked{};
    MenuCheckKind check = MenuCheckKind::None;
};

// 메뉴를 짓는다(UE FMenuBuilder — Docs/Design/UiToolkit 9.1절 UW7c). 항목·명령·구분선·묶음 제목·
// 하위 메뉴를 차례로 더하고 MakePanel로 펼친 메뉴를 만든다. 글자는 문맥의 레이블 factory로
// shaping한다. 묶음 사이에는 구분선을 한 번만 넣는다(이어진 구분선·맨 앞 구분선은 넣지 않는다). 첫
// 실패를 보존하고 그 뒤 호출은 아무것도 하지 않는다 — MakePanel이 그 오류를 돌려준다.
//
// 사용 예(MenuAnchor 메뉴 factory 안에서):
//   MenuBuilder menu(context, anchor);
//   menu.AddCommand(SaveId, definitions, commands, bindings);
//   menu.AddSeparator();
//   menu.AddSubMenu("Recent Files", MenuFill::CreateLambda(...));
//   return menu.MakePanel();
class ROCKS_UIFRAMEWORK_API MenuBuilder final {
public:
    // owner는 이 메뉴를 여는 anchor다(루트면 막대 버튼, 하위면 항목).
    MenuBuilder(const SharedPtr<MenuContext>& context, MenuAnchor* owner) noexcept;

    // 글자 항목이다. shortcut은 표시만 한다(실행은 명령 목록의 몫).
    void AddEntry(StringView label, MenuAction action, KeyChord shortcut = {}) noexcept;
    // 명령 항목이다(라벨·단축키는 정의와 바인딩, 실행·사용 가능·체크는 명령 목록에서 — UE
    // FMenuBuilder::AddMenuEntry(FUICommandInfo)). 명령 목록은 메뉴보다 오래 살아야 한다.
    void AddCommand(Guid id, const CommandSet& definitions, const CommandList& commands,
                    const BindingManager& bindings,
                    MenuCheckKind check = MenuCheckKind::None) noexcept;
    void AddSeparator() noexcept;
    // 묶음을 시작한다. heading이 비어 있지 않으면 묶음 제목을 둔다(앞 묶음과는 구분선으로 나눈다).
    void BeginSection(StringView heading = {}) noexcept;
    void AddSubMenu(StringView label, MenuFill fill) noexcept;

    Result<SharedPtr<MenuPanel>> MakePanel() noexcept;

private:
    Result<void> AddRow(StringView label, MenuAction&& action, KeyChord shortcut,
                        MenuAnchor::MenuFactory&& subMenu) noexcept;
    void FlushSeparator() noexcept;

    SharedPtr<MenuContext> context_;
    SharedPtr<MenuPanel> panel_;
    Result<void> status_ = Result<void>::Success();
    bool pendingSeparator_ = false;
    bool hasItems_ = false;
};

} // namespace rocks::ui
