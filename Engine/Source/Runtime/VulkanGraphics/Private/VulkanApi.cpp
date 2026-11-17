#include "VulkanApi.h"

#include <Core/Move.h>
#include <Platform/Sync.h>

namespace rocks::graphics::vulkan {

namespace {

// 드라이버 상주 상태다(Loader::Load 주석). 로더 참조와 ICD를 쥔 VkInstance다.
struct ResidentDrivers {
    platform::DynamicLibrary library;
    platform::DynamicLibraryLease lease;
    VkInstance instance = VK_NULL_HANDLE;
};

// 프로세스가 끝날 때까지 파괴하지 않는 저장소다. 정적 소멸자가 로더 참조를 풀면 상주가 프로세스
// 종료 직전에 깨지므로 소멸자를 부르지 않는다(OS가 프로세스와 함께 거둔다).
union ResidentStorage {
    ResidentStorage() noexcept {
    }

    ~ResidentStorage() noexcept {
    }

    ResidentDrivers drivers;
};

// 처음 부를 때 한 번만 상주를 시도한다(여러 장치가 동시에 만들어져도 gate가 하나로 줄 세운다).
void KeepDriversResident() noexcept {
    static platform::Mutex gate;
    static ResidentStorage storage;
    static bool attempted = false;
    platform::LockGuard lock(gate);
    if (attempted) {
        return;
    }

    attempted = true;
    ResidentDrivers* drivers = ConstructAt<ResidentDrivers>(&storage.drivers);
    if (!drivers->library.OpenSystem(LoaderLibraryName())) {
        return;
    }

    void* address = nullptr;
    if (!drivers->library.Find("vkGetInstanceProcAddr", address, drivers->lease)) {
        return;
    }

    const auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(address);
    const auto createInstance =
        reinterpret_cast<PFN_vkCreateInstance>(getProc(nullptr, "vkCreateInstance"));
    if (createInstance == nullptr) {
        return;
    }

    VkApplicationInfo application = {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Rocks driver residency";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;
    if (createInstance(&info, nullptr, &drivers->instance) != VK_SUCCESS) {
        drivers->instance = VK_NULL_HANDLE;
    }
}

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

    KeepDriversResident();
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
