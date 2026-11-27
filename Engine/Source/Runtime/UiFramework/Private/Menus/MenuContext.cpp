#include <UiFramework/Application/Application.h>
#include <UiFramework/Menus/MenuContext.h>
#include <UiFramework/Menus/MenuPanel.h>
#include <UiFramework/Popups/MenuAnchor.h>

namespace rocks::ui {

MenuContext::MenuContext(Allocator& allocator, Application& application,
                         MenuLabelFactory&& makeLabel, const MenuStyle& style) noexcept
    : allocator_(allocator), application_(application), makeLabel_(Move(makeLabel)), style_(style) {
}

Allocator& MenuContext::GetAllocator() const noexcept {
    return allocator_;
}

Application& MenuContext::GetApplication() const noexcept {
    return application_;
}

const MenuStyle& MenuContext::Style() const noexcept {
    return style_;
}

Result<SharedPtr<TextLabel>> MenuContext::MakeLabel(StringView text,
                                                    const FontStyle& font) const noexcept {
    if (!makeLabel_.IsBound()) {
        return Result<SharedPtr<TextLabel>>::Failure(ErrorCode::NotReady);
    }

    return makeLabel_.Execute(text, font);
}

void MenuContext::SetRoot(MenuAnchor* root) noexcept {
    root_ = root;
}

MenuAnchor* MenuContext::Root() const noexcept {
    return root_;
}

void MenuContext::CloseAll() noexcept {
    if (root_) {
        root_->Close();
    }

    closedAll_.ExecuteIfBound();
}

void MenuContext::SetOnClosedAll(Delegate<void()>&& callback) noexcept {
    closedAll_ = Move(callback);
}

bool MenuContext::NavigateRoot(int32 step) noexcept {
    return navigateRoot_.IsBound() && navigateRoot_.Execute(step);
}

void MenuContext::SetOnNavigateRoot(Delegate<bool(int32)>&& callback) noexcept {
    navigateRoot_ = Move(callback);
}

bool MenuContext::IsInOpenMenus(const Element& element) const noexcept {
    // 루트 메뉴부터 열린 하위 메뉴를 따라간다. 메뉴 내용은 늘 MenuBuilder가 만든 MenuPanel이다.
    const MenuAnchor* anchor = root_;
    while (anchor) {
        const Element* content = anchor->MenuContent();
        if (!content) {
            return false;
        }

        if (element.IsWithin(*content)) {
            return true;
        }

        anchor = StaticElementCast<MenuPanel>(content)->OpenSubMenu();
    }

    return false;
}

MenuRow* MenuContext::FindOpenRow(const Element& element) const noexcept {
    const MenuAnchor* anchor = root_;
    while (anchor) {
        const auto* panel = StaticElementCast<MenuPanel>(anchor->MenuContent());
        if (!panel) {
            return nullptr;
        }

        if (element.IsWithin(*panel)) {
            for (const Element* current = &element; current && current != panel;
                 current = current->Parent()) {
                if (MenuRow* row = panel->FindRow(current)) {
                    return row;
                }
            }

            return nullptr;
        }

        anchor = panel->OpenSubMenu();
    }

    return nullptr;
}

void MenuContext::BeginSession(uint32 user, const Element& area) noexcept {
    const auto focus = application_.GetFocus(user);
    if (session_ && focus && (focus->IsWithin(area) || IsInOpenMenus(*focus))) {
        return;
    }

    savedFocus_ = WeakPtr<Element>(focus);
    savedCause_ = focus ? application_.GetFocusCause(user) : FocusCause::Direct;
    sessionUser_ = user;
    session_ = true;
}

bool MenuContext::InSession() const noexcept {
    return session_;
}

void MenuContext::EndSession(const Element& area, bool restore) noexcept {
    if (!session_) {
        return;
    }

    session_ = false;
    auto saved = savedFocus_.Lock();
    savedFocus_.Reset();
    if (!restore) {
        return;
    }

    const auto current = application_.GetFocus(sessionUser_);
    const auto& root = application_.GetRoot();
    const bool inMenus = !current || current->IsWithin(area) || IsInOpenMenus(*current) || !root ||
                         !current->IsWithin(*root);
    if (!inMenus) {
        return;
    }

    // 기억한 요소가 그사이 트리에서 떨어졌으면 초점을 비운다(떨어진 요소에 두지 않는다). 초점
    // 콜백 안이라 Busy면 그대로 둔다 — 다음 입력에서 떨어진 초점은 Application이 지운다.
    if (saved && root && saved->IsWithin(*root)) {
        (void)application_.SetFocus(sessionUser_, saved, savedCause_);
    } else {
        (void)application_.SetFocus(sessionUser_, {});
    }
}

} // namespace rocks::ui
