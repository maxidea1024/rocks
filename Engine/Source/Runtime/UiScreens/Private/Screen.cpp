#include <UiWidget/Widget.generated.h>
#include <UiScreens/ScreenContainer.h>
#include <UiScreens/UiScreens.module.generated.h>

namespace rocks::ui {

Result<void> RegisterScreenTypes(ModuleRegistration& registration) noexcept {
    return generated::UiScreens::RegisterModuleTypes(registration);
}

Screen::Screen(Allocator& allocator) noexcept : UserWidget(allocator), bindings_(allocator) {
}

Result<void> Screen::BindAction(Guid action, Delegate<bool()>&& callback) noexcept {
    if (action == Guid{} || !callback) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    for (const auto& entry : bindings_) {
        if (entry.action == action) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }
    auto owned = SharedPtr<Delegate<bool()>>::TryMake(bindings_.GetAllocator(), Move(callback));
    if (!owned) {
        return Result<void>::Failure(owned.Error());
    }
    return bindings_.TryPushBack({action, Move(owned.Value())});
}

bool Screen::UnbindAction(Guid action) noexcept {
    for (usize i = 0; i < bindings_.Size(); ++i) {
        if (bindings_[i].action == action) {
            return static_cast<bool>(bindings_.TryRemoveAt(i));
        }
    }
    return false;
}

bool Screen::DispatchBoundAction(Guid action) noexcept {
    for (const auto& entry : bindings_) {
        if (entry.action == action) {
            // 콜백 안에서 등록을 해제하거나 화면을 닫아도 실행 중인 함수는 유지한다.
            auto callback = entry.callback;
            if (*callback && callback->Execute()) {
                return true;
            }
            break;
        }
    }
    return active_ && HandleAction(action);
}

void Screen::SetActionDomain(Guid domain) noexcept {
    domain_ = domain;
}
void Screen::SetInputConfig(ScreenInputConfig config) noexcept {
    config_ = config;
}
void Screen::SetModal(bool modal) noexcept {
    modal_ = modal;
}
Result<void> Screen::SetFocusTarget(ObjectRegistry& objects, ObjectHandle widget) noexcept {
    return objects.SetReference(objects.OwnerAccess(), Handle(), focusTarget_, widget);
}

bool Screen::IsActive() const noexcept {
    return active_;
}
bool Screen::IsModal() const noexcept {
    return modal_;
}
ScreenInputConfig Screen::InputConfig() const noexcept {
    return config_;
}
ObjectHandle Screen::FocusTarget() const noexcept {
    return focusTarget_.Handle();
}

Result<void> Screen::Dismiss() noexcept {
    return container_ ? container_->RemoveScreen(Handle())
                      : Result<void>::Failure(ErrorCode::NotFound);
}

Guid Screen::ActionDomain() const noexcept {
    return domain_;
}
bool Screen::HandleAction(Guid) noexcept {
    return false;
}

bool Screen::OnBack() noexcept {
    return false;
}
void Screen::OnActivated() noexcept {
}
void Screen::OnDeactivated() noexcept {
}

} // namespace rocks::ui
