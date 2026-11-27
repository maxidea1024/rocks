#include <PlatformWindow/Window.h>

namespace rocks::platform {

Result<UniquePtr<DragImage>> DragImage::Create(Allocator&, const Window&, IntVec2, IntVec2,
                                               Span<const uint8>, uint8) noexcept {
    return Result<UniquePtr<DragImage>>::Failure(ErrorCode::Unsupported);
}

// 창·게임패드 backend가 아직 없는 플랫폼(macOS 등)이다. 동작하는 척하지 않고 Unsupported를
// 반환한다.

bool Window::SupportsFrame(WindowFrame) noexcept {
    return false;
}

Result<UniquePtr<Window>> Window::Create(Allocator&, const WindowDesc& desc,
                                         WindowCreateReport* report) noexcept {
    const ErrorCode error = desc.width == 0 || desc.height == 0 || !desc.title.IsValidUtf8()
                                ? ErrorCode::InvalidArgument
                                : ErrorCode::Unsupported;
    if (report != nullptr) {
        *report = WindowCreateReport{};
        report->code = error;
    }

    return Result<UniquePtr<Window>>::Failure(error);
}

Result<UniquePtr<GamepadBackend>> GamepadBackend::Create(Allocator&) noexcept {
    return Result<UniquePtr<GamepadBackend>>::Failure(ErrorCode::Unsupported);
}

void PumpSentMessages() noexcept {
}

DesktopSessionState QueryDesktopSessionState() noexcept {
    return DesktopSessionState::Unknown;
}

} // namespace rocks::platform
