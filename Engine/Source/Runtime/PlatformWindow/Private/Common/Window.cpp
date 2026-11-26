#include <PlatformWindow/Window.h>

namespace rocks::platform {

void Window::Minimize() noexcept {
}

void Window::ToggleMaximize() noexcept {
}

void Window::RequestClose() noexcept {
}

bool Window::IsMaximized() const noexcept {
    return false;
}

void Window::ToggleFullscreen() noexcept {
}

bool Window::IsFullscreen() const noexcept {
    return false;
}

bool Window::IsResizable() const noexcept {
    return true;
}

WindowZone Window::HitZone(IntVec2) const noexcept {
    return WindowZone::Client;
}

bool Window::IsActive() const noexcept {
    return HasFocus();
}

WindowFrame Window::Frame() const noexcept {
    return WindowFrame::System;
}

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

Result<IntRect> Window::WorkAreaAt(IntVec2) const noexcept {
    return Result<IntRect>::Failure(ErrorCode::Unsupported);
}

bool Window::IsAtDesktopPoint(IntVec2) const noexcept {
    return false;
}

} // namespace rocks::platform
