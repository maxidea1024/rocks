#include <Platform/Memory.h>
#include <VulkanGraphics/VulkanDevice.h>

namespace rocks::graphics {

// Vulkan을 쓰지 않는 플랫폼(macOS는 Metal)이다. 동작하는 척하지 않고 Unsupported를 반환한다.
Result<UniquePtr<Device>> CreateVulkanDevice(Allocator&, const DeviceDesc&,
                                             DeviceCreateReport* report) noexcept {
    if (report != nullptr) {
        *report = {};
        report->code = ErrorCode::Unsupported;
        constexpr char Stage[] = "Platform";
        constexpr char Detail[] = "Vulkan is not a backend on this platform";
        platform::MemoryCopy(report->stage, Stage, sizeof(Stage));
        platform::MemoryCopy(report->detail, Detail, sizeof(Detail));
    }

    return Result<UniquePtr<Device>>::Failure(ErrorCode::Unsupported);
}

} // namespace rocks::graphics
