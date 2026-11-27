#include <UiScreens/ScreenStackWidget.h>

namespace rocks::ui {

Result<SharedPtr<Element>> ScreenStackWidget::CreateContainer(ObjectRegistry& objects,
                                                              Application& app,
                                                              uint32 user) noexcept {
    auto view = SharedPtr<ScreenStack>::TryMake(objects.ObjectAllocator(), objects, app, user);
    return view ? Result<SharedPtr<Element>>::Success(Move(view.Value()))
                : Result<SharedPtr<Element>>::Failure(view.Error());
}
} // namespace rocks::ui
