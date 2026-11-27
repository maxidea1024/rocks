#include <UiScreens/ScreenContainerWidget.h>

namespace rocks::ui {

Result<void> ScreenContainerWidget::BindPlayer(Application& app, uint32 user) noexcept {
    if (auto view = CachedElement(); view && (app_ != &app || user_ != user)) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    app_ = &app;
    user_ = user;
    return Result<void>::Success();
}

Result<SharedPtr<Element>> ScreenContainerWidget::BuildElement(ObjectRegistry& objects) noexcept {
    return app_ ? CreateContainer(objects, *app_, user_)
                : Result<SharedPtr<Element>>::Failure(ErrorCode::NotReady);
}

SharedPtr<ScreenContainer> ScreenContainerWidget::NativeContainer() const noexcept {
    auto live = CachedElement();
    return SharedPtr<ScreenContainer>(live, StaticElementCast<ScreenContainer>(live.Get()));
}

Result<ObjectHandle> ScreenContainerWidget::PushScreen(ClassRef<Screen> type) noexcept {
    auto live = NativeContainer();
    return live ? live->AddScreen(type) : Result<ObjectHandle>::Failure(ErrorCode::NotReady);
}

Result<void> ScreenContainerWidget::PushScreen(ObjectHandle screen) noexcept {
    auto live = NativeContainer();
    return live ? live->AddScreen(screen) : Result<void>::Failure(ErrorCode::NotReady);
}

Result<void> ScreenContainerWidget::RemoveScreen(ObjectHandle screen) noexcept {
    auto live = NativeContainer();
    return live ? live->RemoveScreen(screen) : Result<void>::Failure(ErrorCode::NotReady);
}

Result<void> ScreenContainerWidget::ClearScreens() noexcept {
    auto live = NativeContainer();
    return live ? live->ClearScreens() : Result<void>::Success();
}

usize ScreenContainerWidget::ScreenCount() const noexcept {
    auto live = NativeContainer();
    return live ? live->ScreenCount() : 0;
}

ObjectHandle ScreenContainerWidget::ActiveScreen() const noexcept {
    auto live = NativeContainer();
    return live ? live->ActiveScreen() : ObjectHandle{};
}

void ScreenContainerWidget::ReleaseElements() noexcept {
    (void)ClearScreens();
    Widget::ReleaseElements();
}
} // namespace rocks::ui
