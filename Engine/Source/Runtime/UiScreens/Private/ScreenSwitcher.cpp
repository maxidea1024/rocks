#include <UiScreens/ScreenSwitcher.h>

namespace rocks::ui {

ScreenSwitcher::ScreenSwitcher(ObjectRegistry& objects, Application& app, uint32 user) noexcept
    : ScreenContainer(objects, app, user, Order::Switcher) {
}

Result<void> ScreenSwitcher::SetActivePage(usize index) noexcept {
    return ActivateAt(index);
}
} // namespace rocks::ui
