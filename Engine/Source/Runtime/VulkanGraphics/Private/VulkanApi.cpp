#include "VulkanApi.h"

namespace rocks::graphics::vulkan {

namespace {

template <class Function>
bool LoadGlobal(PFN_vkGetInstanceProcAddr getProc, const char* name, Function& out) noexcept {
    out = reinterpret_cast<Function>(getProc(nullptr, name));
    return out != nullptr;
}

template <class Function>
bool LoadInstance(PFN_vkGetInstanceProcAddr getProc, VkInstance instance, const char* name,
                  Function& out) noexcept {
    out = reinterpret_cast<Function>(getProc(instance, name));
    return out != nullptr;
}

template <class Function>
bool LoadDevice(PFN_vkGetDeviceProcAddr getProc, VkDevice device, const char* name,
                Function& out) noexcept {
    out = reinterpret_cast<Function>(getProc(device, name));
    return out != nullptr;
}

} // namespace

Loader::~Loader() noexcept {
    lease_.Reset();
    auto closed = library_.Close();
    (void)closed;
}

platform::FileStatus Loader::Load(Functions& functions, const char*& missing) noexcept {
    missing = nullptr;
    const platform::FileStatus opened = library_.OpenSystem(LoaderLibraryName());
    if (!opened) {
        return opened;
    }

    void* address = nullptr;
    const platform::FileStatus found = library_.Find("vkGetInstanceProcAddr", address, lease_);
    if (!found) {
        missing = "vkGetInstanceProcAddr";
        return found;
    }

    functions.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(address);
#define ROCKS_VULKAN_LOAD_GLOBAL(name)                                                             \
    if (missing == nullptr &&                                                                      \
        !LoadGlobal(functions.vkGetInstanceProcAddr, #name, functions.name)) {                     \
        missing = #name;                                                                           \
    }
    ROCKS_VULKAN_GLOBAL_FUNCTIONS(ROCKS_VULKAN_LOAD_GLOBAL)
#undef ROCKS_VULKAN_LOAD_GLOBAL
    if (missing != nullptr) {
        return {platform::FileCode::NotFound, 0};
    }

    return {};
}

const char* LoadInstanceFunctions(VkInstance instance, Functions& functions) noexcept {
    const PFN_vkGetInstanceProcAddr getProc = functions.vkGetInstanceProcAddr;
    const char* missing = nullptr;
#define ROCKS_VULKAN_LOAD_INSTANCE(name)                                                           \
    if (missing == nullptr && !LoadInstance(getProc, instance, #name, functions.name)) {           \
        missing = #name;                                                                           \
    }
    ROCKS_VULKAN_INSTANCE_FUNCTIONS(ROCKS_VULKAN_LOAD_INSTANCE)
#undef ROCKS_VULKAN_LOAD_INSTANCE
#define ROCKS_VULKAN_LOAD_OPTIONAL(name)                                                           \
    (void)LoadInstance(getProc, instance, #name, functions.name);
    ROCKS_VULKAN_INSTANCE_OPTIONAL_FUNCTIONS(ROCKS_VULKAN_LOAD_OPTIONAL)
#undef ROCKS_VULKAN_LOAD_OPTIONAL
    return missing;
}

const char* LoadDeviceFunctions(VkDevice device, Functions& functions) noexcept {
    const PFN_vkGetDeviceProcAddr getProc = functions.vkGetDeviceProcAddr;
    const char* missing = nullptr;
#define ROCKS_VULKAN_LOAD_DEVICE(name)                                                             \
    if (missing == nullptr && !LoadDevice(getProc, device, #name, functions.name)) {               \
        missing = #name;                                                                           \
    }
    ROCKS_VULKAN_DEVICE_FUNCTIONS(ROCKS_VULKAN_LOAD_DEVICE)
#undef ROCKS_VULKAN_LOAD_DEVICE
#define ROCKS_VULKAN_LOAD_OPTIONAL(name) (void)LoadDevice(getProc, device, #name, functions.name);
    ROCKS_VULKAN_DEVICE_OPTIONAL_FUNCTIONS(ROCKS_VULKAN_LOAD_OPTIONAL)
#undef ROCKS_VULKAN_LOAD_OPTIONAL
    return missing;
}

const char* ResultName(VkResult result) noexcept {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    case VK_SUBOPTIMAL_KHR:
        return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
        return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
        return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL:
        return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_OUT_OF_POOL_MEMORY:
        return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_SURFACE_LOST_KHR:
        return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
        return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:
        return "VK_ERROR_OUT_OF_DATE_KHR";
    default:
        return "VK_RESULT_UNKNOWN";
    }
}

} // namespace rocks::graphics::vulkan
