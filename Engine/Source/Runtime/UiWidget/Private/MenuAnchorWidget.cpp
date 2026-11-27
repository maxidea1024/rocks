#include "MakeWidgetElement.h"
#include <UiWidget/MenuAnchorWidget.generated.h>

namespace rocks::ui {

MenuAnchorWidget::~MenuAnchorWidget() noexcept {
    if (auto live = CachedElement()) {
        StaticElementCast<MenuAnchor>(*live).DisconnectMenu();
    }
}

MenuAnchorWidget::MenuAnchorWidget(Allocator& allocator) noexcept : ContentWidget(allocator) {
}

void MenuAnchorWidget::SetMenuFactory(MenuAnchor::MenuFactory&& factory) noexcept {
    factory_ = Move(factory);
}

Result<SharedPtr<Element>> MenuAnchorWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<MenuAnchor>(allocator);
}

Result<SharedPtr<Element>> MenuAnchorWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto value = PanelWidget::BuildElement(objects);
    if (value) {
        auto& menu = StaticElementCast<MenuAnchor>(*value.Value());
        menu.SetPlacement(static_cast<MenuPlacement>(placement_));
        menu.SetFitInWindow(fitInWindow_);
        menu.SetOnOpenChanged(
            Delegate<void(bool)>::CreateLambda([&objects, owner = Handle()](bool open) noexcept {
                auto value =
                    objects.ResolveForWrite<MenuAnchorWidget>(objects.OwnerAccess(), owner);
                if (value) {
                    value.Value().Get().Value()->openChanged_.ExecuteIfBound(open);
                }
            }));
        menu.SetMenuFactory(MenuAnchor::MenuFactory::CreateLambda([&objects, owner = Handle(),
                                                                   native = &menu]() noexcept {
            auto borrowed = objects.ResolveForWrite<MenuAnchorWidget>(objects.OwnerAccess(), owner);
            if (!borrowed) {
                return Result<SharedPtr<Element>>::Failure(borrowed.Error());
            }

            auto* widget = borrowed.Value().Get().Value();
            if (!widget->factory_ || widget->CachedElement().Get() != native) {
                return Result<SharedPtr<Element>>::Failure(ErrorCode::NotReady);
            }

            return widget->factory_.Execute();
        }));
    }

    return value;
}

Result<void> MenuAnchorWidget::Open() noexcept {
    auto live = CachedElement();
    return live ? StaticElementCast<MenuAnchor>(*live).Open()
                : Result<void>::Failure(ErrorCode::NotReady);
}

Result<void> MenuAnchorWidget::OpenFocused(Application& app, uint32 user) noexcept {
    auto live = CachedElement();
    return live ? StaticElementCast<MenuAnchor>(*live).OpenFocused(app, user, {})
                : Result<void>::Failure(ErrorCode::NotReady);
}

Result<void> MenuAnchorWidget::ToggleOpen() noexcept {
    if (IsOpen()) {
        Close();
        return Result<void>::Success();
    }

    return Open();
}

void MenuAnchorWidget::SetPlacement(MenuPlacement placement) noexcept {
    placement_ = static_cast<uint8>(placement);
    if (auto live = CachedElement()) {
        StaticElementCast<MenuAnchor>(*live).SetPlacement(placement);
    }
}

void MenuAnchorWidget::SetFitInWindow(bool fit) noexcept {
    fitInWindow_ = fit;
    if (auto live = CachedElement()) {
        StaticElementCast<MenuAnchor>(*live).SetFitInWindow(fit);
    }
}

Optional<Vec2> MenuAnchorWidget::MenuPosition() const noexcept {
    auto live = CachedElement();
    return live ? StaticElementCast<MenuAnchor>(*live).MenuPosition() : Optional<Vec2>{};
}

void MenuAnchorWidget::SetOnOpenChanged(Delegate<void(bool)>&& callback) noexcept {
    openChanged_ = Move(callback);
}

void MenuAnchorWidget::Close() noexcept {
    if (auto live = CachedElement()) {
        StaticElementCast<MenuAnchor>(*live).Close();
    }
}

bool MenuAnchorWidget::IsOpen() const noexcept {
    auto live = CachedElement();
    return live && StaticElementCast<MenuAnchor>(*live).IsOpen();
}

void MenuAnchorWidget::ReleaseElements() noexcept {
    if (auto live = CachedElement()) {
        StaticElementCast<MenuAnchor>(*live).DisconnectMenu();
    }

    PanelWidget::ReleaseElements();
}

} // namespace rocks::ui
