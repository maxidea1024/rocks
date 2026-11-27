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

void Window::PumpEvents(input::InputQueue& queue) noexcept {
    PumpMessages();
    TakeEvents(queue);
}

Result<void> Window::SetWindowMode(WindowMode mode) noexcept {
    return mode == WindowMode::Windowed ? Result<void>::Success()
                                        : Result<void>::Failure(ErrorCode::Unsupported);
}

WindowMode Window::GetWindowMode() const noexcept {
    return WindowMode::Windowed;
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

float Window::DpiScale() const noexcept {
    return 1.0f;
}

bool Window::ConsumeDpiChange(float&) noexcept {
    return false;
}

void Window::ActivateTextInput(TextInputClient&) noexcept {
}

void Window::DeactivateTextInput(TextInputClient&) noexcept {
}

void Window::CancelTextComposition(TextInputClient&) noexcept {
}

void Window::NotifyTextInputLayoutChanged(TextInputClient&) noexcept {
}

void Window::ProcessTextInput(uint32) noexcept {
}

bool Window::ConsumeRepaint() noexcept {
    return true;
}

void Window::SetLiveResizeHandler(LiveResizeHandler&&) noexcept {
}

void Window::PublishRegionMap(const input::WindowRegionMap&) noexcept {
}

void Window::SetMouseCursor(input::MouseCursor) noexcept {
}

Result<void> Window::SetCursorVisible(bool) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

Result<void> Window::SetMouseLocked(bool) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
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
