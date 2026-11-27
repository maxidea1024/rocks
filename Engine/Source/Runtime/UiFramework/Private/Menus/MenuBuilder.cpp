#include <UiFramework/Menus/MenuBuilder.h>
#include <UiFramework/Menus/MenuHeading.h>
#include <UiFramework/Menus/MenuSeparator.h>

namespace rocks::ui {

MenuBuilder::MenuBuilder(const SharedPtr<MenuContext>& context, MenuAnchor* owner) noexcept
    : context_(context) {
    if (!context_) {
        status_ = Result<void>::Failure(ErrorCode::InvalidArgument);
        return;
    }

    Allocator& allocator = context_->GetAllocator();
    auto panel = SharedPtr<MenuPanel>::TryMake(allocator, allocator, context_, owner);
    if (!panel) {
        status_ = Result<void>::Failure(panel.Error());
        return;
    }

    panel_ = Move(panel.Value());
}

void MenuBuilder::FlushSeparator() noexcept {
    // 구분선은 다음 항목이 올 때 넣는다(맨 앞·이어진·맨 끝 구분선이 생기지 않는다).
    if (!pendingSeparator_ || !status_) {
        return;
    }

    pendingSeparator_ = false;
    Allocator& allocator = context_->GetAllocator();
    auto separator = SharedPtr<MenuSeparator>::TryMake(allocator, allocator, context_->Style());
    status_ =
        separator ? panel_->AddItem(separator.Value()) : Result<void>::Failure(separator.Error());
}

Result<void> MenuBuilder::AddRow(StringView label, MenuAction&& action, KeyChord shortcut,
                                 MenuAnchor::MenuFactory&& subMenu) noexcept {
    const MenuStyle& style = context_->Style();
    auto text = context_->MakeLabel(label, style.label.font);
    if (!text) {
        return Result<void>::Failure(text.Error());
    }

    SharedPtr<TextLabel> keys;
    // 단축키가 없으면(빈 KeyChord) 칸을 비운다.
    if (shortcut.IsValid() && shortcut != KeyChord{}) {
        String chord(context_->GetAllocator());
        auto formatted = FormatKeyChord(shortcut, chord);
        auto made = formatted ? context_->MakeLabel(chord.View(), style.keybinding.font)
                              : Result<SharedPtr<TextLabel>>::Failure(formatted.Error());
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        keys = Move(made.Value());
    }

    Allocator& allocator = context_->GetAllocator();
    auto row = SharedPtr<MenuRow>::TryMake(allocator, allocator, context_);
    if (!row) {
        return Result<void>::Failure(row.Error());
    }

    auto built = row.Value()->Construct({.label = Move(text.Value()),
                                         .keybinding = Move(keys),
                                         .check = action.check,
                                         .isChecked = Move(action.isChecked),
                                         .canExecute = Move(action.canExecute),
                                         .execute = Move(action.execute),
                                         .subMenu = Move(subMenu),
                                         .icon = action.icon,
                                         .toolTip = action.toolTip});
    if (!built) {
        return built;
    }

    hasItems_ = true;
    return panel_->AddRow(row.Value());
}

void MenuBuilder::AddEntry(StringView label, MenuAction action, KeyChord shortcut) noexcept {
    FlushSeparator();
    if (status_) {
        status_ = AddRow(label, Move(action), shortcut, {});
    }
}

void MenuBuilder::AddCommand(Guid id, const CommandSet& definitions, const CommandList& commands,
                             const BindingManager& bindings, MenuCheckKind check,
                             VectorImageKey icon) noexcept {
    const CommandInfo* info = definitions.Find(id);
    if (!info) {
        if (status_) {
            status_ = Result<void>::Failure(ErrorCode::NotFound);
        }

        return;
    }

    // 명령 목록은 메뉴보다 오래 산다(문서 계약). 실행·사용 가능·체크는 그때그때 묻는다.
    const CommandList* list = &commands;
    MenuAction action{
        .execute =
            Delegate<void()>::CreateLambda([list, id]() noexcept { (void)list->Execute(id); }),
        .canExecute =
            Delegate<bool()>::CreateLambda([list, id]() noexcept { return list->IsEnabled(id); }),
        .isChecked =
            Delegate<bool()>::CreateLambda([list, id]() noexcept { return list->IsChecked(id); }),
        .check = check,
        .icon = icon,
        .toolTip = info->description.View()};
    AddEntry(info->label.View(), Move(action), bindings.Chords(*info).primary);
}

void MenuBuilder::AddSeparator() noexcept {
    pendingSeparator_ = hasItems_;
}

void MenuBuilder::BeginSection(StringView heading) noexcept {
    AddSeparator();
    if (heading.IsEmpty() || !status_) {
        return;
    }

    FlushSeparator();
    if (!status_) {
        return;
    }

    const MenuStyle& style = context_->Style();
    Allocator& allocator = context_->GetAllocator();
    // 묶음 제목은 대문자로 보인다(UE SHeadingBlock — ASCII만 바꾸고 다른 글자는 그대로).
    String upper(allocator);
    for (usize i = 0; status_ && i < heading.ByteCount(); ++i) {
        char c = heading.Data()[i];
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }

        status_ = upper.TryAppend(StringView(&c, 1));
    }

    if (!status_) {
        return;
    }

    auto text = context_->MakeLabel(upper.View(), style.heading.font);
    auto title = SharedPtr<MenuHeading>::TryMake(allocator, allocator, style);
    if (!text || !title) {
        status_ = Result<void>::Failure(!text ? text.Error() : title.Error());
        return;
    }

    status_ = title.Value()->SetLabel(text.Value());
    if (status_) {
        status_ = panel_->AddItem(title.Value());
    }
}

void MenuBuilder::AddSubMenu(StringView label, MenuFill fill) noexcept {
    FlushSeparator();
    if (!status_) {
        return;
    }

    // 하위 메뉴는 열 때마다 채운다(UE FNewMenuDelegate). 하위 메뉴의 owner는 그 항목이다 — 항목이
    // 만들어진 뒤 factory를 바꿔 단다.
    status_ = AddRow(label, {}, {}, MenuAnchor::MenuFactory::CreateLambda([]() noexcept {
                         return Result<SharedPtr<Element>>::Failure(ErrorCode::NotReady);
                     }));
    if (!status_) {
        return;
    }

    auto& row = StaticElementCast<MenuRow>(*panel_->ChildAt(panel_->ChildCount() - 1));
    // 채우기 delegate를 품은 factory는 인라인 저장보다 커서 allocator에 둔다.
    auto factory = MenuAnchor::MenuFactory::TryCreateLambda(
        context_->GetAllocator(),
        [context = context_, owner = &row,
         fill = Move(fill)]() noexcept -> Result<SharedPtr<Element>> {
            MenuBuilder sub(context, owner);
            if (fill.IsBound()) {
                auto filled = fill.Execute(sub);
                if (!filled) {
                    return Result<SharedPtr<Element>>::Failure(filled.Error());
                }
            }

            auto panel = sub.MakePanel();
            if (!panel) {
                return Result<SharedPtr<Element>>::Failure(panel.Error());
            }

            return Result<SharedPtr<Element>>::Success(panel.Value());
        });
    if (!factory) {
        status_ = Result<void>::Failure(factory.Error());
        return;
    }

    row.SetMenuFactory(Move(factory.Value()));
}

Result<SharedPtr<MenuPanel>> MenuBuilder::MakePanel() noexcept {
    if (!status_) {
        return Result<SharedPtr<MenuPanel>>::Failure(status_.Error());
    }

    return Result<SharedPtr<MenuPanel>>::Success(panel_);
}

} // namespace rocks::ui
