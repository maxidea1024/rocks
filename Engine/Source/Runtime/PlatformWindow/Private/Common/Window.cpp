#include <PlatformWindow/Window.h>

namespace rocks::platform {

bool Window::ConsumeRepaint() noexcept {
    return true;
}

void Window::WaitForEvents(uint32) noexcept {
}

void Window::SetLiveResizeHandler(LiveResizeHandler&&) noexcept {
}

void Window::SetRegionHitTest(RegionHitTest&&) noexcept {
}

void Window::SetMouseCursor(input::MouseCursor) noexcept {
}

Result<IntVec2> Window::ClientToDesktop(IntVec2) const noexcept {
    return Result<IntVec2>::Failure(ErrorCode::Unsupported);
}

Result<IntVec2> Window::DesktopToClient(IntVec2) const noexcept {
    return Result<IntVec2>::Failure(ErrorCode::Unsupported);
}

bool Window::IsAtDesktopPoint(IntVec2) const noexcept {
    return false;
}

} // namespace rocks::platform
