#include <D3D11Graphics/D3D11Device.h>

namespace rocks::graphics {

// Direct3D 11이 없는 플랫폼이다. 동작하는 척하지 않고 Unsupported를 반환한다.
Result<UniquePtr<Device>> CreateD3D11Device(Allocator&, const DeviceDesc&,
                                            DeviceCreateReport* report) noexcept {
    if (report != nullptr) {
        *report = DeviceCreateReport{};
        report->code = ErrorCode::Unsupported;
    }

    return Result<UniquePtr<Device>>::Failure(ErrorCode::Unsupported);
}

} // namespace rocks::graphics
