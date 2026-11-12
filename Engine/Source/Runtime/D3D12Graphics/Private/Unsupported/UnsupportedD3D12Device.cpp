#include <D3D12Graphics/D3D12Device.h>

namespace rocks::graphics {

// Direct3D 12가 없는 플랫폼이다. 동작하는 척하지 않고 Unsupported를 반환한다.
Result<UniquePtr<Device>> CreateD3D12Device(Allocator&, const DeviceDesc&) noexcept {
    return Result<UniquePtr<Device>>::Failure(ErrorCode::Unsupported);
}

} // namespace rocks::graphics
