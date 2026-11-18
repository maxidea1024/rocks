#include "VulkanDeviceImpl.h"

#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <VulkanGraphics/VulkanDevice.h>

namespace rocks::graphics::vulkan {

namespace {

constexpr char ValidationLayerName[] = "VK_LAYER_KHRONOS_validation";
// 열거 결과를 담을 고정 칸 수다(확장·layer·물리 장치·queue family).
constexpr uint32 MaxEnumerated = 512;
constexpr uint32 MaxPhysicalDevices = 16;
constexpr uint32 MaxQueueFamilies = 32;
constexpr uint32 MaxEnabledExtensions = 8;

// 정수를 10진 문자열로 붙인다.
void AppendInteger(String& text, int64 value) noexcept {
    char digits[24] = {};
    usize count = 0;
    const bool negative = value < 0;
    uint64 magnitude =
        negative ? static_cast<uint64>(-(value + 1)) + 1 : static_cast<uint64>(value);
    do {
        digits[count++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (negative) {
        digits[count++] = '-';
    }

    char ordered[24] = {};
    for (usize index = 0; index < count; ++index) {
        ordered[index] = digits[count - 1 - index];
    }

    auto appended = text.TryAppend(StringView(ordered, count));
    (void)appended;
}

void Append(String& text, StringView part) noexcept {
    auto appended = text.TryAppend(part);
    (void)appended;
}

// 고정 크기 칸에 NUL 종료로 복사한다. 넘치면 잘랐는지를 돌려준다.
bool CopyText(char* destination, usize capacity, StringView text) noexcept {
    const usize count = text.ByteCount() < capacity ? text.ByteCount() : capacity - 1;
    platform::MemoryCopy(destination, text.Data(), count);
    destination[count] = '\0';
    return count != text.ByteCount();
}

ErrorCode ResultError(VkResult result) noexcept {
    switch (result) {
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    case VK_ERROR_OUT_OF_POOL_MEMORY:
        return ErrorCode::OutOfMemory;
    case VK_ERROR_INCOMPATIBLE_DRIVER:
    case VK_ERROR_EXTENSION_NOT_PRESENT:
    case VK_ERROR_LAYER_NOT_PRESENT:
    case VK_ERROR_FEATURE_NOT_PRESENT:
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return ErrorCode::Unsupported;
    case VK_ERROR_DEVICE_LOST:
        return ErrorCode::Destroyed;
    default:
        return ErrorCode::DependencyFailed;
    }
}

bool HasExtension(const VkExtensionProperties* extensions, uint32 count,
                  const char* name) noexcept {
    for (uint32 index = 0; index < count; ++index) {
        if (StringView(extensions[index].extensionName) == StringView(name)) {
            return true;
        }
    }

    return false;
}

// 물리 장치 종류의 선택 순위다(작을수록 먼저). 외장 → 내장 → 가상 → 그 밖 → CPU.
uint32 DeviceRank(VkPhysicalDeviceType type) noexcept {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return 0;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return 1;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return 4;
    default:
        return 3;
    }
}

constexpr uint32 CpuRank = 4;

} // namespace

VulkanDevice::VulkanDevice(Allocator& allocator) noexcept
    : allocator_(allocator), adapterName_(allocator), lastError_(allocator), memory_(allocator),
      buffers_(allocator), textures_(allocator), pipelines_(allocator), swapChains_(allocator),
      readbacks_(allocator), bufferViews_(allocator), orphanBuffers_(allocator),
      retired_(allocator), renderPasses_(allocator),
      timeline_(allocator, NewDeviceEpoch(), QueueKind::Graphics), touchedTextures_(allocator),
      acquireSemaphores_(allocator), orphanSemaphores_(allocator), ring_(allocator),
      stagingBlocks_(allocator), timestamps_(allocator) {
    stagingSource_.owner = this;
    timestampSource_.owner = this;
}

VulkanDevice::~VulkanDevice() {
    // 장치는 소유 스레드(또는 소유자가 없을 때 아무 스레드)에서 파괴한다.
    if (!ownership_.IsOwner() && !ownership_.Acquire()) {
        platform::Fatal("graphics device destroyed while another thread owns it");
    }

    if (device_ != VK_NULL_HANDLE && queue_ != VK_NULL_HANDLE) {
        platform::LockGuard lock(gate_);
        if (recording_) {
            // 제출하지 않은 프레임 기록을 버린다(명령 버퍼는 pool과 함께 파괴된다).
            recording_ = false;
            insideRenderPass_ = false;
            AssignPendingRetires(fenceValue_);
        }

        auto idle = WaitIdleLocked();
        (void)idle;
        // present의 semaphore 대기까지 끝낸다(fence는 present를 덮지 않는다).
        (void)functions_.vkDeviceWaitIdle(device_);
    }

    // 모든 제출이 끝났거나 장치가 손실되어 관찰 스레드가 곧 빠져나온다.
    timeline_.StopWatcher();
    timeline_.Close();
    platform::LockGuard lock(gate_);
    if (staging_) {
        staging_->Clear();
        staging_ = UniquePtr<StagingPool>();
    }

    if (device_ != VK_NULL_HANDLE) {
        for (ReadbackRecord& record : readbacks_) {
            functions_.vkDestroyBuffer(device_, record.buffer, nullptr);
            FreeMemoryLocked(record.memory);
        }

        readbacks_.Clear();
        for (Retired& entry : retired_) {
            DestroyRetired(entry);
        }

        retired_.Clear();
        for (SwapChainRecord& record : swapChains_) {
            DestroySwapChainTarget(record);
            DestroySwapChainImages(record, false);
            if (record.surface != VK_NULL_HANDLE) {
                functions_.vkDestroySurfaceKHR(instance_, record.surface, nullptr);
            }
        }

        swapChains_.Clear();
        for (TextureRecord& record : textures_) {
            if (record.framebuffer != VK_NULL_HANDLE) {
                functions_.vkDestroyFramebuffer(device_, record.framebuffer, nullptr);
            }

            functions_.vkDestroyImageView(device_, record.view, nullptr);
            // view는 원 텍스처의 이미지를 공유하므로 원 기록만 이미지·메모리를 놓는다.
            if (!record.isView) {
                functions_.vkDestroyImage(device_, record.image, nullptr);
                FreeMemoryLocked(record.memory);
            }
        }

        textures_.Clear();
        for (BufferViewRecord& record : bufferViews_) {
            if (record.view != VK_NULL_HANDLE) {
                functions_.vkDestroyBufferView(device_, record.view, nullptr);
            }
        }

        bufferViews_.Clear();
        for (OrphanBuffer& orphan : orphanBuffers_) {
            functions_.vkDestroyBuffer(device_, orphan.buffer, nullptr);
            FreeMemoryLocked(orphan.memory);
        }

        orphanBuffers_.Clear();
        for (BufferRecord& record : buffers_) {
            functions_.vkDestroyBuffer(device_, record.buffer, nullptr);
            FreeMemoryLocked(record.memory);
        }

        buffers_.Clear();
        for (PipelineRecord& record : pipelines_) {
            functions_.vkDestroyPipeline(device_, record.pipeline, nullptr);
            DestroyPipelineLayout(record);
        }

        pipelines_.Clear();
        for (AcquireSemaphore& entry : acquireSemaphores_) {
            functions_.vkDestroySemaphore(device_, entry.semaphore, nullptr);
        }

        acquireSemaphores_.Clear();
        for (VkSemaphore semaphore : orphanSemaphores_) {
            functions_.vkDestroySemaphore(device_, semaphore, nullptr);
        }

        orphanSemaphores_.Clear();
        for (uint32 index = 0; index < samplerCount_; ++index) {
            functions_.vkDestroySampler(device_, samplers_[index].sampler, nullptr);
        }

        samplerCount_ = 0;
        DestroyRecordPools();
        for (RenderPassEntry& entry : renderPasses_) {
            functions_.vkDestroyRenderPass(device_, entry.renderPass, nullptr);
        }

        renderPasses_.Clear();
        if (pipelineLayout_ != VK_NULL_HANDLE) {
            functions_.vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        }

        if (emptySetLayout_ != VK_NULL_HANDLE) {
            functions_.vkDestroyDescriptorSetLayout(device_, emptySetLayout_, nullptr);
        }

        if (pipelineCache_ != VK_NULL_HANDLE) {
            functions_.vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
        }

        if (queryPool_ != VK_NULL_HANDLE) {
            functions_.vkDestroyQueryPool(device_, queryPool_, nullptr);
        }

        if (ringBuffer_ != VK_NULL_HANDLE) {
            functions_.vkDestroyBuffer(device_, ringBuffer_, nullptr);
        }

        FreeMemoryLocked(ringMemory_);
        DestroySubmissionObjects();
        {
            platform::LockGuard memoryLock(memoryGate_);
            memory_.Clear();
        }

        functions_.vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    if (messenger_ != VK_NULL_HANDLE) {
        functions_.vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    }

    // instance 함수 표를 못 채운 실패(LoadInstanceFunctions)에서는 파괴 함수도 없을 수 있다.
    if (instance_ != VK_NULL_HANDLE && functions_.vkDestroyInstance != nullptr) {
        functions_.vkDestroyInstance(instance_, nullptr);
    }
}

Result<void> VulkanDevice::Fail(const char* stage, VkResult result) noexcept {
    if (result == VK_ERROR_DEVICE_LOST) {
        DetectLost(result);
    }

    lastError_.Clear();
    Append(lastError_, StringView(stage));
    Append(lastError_, StringView(" failed: "));
    Append(lastError_, StringView(ResultName(result)));
    Append(lastError_, StringView(" ("));
    AppendInteger(lastError_, static_cast<int64>(result));
    Append(lastError_, StringView(")"));
    const ErrorCode code = timeline_.IsLost() ? ErrorCode::Destroyed : ResultError(result);
    if (report_ != nullptr) {
        report_->code = code;
        report_->nativeCode = static_cast<int64>(result);
        const bool stageCut = CopyText(report_->stage, sizeof(report_->stage), StringView(stage));
        const bool detailCut =
            CopyText(report_->detail, sizeof(report_->detail), lastError_.View());
        report_->truncated = stageCut || detailCut;
    }

    return Result<void>::Failure(code);
}

Result<void> VulkanDevice::FailCode(const char* stage, ErrorCode code,
                                    const char* detail) noexcept {
    lastError_.Clear();
    Append(lastError_, StringView(stage));
    Append(lastError_, StringView(": "));
    Append(lastError_, StringView(detail));
    if (report_ != nullptr) {
        report_->code = code;
        report_->nativeCode = 0;
        const bool stageCut = CopyText(report_->stage, sizeof(report_->stage), StringView(stage));
        const bool detailCut =
            CopyText(report_->detail, sizeof(report_->detail), lastError_.View());
        report_->truncated = stageCut || detailCut;
    }

    return Result<void>::Failure(code);
}

void VulkanDevice::DetectLost(VkResult result) noexcept {
    if (result == VK_ERROR_DEVICE_LOST && !timeline_.IsLost()) {
        timeline_.MarkLost();
        Log::Error(LogGraphicsVulkan, "Vulkan device lost on {Adapter}", adapterName_.CStr());
    }
}

Result<void> VulkanDevice::Initialize(const DeviceDesc& desc, DeviceCreateReport* report) noexcept {
    report_ = report;
    shaderCompiler_ = desc.shaderCompiler;
    const char* missing = nullptr;
    const platform::FileStatus opened = loader_.Load(functions_, missing);
    if (!opened) {
        lastError_.Clear();
        Append(lastError_, StringView("Vulkan loader "));
        Append(lastError_, StringView(LoaderLibraryName()));
        Append(lastError_, StringView(missing != nullptr ? " lacks a required entry point"
                                                         : " could not be opened"));
        if (report_ != nullptr) {
            report_->code = ErrorCode::Unsupported;
            report_->nativeCode = static_cast<int64>(opened.nativeError);
            CopyText(report_->stage, sizeof(report_->stage),
                     StringView(missing != nullptr ? missing : "LoadVulkanLoader"));
            CopyText(report_->detail, sizeof(report_->detail), lastError_.View());
        }

        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    auto instance = CreateInstance(desc);
    if (!instance) {
        return instance;
    }

    auto picked = PickPhysicalDevice(desc);
    if (!picked) {
        return picked;
    }

    auto device = CreateLogicalDevice();
    if (!device) {
        return device;
    }

    // 형식 능력은 켠 기능(Y′CbCr·ASTC HDR)에 따라 달라지므로 논리 장치를 만든 뒤 묻는다. 장치
    // 능력(GX1)도 켠 기능 기준이다.
    QueryFormatCaps();
    QueryDeviceCaps();

    auto shared = CreateSharedObjects(desc);
    if (!shared) {
        return shared;
    }

    report_ = nullptr;
    Log::Info(LogGraphicsVulkan, "Created {Backend} device on {Adapter} (validation {Validation})",
              "Vulkan", adapterName_.CStr(), validation_ ? "on" : "off");
    return Result<void>::Success();
}

Result<void> VulkanDevice::CreateInstance(const DeviceDesc& desc) noexcept {
    uint32 version = 0;
    VkResult result = functions_.vkEnumerateInstanceVersion(&version);
    if (result != VK_SUCCESS || version < VK_API_VERSION_1_1) {
        return FailCode("vkEnumerateInstanceVersion", ErrorCode::Unsupported,
                        "the Vulkan loader does not provide Vulkan 1.1");
    }

    VkExtensionProperties extensions[MaxEnumerated];
    uint32 extensionCount = MaxEnumerated;
    result =
        functions_.vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return Fail("vkEnumerateInstanceExtensionProperties", result);
    }

    const char* enabled[MaxEnabledExtensions] = {};
    uint32 enabledCount = 0;
    surfaceExtension_ = HasExtension(extensions, extensionCount, VK_KHR_SURFACE_EXTENSION_NAME) &&
                        HasExtension(extensions, extensionCount, SurfaceExtensionName());
    if (surfaceExtension_) {
        enabled[enabledCount++] = VK_KHR_SURFACE_EXTENSION_NAME;
        enabled[enabledCount++] = SurfaceExtensionName();
    }

    debugUtils_ = HasExtension(extensions, extensionCount, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debugUtils_) {
        enabled[enabledCount++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }

    const char* layers[1] = {};
    uint32 layerCount = 0;
    if (desc.enableValidation) {
        VkLayerProperties available[MaxEnumerated / 4];
        uint32 availableCount = MaxEnumerated / 4;
        result = functions_.vkEnumerateInstanceLayerProperties(&availableCount, available);
        for (uint32 index = 0;
             (result == VK_SUCCESS || result == VK_INCOMPLETE) && index < availableCount; ++index) {
            if (StringView(available[index].layerName) == StringView(ValidationLayerName)) {
                layers[layerCount++] = ValidationLayerName;
                break;
            }
        }

        if (layerCount == 0) {
            Log::Warning(LogGraphicsVulkan,
                         "Vulkan validation requested but {Layer} is not installed; continuing "
                         "without it",
                         ValidationLayerName);
        }
    }

    validation_ = layerCount != 0 && debugUtils_;
    VkApplicationInfo application = {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Rocks";
    application.pEngineName = "Rocks";
    application.apiVersion = VK_API_VERSION_1_1;
    // instance 생성·파괴 중 메시지도 센다(VK_EXT_debug_utils "pNext of VkInstanceCreateInfo").
    VkDebugUtilsMessengerCreateInfoEXT messenger = {};
    messenger.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    messenger.pfnUserCallback = &VulkanDevice::OnValidationMessage;
    messenger.pUserData = &validationState_;
    VkInstanceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pNext = validation_ ? &messenger : nullptr;
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = enabledCount;
    info.ppEnabledExtensionNames = enabled;
    info.enabledLayerCount = layerCount;
    info.ppEnabledLayerNames = layers;
    result = functions_.vkCreateInstance(&info, nullptr, &instance_);
    if (result != VK_SUCCESS) {
        instance_ = VK_NULL_HANDLE;
        return Fail("vkCreateInstance", result);
    }

    const char* missingFunction = LoadInstanceFunctions(instance_, functions_);
    if (missingFunction != nullptr) {
        return FailCode(missingFunction, ErrorCode::DependencyFailed,
                        "required Vulkan instance function is missing");
    }

    if (validation_) {
        return CreateMessenger();
    }

    return Result<void>::Success();
}

Result<void> VulkanDevice::PickPhysicalDevice(const DeviceDesc& desc) noexcept {
    VkPhysicalDevice devices[MaxPhysicalDevices];
    uint32 count = MaxPhysicalDevices;
    const VkResult result = functions_.vkEnumeratePhysicalDevices(instance_, &count, devices);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return Fail("vkEnumeratePhysicalDevices", result);
    }

    uint32 bestRank = NumericLimits<uint32>::Max();
    for (uint32 index = 0; index < count; ++index) {
        VkPhysicalDeviceProperties properties = {};
        functions_.vkGetPhysicalDeviceProperties(devices[index], &properties);
        if (properties.apiVersion < VK_API_VERSION_1_1) {
            continue;
        }

        VkQueueFamilyProperties families[MaxQueueFamilies];
        // 수를 먼저 묻고 칸 수로 자른다(두 번 부르기 규약).
        uint32 familyCount = 0;
        functions_.vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &familyCount, nullptr);
        familyCount = familyCount < MaxQueueFamilies ? familyCount : MaxQueueFamilies;
        functions_.vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &familyCount, families);
        uint32 family = NumericLimits<uint32>::Max();
        for (uint32 candidate = 0; candidate < familyCount; ++candidate) {
            if ((families[candidate].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                family = candidate;
                break;
            }
        }

        const uint32 rank = DeviceRank(properties.deviceType);
        if (family == NumericLimits<uint32>::Max() || (desc.preferSoftware && rank != CpuRank) ||
            rank >= bestRank) {
            continue;
        }

        bestRank = rank;
        physical_ = devices[index];
        properties_ = properties;
        queueFamily_ = family;
    }

    if (physical_ == VK_NULL_HANDLE) {
        return FailCode("vkEnumeratePhysicalDevices", ErrorCode::Unsupported,
                        desc.preferSoftware
                            ? "no CPU (software) Vulkan 1.1 device with a graphics queue"
                            : "no Vulkan 1.1 device with a graphics queue");
    }

    software_ = bestRank == CpuRank;
    auto named = adapterName_.TryAssign(StringView(properties_.deviceName));
    (void)named;
    return Result<void>::Success();
}

Result<void> VulkanDevice::CreateLogicalDevice() noexcept {
    VkExtensionProperties extensions[MaxEnumerated];
    uint32 extensionCount = MaxEnumerated;
    VkResult result = functions_.vkEnumerateDeviceExtensionProperties(physical_, nullptr,
                                                                      &extensionCount, extensions);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return Fail("vkEnumerateDeviceExtensionProperties", result);
    }

    const char* enabled[MaxEnabledExtensions] = {};
    uint32 enabledCount = 0;
    swapchainExtension_ = surfaceExtension_ &&
                          HasExtension(extensions, extensionCount, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (swapchainExtension_) {
        enabled[enabledCount++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    }

    const bool calibratedKhr =
        HasExtension(extensions, extensionCount, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    const bool calibratedExt =
        HasExtension(extensions, extensionCount, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    if (calibratedKhr) {
        enabled[enabledCount++] = VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    } else if (calibratedExt) {
        enabled[enabledCount++] = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    }

    // ASTC HDR 형식(VK_FORMAT_ASTC_*_SFLOAT_BLOCK)은 확장 형식이다. 확장과 기능을 켜지 않으면 그
    // 형식을 묻거나 쓸 수 없다(PixelFormat.ko.md 14.2절).
    const bool astcHdrExtension = HasExtension(extensions, extensionCount,
                                               VK_EXT_TEXTURE_COMPRESSION_ASTC_HDR_EXTENSION_NAME);
    // multi-planar 이미지(NV12·P010)를 만들려면 samplerYcbcrConversion 기능을 켜야 한다(Vulkan 1.1
    // core 기능, 지원하면 켠다). plane view만 쓰므로 Y′CbCr 변환 객체는 만들지 않는다.
    // 64 bit atomic은 버퍼(VK_KHR_shader_atomic_int64)와 이미지(VK_EXT_shader_image_atomic_int64)가
    // 모두 있을 때만 켠다(UE VulkanExtensions.cpp:536과 같은 조건). VS 층 출력은
    // VK_EXT_shader_viewport_index_layer다(UE :560). 확장이 없으면 그 기능 구조를 묻지 않는다.
    const bool atomicInt64Extension =
        HasExtension(extensions, extensionCount, VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME);
    const bool imageAtomicInt64Extension =
        HasExtension(extensions, extensionCount, VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME);
    viewportLayer_ =
        HasExtension(extensions, extensionCount, VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME);
    if (viewportLayer_) {
        enabled[enabledCount++] = VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME;
    }

    VkPhysicalDeviceShaderAtomicInt64Features atomicSupported = {};
    atomicSupported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES;
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT imageAtomicSupported = {};
    imageAtomicSupported.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT;
    const bool atomicQuery = atomicInt64Extension && imageAtomicInt64Extension;
    atomicSupported.pNext = &imageAtomicSupported;
    VkPhysicalDeviceTextureCompressionASTCHDRFeatures astcHdrSupported = {};
    astcHdrSupported.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES;
    astcHdrSupported.pNext = atomicQuery ? &atomicSupported : nullptr;
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrSupported = {};
    ycbcrSupported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    ycbcrSupported.pNext = astcHdrExtension ? static_cast<void*>(&astcHdrSupported)
                           : atomicQuery    ? static_cast<void*>(&atomicSupported)
                                            : nullptr;
    VkPhysicalDeviceFeatures2 supported2 = {};
    supported2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supported2.pNext = &ycbcrSupported;
    functions_.vkGetPhysicalDeviceFeatures2(physical_, &supported2);
    const VkPhysicalDeviceFeatures& supported = supported2.features;
    ycbcr_ = ycbcrSupported.samplerYcbcrConversion == VK_TRUE;
    astcHdr_ = astcHdrExtension && astcHdrSupported.textureCompressionASTC_HDR == VK_TRUE;
    if (astcHdr_) {
        enabled[enabledCount++] = VK_EXT_TEXTURE_COMPRESSION_ASTC_HDR_EXTENSION_NAME;
    }

    atomic64_ = atomicQuery && atomicSupported.shaderBufferInt64Atomics == VK_TRUE &&
                imageAtomicSupported.shaderImageInt64Atomics == VK_TRUE;
    if (atomic64_) {
        enabled[enabledCount++] = VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME;
        enabled[enabledCount++] = VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME;
    }

    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT imageAtomicEnabled = {};
    imageAtomicEnabled.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT;
    imageAtomicEnabled.shaderImageInt64Atomics = VK_TRUE;
    VkPhysicalDeviceShaderAtomicInt64Features atomicEnabled = {};
    atomicEnabled.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES;
    atomicEnabled.shaderBufferInt64Atomics = VK_TRUE;
    atomicEnabled.pNext = &imageAtomicEnabled;
    VkPhysicalDeviceTextureCompressionASTCHDRFeatures astcHdrEnabled = {};
    astcHdrEnabled.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES;
    astcHdrEnabled.textureCompressionASTC_HDR = VK_TRUE;
    astcHdrEnabled.pNext = atomic64_ ? &atomicEnabled : nullptr;
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrEnabled = {};
    ycbcrEnabled.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    ycbcrEnabled.samplerYcbcrConversion = ycbcr_ ? VK_TRUE : VK_FALSE;
    ycbcrEnabled.pNext = astcHdr_    ? static_cast<void*>(&astcHdrEnabled)
                         : atomic64_ ? static_cast<void*>(&atomicEnabled)
                                     : nullptr;
    VkPhysicalDeviceFeatures features = {};
    anisotropy_ = supported.samplerAnisotropy == VK_TRUE;
    gather_ = supported.shaderImageGatherExtended == VK_TRUE;
    features.samplerAnisotropy = anisotropy_ ? VK_TRUE : VK_FALSE;
    // 블록 압축 묶음 기능은 지원하면 켠다. 형식마다의 지원은 형식 능력 질의가 판정한다.
    features.textureCompressionBC = supported.textureCompressionBC;
    features.textureCompressionETC2 = supported.textureCompressionETC2;
    features.textureCompressionASTC_LDR = supported.textureCompressionASTC_LDR;
    // 장치 능력(GX1)으로 공개하는 기능은 지원하면 켠다(UE도 지원하는 core 기능을 켠다).
    features.depthClamp = supported.depthClamp;
    features.multiDrawIndirect = supported.multiDrawIndirect;
    features.drawIndirectFirstInstance = supported.drawIndirectFirstInstance;
    features.imageCubeArray = supported.imageCubeArray;
    features.vertexPipelineStoresAndAtomics = supported.vertexPipelineStoresAndAtomics;
    features.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
    features.shaderInt64 = atomic64_ ? supported.shaderInt64 : VK_FALSE;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = queueFamily_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = enabledCount;
    info.ppEnabledExtensionNames = enabled;
    info.pEnabledFeatures = &features;
    info.pNext = &ycbcrEnabled;
    result = functions_.vkCreateDevice(physical_, &info, nullptr, &device_);
    if (result != VK_SUCCESS) {
        device_ = VK_NULL_HANDLE;
        return Fail("vkCreateDevice", result);
    }

    enabledFeatures_ = features;

    const char* missingFunction = LoadDeviceFunctions(device_, functions_);
    if (missingFunction != nullptr) {
        // 함수 표가 비어 있으므로 소멸자가 장치를 파괴할 수 없다. 여기서 바로 놓는다.
        const auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(
            functions_.vkGetDeviceProcAddr(device_, "vkDestroyDevice"));
        if (destroy != nullptr) {
            destroy(device_, nullptr);
        }

        device_ = VK_NULL_HANDLE;
        return FailCode(missingFunction, ErrorCode::DependencyFailed,
                        "required Vulkan device function is missing");
    }

    functions_.vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    if (calibratedKhr || calibratedExt) {
        getCalibratedTimestamps_ = calibratedKhr
                                       ? functions_.vkGetCalibratedTimestampsKHR
                                       : reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(
                                             functions_.vkGetCalibratedTimestampsEXT);
    }

    VkPhysicalDeviceMemoryProperties memory = {};
    functions_.vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
    platform::LockGuard memoryLock(memoryGate_);
    memory_.Initialize(functions_, device_, memory, properties_.limits.nonCoherentAtomSize);
    return Result<void>::Success();
}

Result<void> VulkanDevice::CreateSharedObjects(const DeviceDesc& desc) noexcept {
    {
        platform::LockGuard lock(gate_);
        auto pool = CreateCommandPool();
        if (!pool) {
            return pool;
        }

        watcher_.functions = &functions_;
        watcher_.device = device_;
        auto watching = timeline_.StartWatcher(watcher_, "VulkanFence");
        if (!watching) {
            return FailCode("StartWatcher", watching.Error(),
                            "fence watcher thread creation failed");
        }

        auto layout = CreatePipelineLayout();
        if (!layout) {
            return layout;
        }

        auto ring = CreateUploadRing(desc);
        if (!ring) {
            return ring;
        }

        const uint64 stagingBudget =
            desc.stagingBudgetBytes != 0 ? desc.stagingBudgetBytes : DefaultStagingBudgetBytes;
        auto staging =
            UniquePtr<StagingPool>::TryMake(allocator_, allocator_, stagingSource_, stagingBudget);
        if (!staging) {
            return Result<void>::Failure(staging.Error());
        }

        staging_ = Move(staging.Value());
        InitializeTimestamps(queueFamily_);
        InitializeCalibration();
    }

    // 텍스처를 묶지 않은 그리기가 쓸 1x1 흰색 텍스처다(CreateTexture가 장치 잠금을 잡는다).
    const uint8 white[4] = {255, 255, 255, 255};
    TextureDesc whiteDesc;
    whiteDesc.width = 1;
    whiteDesc.height = 1;
    whiteDesc.format = PixelFormat::RGBA8Unorm;
    auto whiteTexture = CreateTexture(whiteDesc, Span<const uint8>(white, 4));
    if (!whiteTexture) {
        return Result<void>::Failure(whiteTexture.Error());
    }

    white_ = whiteTexture.Value();
    return Result<void>::Success();
}

DeviceInfo VulkanDevice::Info() const noexcept {
    DeviceInfo info;
    info.backend = Backend::Vulkan;
    info.adapterName = adapterName_.View();
    info.deviceEpoch = timeline_.DeviceEpoch();
    info.validationEnabled = validation_;
    info.software = software_;
    // 생성은 Vulkan 객체 생성(스레드 안전)과 내부 잠금 아래 업로드 기록이다.
    info.concurrentResourceCreation = true;
    info.timestamps = queryPool_ != VK_NULL_HANDLE;
    info.timestampFrequency = timestampFrequency_;
    // Vulkan timestamp에는 구간 끊김 표시가 없다.
    info.timestampDisjointDetection = false;
    info.clockCalibration = info.timestamps && getCalibratedTimestamps_ != nullptr;
    info.debugMarkers = debugUtils_ && functions_.vkCmdBeginDebugUtilsLabelEXT != nullptr &&
                        functions_.vkCmdEndDebugUtilsLabelEXT != nullptr;
    info.apiVersion = properties_.apiVersion;
    return info;
}

GpuFencePoint VulkanDevice::LastSubmittedFence() const noexcept {
    return timeline_.LastSubmitted();
}

Result<bool> VulkanDevice::IsFenceComplete(GpuFencePoint point) const noexcept {
    return timeline_.IsComplete(point);
}

Result<platform::WaitResult> VulkanDevice::WaitFence(GpuFencePoint point,
                                                     uint32 timeoutMs) noexcept {
    return timeline_.Wait(point, timeoutMs);
}

Result<void> VulkanDevice::NotifyFence(GpuFencePoint point, FenceCallback callback,
                                       void* context) noexcept {
    return timeline_.Notify(point, callback, context);
}

bool VulkanDevice::IsLost() const noexcept {
    return timeline_.IsLost();
}

bool VulkanDevice::IsOwnerThread() const noexcept {
    return ownership_.IsOwner();
}

Result<void> VulkanDevice::ReleaseOwnership() noexcept {
    if (!ownership_.IsOwner()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (recording_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    return ownership_.Release();
}

Result<void> VulkanDevice::AcquireOwnership() noexcept {
    return ownership_.Acquire();
}

uint64 VulkanDevice::RejectedCalls() const noexcept {
    return ownership_.Rejected();
}

StringView VulkanDevice::LastError() const noexcept {
    return lastError_.View();
}

uint64 VulkanDevice::IgnoredDraws() const noexcept {
    return ignoredDraws_;
}

uint64 VulkanDevice::ValidationErrorCount() const noexcept {
    return platform::AtomicLoad<platform::MemoryOrder::Acquire>(&validationState_.errors);
}

usize VulkanDevice::PendingRetireCount() const noexcept {
    platform::LockGuard lock(gate_);
    return retired_.Size();
}

} // namespace rocks::graphics::vulkan

namespace rocks::graphics {

Result<UniquePtr<Device>> CreateVulkanDevice(Allocator& allocator, const DeviceDesc& desc,
                                             DeviceCreateReport* report) noexcept {
    if (report != nullptr) {
        *report = {};
    }

    auto created = UniquePtr<vulkan::VulkanDevice>::TryMake(allocator, allocator);
    if (!created) {
        if (report != nullptr) {
            report->code = created.Error();
        }

        return Result<UniquePtr<Device>>::Failure(created.Error());
    }

    auto initialized = created.Value()->Initialize(desc, report);
    if (!initialized) {
        return Result<UniquePtr<Device>>::Failure(initialized.Error());
    }

    return Result<UniquePtr<Device>>::Success(UniquePtr<Device>(Move(created.Value())));
}

} // namespace rocks::graphics
