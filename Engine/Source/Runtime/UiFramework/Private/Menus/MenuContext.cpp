#include <UiFramework/Popups/MenuAnchor.h>
#include <UiFramework/Menus/MenuContext.h>

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

} // namespace rocks::ui
