#include "VulkanApi.h"

#include <Windows.h>
#include <vulkan/vulkan_win32.h>

namespace rocks::graphics::vulkan {

// UE도 System32의 vulkan-1.dll을 연다(VulkanRHI/Private/Windows/VulkanWindowsPlatform.cpp:66).
const char* LoaderLibraryName() noexcept {
    return "vulkan-1.dll";
}

const char* SurfaceExtensionName() noexcept {
    return VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
}

VkResult CreateSurface(const Functions& functions, VkInstance instance, void* window,
                       void* instanceHandle, VkSurfaceKHR& surface) noexcept {
    const auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        functions.vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
    if (create == nullptr) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    VkWin32SurfaceCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = instanceHandle != nullptr ? static_cast<HINSTANCE>(instanceHandle)
                                               : GetModuleHandleW(nullptr);
    info.hwnd = static_cast<HWND>(window);
    return create(instance, &info, nullptr, &surface);
}

// PlatformBase MonotonicTicks는 QueryPerformanceCounter 값 그대로다.
VkTimeDomainKHR HostTimeDomain() noexcept {
    return VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
}

} // namespace rocks::graphics::vulkan
