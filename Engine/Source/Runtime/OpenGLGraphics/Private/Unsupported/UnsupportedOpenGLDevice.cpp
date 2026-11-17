#include <OpenGLGraphics/OpenGLDevice.h>

namespace rocks::graphics {

namespace {

constexpr char UnsupportedStage[] = "CreateOpenGLDevice";
constexpr char UnsupportedDetail[] = "the OpenGL ES backend has no platform layer on this platform";

void FillUnsupported(DeviceCreateReport* report) noexcept {
    if (report == nullptr) {
        return;
    }

    *report = DeviceCreateReport{};
    report->code = ErrorCode::Unsupported;
    for (usize index = 0; index + 1 < sizeof(report->stage) && UnsupportedStage[index] != 0;
         ++index) {
        report->stage[index] = UnsupportedStage[index];
    }

    for (usize index = 0; index + 1 < sizeof(report->detail) && UnsupportedDetail[index] != 0;
         ++index) {
        report->detail[index] = UnsupportedDetail[index];
    }
}

} // namespace

// OpenGL ES 플랫폼 층(WGL·EGL)이 없는 플랫폼이다(macOS는 Metal을 쓴다). 동작하는 척하지 않고
// Unsupported를 반환한다.
Result<UniquePtr<Device>> CreateOpenGLDevice(Allocator&, const DeviceDesc&,
                                             DeviceCreateReport* report) noexcept {
    FillUnsupported(report);
    return Result<UniquePtr<Device>>::Failure(ErrorCode::Unsupported);
}

Result<UniquePtr<Device>> CreateOpenGLDevice(Allocator&, const DeviceDesc&,
                                             const OpenGLDeviceOptions&,
                                             DeviceCreateReport* report) noexcept {
    FillUnsupported(report);
    return Result<UniquePtr<Device>>::Failure(ErrorCode::Unsupported);
}

} // namespace rocks::graphics
