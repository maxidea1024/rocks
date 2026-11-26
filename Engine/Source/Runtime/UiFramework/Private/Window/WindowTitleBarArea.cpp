#include <UiFramework/Window/WindowTitleBarArea.h>

namespace rocks::ui {

WindowTitleBarArea::WindowTitleBarArea(Allocator& allocator) noexcept : Frame(allocator) {
    (void)SetBrush({.mode = BrushMode::None});
}

void WindowTitleBarArea::SetDoubleClickTogglesFullscreen(bool enabled) noexcept {
    fullscreen_ = enabled;
}

input::WindowRegion WindowTitleBarArea::GetWindowRegion() const noexcept {
    return fullscreen_ ? input::WindowRegion::FullscreenCaption : input::WindowRegion::Caption;
}
} // namespace rocks::ui
