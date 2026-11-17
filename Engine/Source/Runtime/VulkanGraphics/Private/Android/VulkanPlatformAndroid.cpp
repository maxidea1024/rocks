#include "VulkanApi.h"

#include <vulkan/vulkan_android.h>

namespace rocks::graphics::vulkan {

// UE도 libvulkan.so를 연다(VulkanRHI/Private/Android/VulkanAndroidPlatform.cpp:314).
const char* LoaderLibraryName() noexcept {
    return "libvulkan.so";
}

const char* SurfaceExtensionName() noexcept {
    return VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
}

// window는 ANativeWindow*다. instanceHandle은 쓰지 않는다. 창 수명(onNativeWindowCreated·
// Destroyed)과의 연결은 Android 실행 기반(ShaderSystem P13)이 맡는다.
VkResult CreateSurface(const Functions& functions, VkInstance instance, void* window, void*,
                       VkSurfaceKHR& surface) noexcept {
    const auto create = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
        functions.vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR"));
    if (create == nullptr) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    VkAndroidSurfaceCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    info.window = static_cast<ANativeWindow*>(window);
    return create(instance, &info, nullptr, &surface);
}

// PlatformBase MonotonicTicks는 CLOCK_MONOTONIC의 ns다(Private/Android/AndroidPosix.cpp).
VkTimeDomainKHR HostTimeDomain() noexcept {
    return VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
}

} // namespace rocks::graphics::vulkan
