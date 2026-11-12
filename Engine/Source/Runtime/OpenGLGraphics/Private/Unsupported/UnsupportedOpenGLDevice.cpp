#include <OpenGLGraphics/OpenGLDevice.h>

namespace rocks::graphics {

// OpenGL backend가 아직 없는 플랫폼이다. 동작하는 척하지 않고 Unsupported를 반환한다.
Result<UniquePtr<Device>> CreateOpenGLDevice(Allocator&, const DeviceDesc&) noexcept {
    return Result<UniquePtr<Device>>::Failure(ErrorCode::Unsupported);
}

} // namespace rocks::graphics
