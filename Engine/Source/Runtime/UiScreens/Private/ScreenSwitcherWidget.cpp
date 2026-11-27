#include <UiScreens/ScreenSwitcherWidget.h>
#include <UiScreens/ScreenSwitcher.h>

namespace rocks::ui {

Result<SharedPtr<Element>> ScreenSwitcherWidget::CreateContainer(ObjectRegistry& objects,
                                                                 Application& app,
                                                                 uint32 user) noexcept {
    auto view = SharedPtr<ScreenSwitcher>::TryMake(objects.ObjectAllocator(), objects, app, user);
    return view ? Result<SharedPtr<Element>>::Success(Move(view.Value()))
                : Result<SharedPtr<Element>>::Failure(view.Error());
}

Result<void> ScreenSwitcherWidget::SetActivePage(usize index) noexcept {
    auto view = NativeContainer();
    return view ? StaticElementCast<ScreenSwitcher>(*view).SetActivePage(index)
                : Result<void>::Failure(ErrorCode::NotReady);
}
} // namespace rocks::ui
