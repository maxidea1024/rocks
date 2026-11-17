#pragma once

#include <Base/Types.h>
#include <Platform/DynamicLibrary.h>
#include <vulkan/vulkan_core.h>

// Vulkan 함수 표다(Docs/Design/VulkanGraphics.ko.md 3절). 로더를 링크하지 않으므로
// (VK_NO_PROTOTYPES) 모든 함수를 실행 중에 얻는다. 표는 장치 객체 안에 두며 전역 상태가 없다.
//
// - 전역 함수: vkGetInstanceProcAddr(nullptr, 이름)
// - instance 함수와 instance 확장 명령(debug utils label 포함): vkGetInstanceProcAddr(instance,
// 이름)
// - 장치 함수: vkGetDeviceProcAddr(device, 이름)(loader trampoline을 거치지 않는다)
//
// 필수 목록의 함수가 없으면 그 단계가 실패하고, 선택 목록(확장)의 함수는 없으면 null로 남는다.
namespace rocks::graphics::vulkan {

#define ROCKS_VULKAN_GLOBAL_FUNCTIONS(X)                                                           \
    X(vkEnumerateInstanceVersion)                                                                  \
    X(vkEnumerateInstanceExtensionProperties)                                                      \
    X(vkEnumerateInstanceLayerProperties)                                                          \
    X(vkCreateInstance)

#define ROCKS_VULKAN_INSTANCE_FUNCTIONS(X)                                                         \
    X(vkDestroyInstance)                                                                           \
    X(vkEnumeratePhysicalDevices)                                                                  \
    X(vkGetPhysicalDeviceProperties)                                                               \
    X(vkGetPhysicalDeviceFeatures)                                                                 \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                    \
    X(vkGetPhysicalDeviceMemoryProperties)                                                         \
    X(vkGetPhysicalDeviceFormatProperties)                                                         \
    X(vkEnumerateDeviceExtensionProperties)                                                        \
    X(vkCreateDevice)                                                                              \
    X(vkGetDeviceProcAddr)

#define ROCKS_VULKAN_INSTANCE_OPTIONAL_FUNCTIONS(X)                                                \
    X(vkDestroySurfaceKHR)                                                                         \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                        \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                   \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                        \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)                                                   \
    X(vkCreateDebugUtilsMessengerEXT)                                                              \
    X(vkDestroyDebugUtilsMessengerEXT)                                                             \
    X(vkCmdBeginDebugUtilsLabelEXT)                                                                \
    X(vkCmdEndDebugUtilsLabelEXT)                                                                  \
    X(vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)                                              \
    X(vkGetPhysicalDeviceCalibrateableTimeDomainsEXT)

#define ROCKS_VULKAN_DEVICE_FUNCTIONS(X)                                                           \
    X(vkDestroyDevice)                                                                             \
    X(vkGetDeviceQueue)                                                                            \
    X(vkDeviceWaitIdle)                                                                            \
    X(vkQueueSubmit)                                                                               \
    X(vkQueueWaitIdle)                                                                             \
    X(vkCreateFence)                                                                               \
    X(vkDestroyFence)                                                                              \
    X(vkResetFences)                                                                               \
    X(vkGetFenceStatus)                                                                            \
    X(vkWaitForFences)                                                                             \
    X(vkCreateSemaphore)                                                                           \
    X(vkDestroySemaphore)                                                                          \
    X(vkCreateCommandPool)                                                                         \
    X(vkDestroyCommandPool)                                                                        \
    X(vkAllocateCommandBuffers)                                                                    \
    X(vkFreeCommandBuffers)                                                                        \
    X(vkResetCommandBuffer)                                                                        \
    X(vkBeginCommandBuffer)                                                                        \
    X(vkEndCommandBuffer)                                                                          \
    X(vkAllocateMemory)                                                                            \
    X(vkFreeMemory)                                                                                \
    X(vkMapMemory)                                                                                 \
    X(vkUnmapMemory)                                                                               \
    X(vkInvalidateMappedMemoryRanges)                                                              \
    X(vkCreateBuffer)                                                                              \
    X(vkDestroyBuffer)                                                                             \
    X(vkGetBufferMemoryRequirements2)                                                              \
    X(vkBindBufferMemory)                                                                          \
    X(vkCreateImage)                                                                               \
    X(vkDestroyImage)                                                                              \
    X(vkGetImageMemoryRequirements2)                                                               \
    X(vkBindImageMemory)                                                                           \
    X(vkCreateImageView)                                                                           \
    X(vkDestroyImageView)                                                                          \
    X(vkCreateSampler)                                                                             \
    X(vkDestroySampler)                                                                            \
    X(vkCreateDescriptorSetLayout)                                                                 \
    X(vkDestroyDescriptorSetLayout)                                                                \
    X(vkCreatePipelineLayout)                                                                      \
    X(vkDestroyPipelineLayout)                                                                     \
    X(vkCreateDescriptorPool)                                                                      \
    X(vkDestroyDescriptorPool)                                                                     \
    X(vkAllocateDescriptorSets)                                                                    \
    X(vkFreeDescriptorSets)                                                                        \
    X(vkUpdateDescriptorSets)                                                                      \
    X(vkCreateRenderPass)                                                                          \
    X(vkDestroyRenderPass)                                                                         \
    X(vkCreateFramebuffer)                                                                         \
    X(vkDestroyFramebuffer)                                                                        \
    X(vkCreateShaderModule)                                                                        \
    X(vkDestroyShaderModule)                                                                       \
    X(vkCreatePipelineCache)                                                                       \
    X(vkDestroyPipelineCache)                                                                      \
    X(vkCreateGraphicsPipelines)                                                                   \
    X(vkDestroyPipeline)                                                                           \
    X(vkCreateQueryPool)                                                                           \
    X(vkDestroyQueryPool)                                                                          \
    X(vkGetQueryPoolResults)                                                                       \
    X(vkCmdResetQueryPool)                                                                         \
    X(vkCmdWriteTimestamp)                                                                         \
    X(vkCmdPipelineBarrier)                                                                        \
    X(vkCmdBeginRenderPass)                                                                        \
    X(vkCmdEndRenderPass)                                                                          \
    X(vkCmdBindPipeline)                                                                           \
    X(vkCmdBindVertexBuffers)                                                                      \
    X(vkCmdBindIndexBuffer)                                                                        \
    X(vkCmdBindDescriptorSets)                                                                     \
    X(vkCmdPushConstants)                                                                          \
    X(vkCmdSetViewport)                                                                            \
    X(vkCmdSetScissor)                                                                             \
    X(vkCmdDraw)                                                                                   \
    X(vkCmdDrawIndexed)                                                                            \
    X(vkCmdCopyBuffer)                                                                             \
    X(vkCmdCopyBufferToImage)                                                                      \
    X(vkCmdCopyImageToBuffer)                                                                      \
    X(vkCmdCopyImage)                                                                              \
    X(vkCmdBlitImage)

#define ROCKS_VULKAN_DEVICE_OPTIONAL_FUNCTIONS(X)                                                  \
    X(vkCreateSwapchainKHR)                                                                        \
    X(vkDestroySwapchainKHR)                                                                       \
    X(vkGetSwapchainImagesKHR)                                                                     \
    X(vkAcquireNextImageKHR)                                                                       \
    X(vkQueuePresentKHR)                                                                           \
    X(vkGetCalibratedTimestampsKHR)                                                                \
    X(vkGetCalibratedTimestampsEXT)

#define ROCKS_VULKAN_DECLARE_FUNCTION(name) PFN_##name name = nullptr;

// 함수 표다. 각 칸은 Vulkan 함수 이름과 같다.
struct Functions {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    ROCKS_VULKAN_GLOBAL_FUNCTIONS(ROCKS_VULKAN_DECLARE_FUNCTION)
    ROCKS_VULKAN_INSTANCE_FUNCTIONS(ROCKS_VULKAN_DECLARE_FUNCTION)
    ROCKS_VULKAN_INSTANCE_OPTIONAL_FUNCTIONS(ROCKS_VULKAN_DECLARE_FUNCTION)
    ROCKS_VULKAN_DEVICE_FUNCTIONS(ROCKS_VULKAN_DECLARE_FUNCTION)
    ROCKS_VULKAN_DEVICE_OPTIONAL_FUNCTIONS(ROCKS_VULKAN_DECLARE_FUNCTION)
};

#undef ROCKS_VULKAN_DECLARE_FUNCTION

// 시스템 Vulkan 로더다. 열린 동안 lease가 vkGetInstanceProcAddr를 유효하게 둔다.
class Loader {
public:
    Loader() noexcept = default;
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    ~Loader() noexcept;

    // 플랫폼 로더(LoaderLibraryName)를 열고 vkGetInstanceProcAddr와 전역 함수를 functions에
    // 채운다. 실패하면 OS 상태를 돌려준다(전역 함수가 없으면 NotFound, missing에 이름).
    platform::FileStatus Load(Functions& functions, const char*& missing) noexcept;

private:
    platform::DynamicLibrary library_;
    platform::DynamicLibraryLease lease_;
};

// instance 함수를 채운다. 필수 함수가 없으면 그 이름, 다 있으면 nullptr다.
const char* LoadInstanceFunctions(VkInstance instance, Functions& functions) noexcept;

// 장치 함수를 채운다. 필수 함수가 없으면 그 이름, 다 있으면 nullptr다.
const char* LoadDeviceFunctions(VkDevice device, Functions& functions) noexcept;

// VkResult의 이름이다("VK_ERROR_DEVICE_LOST" 등, 모르면 "VK_RESULT_UNKNOWN").
const char* ResultName(VkResult result) noexcept;

// --- 플랫폼 구현(Private/Win64·Private/Android) ---

// 로더 파일 이름이다(vulkan-1.dll, libvulkan.so).
const char* LoaderLibraryName() noexcept;

// 창 표면 instance 확장 이름이다(VK_KHR_win32_surface 등).
const char* SurfaceExtensionName() noexcept;

// 네이티브 창(Windows HWND·HINSTANCE, Android ANativeWindow*)에 surface를 만든다. 확장 함수가
// 없으면 VK_ERROR_EXTENSION_NOT_PRESENT다.
VkResult CreateSurface(const Functions& functions, VkInstance instance, void* window,
                       void* instanceHandle, VkSurfaceKHR& surface) noexcept;

// PlatformBase MonotonicTicks와 같은 원천의 호스트 time domain이다(Windows QPC, Android
// CLOCK_MONOTONIC). 그 domain 값은 MonotonicTicks와 같은 단위다.
VkTimeDomainKHR HostTimeDomain() noexcept;

} // namespace rocks::graphics::vulkan
