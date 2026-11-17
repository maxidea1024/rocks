#include <PlatformWindow/Window.h>

namespace rocks::platform {

// 창·게임패드 backend가 아직 없는 플랫폼(macOS 등)이다. 동작하는 척하지 않고 Unsupported를
// 반환한다.

Result<UniquePtr<Window>> Window::Create(Allocator&, const WindowDesc& desc,
                                         WindowCreateReport* report) noexcept {
    const ErrorCode error =
        desc.width == 0 || desc.height == 0 ? ErrorCode::InvalidArgument : ErrorCode::Unsupported;
    if (report != nullptr) {
        *report = WindowCreateReport{};
        report->code = error;
    }

    return Result<UniquePtr<Window>>::Failure(error);
}

Result<UniquePtr<GamepadBackend>> GamepadBackend::Create(Allocator&) noexcept {
    return Result<UniquePtr<GamepadBackend>>::Failure(ErrorCode::Unsupported);
}

DesktopSessionState QueryDesktopSessionState() noexcept {
    return DesktopSessionState::Unknown;
}

} // namespace rocks::platform
