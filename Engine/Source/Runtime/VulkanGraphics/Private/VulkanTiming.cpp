#include "VulkanDeviceImpl.h"

#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Memory.h>

// GPU 계측(GX9)·debug label·검증 messenger다(Docs/Design/VulkanGraphics.ko.md 12절).
namespace rocks::graphics::vulkan {

namespace {

constexpr uint32 MaxTimeDomains = 8;
constexpr uint32 VulkanTimingMaxQueueFamilies = 32;
constexpr double NanosecondsPerSecond = 1e9;
constexpr uint32 FullTimestampBits = 64;

} // namespace

TimestampReadState TimestampSourceImpl::ReadTimestamps(uint32 first, uint32 count,
                                                       uint64* ticks) noexcept {
    return owner->ReadTimestampQueries(first, count, ticks);
}

TimestampReadState VulkanDevice::ReadTimestampQueries(uint32 first, uint32 count,
                                                      uint64* ticks) noexcept {
    // 손실 뒤에는 fence가 끝난 것처럼 보여도 값이 없다.
    if (timeline_.IsLost()) {
        return TimestampReadState::Disjoint;
    }

    // WAIT 없이 읽는다(fence 완료 뒤에만 부르므로 보통 준비돼 있다, UE도 WAIT를 쓰지 않는다).
    const VkResult result = functions_.vkGetQueryPoolResults(
        device_, queryPool_, first, count, static_cast<usize>(count) * sizeof(uint64), ticks,
        sizeof(uint64), VK_QUERY_RESULT_64_BIT);
    if (result == VK_NOT_READY) {
        return TimestampReadState::NotReady;
    }

    if (result != VK_SUCCESS) {
        DetectLost(result);
        return TimestampReadState::Disjoint;
    }

    for (uint32 index = 0; index < count; ++index) {
        ticks[index] &= timestampMask_;
    }

    return TimestampReadState::Ready;
}

void VulkanDevice::InitializeTimestamps(uint32 queueFamily) noexcept {
    VkQueueFamilyProperties families[VulkanTimingMaxQueueFamilies];
    uint32 familyCount = 0;
    functions_.vkGetPhysicalDeviceQueueFamilyProperties(physical_, &familyCount, nullptr);
    familyCount =
        familyCount < VulkanTimingMaxQueueFamilies ? familyCount : VulkanTimingMaxQueueFamilies;
    functions_.vkGetPhysicalDeviceQueueFamilyProperties(physical_, &familyCount, families);
    const float period = properties_.limits.timestampPeriod;
    if (queueFamily >= familyCount || families[queueFamily].timestampValidBits == 0 ||
        period <= 0.0f) {
        // timestamp를 "없음"으로 공개한다(추정값을 만들지 않는다).
        return;
    }

    constexpr uint32 Count = TimestampFrameSlots * MaxTimestampsPerFrame;
    VkQueryPoolCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = Count;
    VkQueryPool pool = VK_NULL_HANDLE;
    if (functions_.vkCreateQueryPool(device_, &info, nullptr, &pool) != VK_SUCCESS) {
        return;
    }

    if (!timestamps_.Reset(TimestampFrameSlots, MaxTimestampsPerFrame)) {
        functions_.vkDestroyQueryPool(device_, pool, nullptr);
        return;
    }

    queryPool_ = pool;
    const uint32 bits = families[queueFamily].timestampValidBits;
    timestampMask_ =
        bits >= FullTimestampBits ? NumericLimits<uint64>::Max() : (uint64{1} << bits) - 1;
    // DeviceInfo::timestampFrequency는 정수 Hz다. period가 정수 ns가 아니면 반올림만큼 변환 오차가
    // 있다(VulkanGraphics 12절).
    timestampFrequency_ = static_cast<uint64>(NanosecondsPerSecond / period + 0.5);
    // 처음에는 모든 slot이 초기화 전이다(첫 기록의 render pass 앞에서 초기화한다).
    dirtySlots_ = (1u << TimestampFrameSlots) - 1;
}

void VulkanDevice::InitializeCalibration() noexcept {
    if (getCalibratedTimestamps_ == nullptr || queryPool_ == VK_NULL_HANDLE) {
        getCalibratedTimestamps_ = nullptr;
        return;
    }

    const auto domainsOf =
        functions_.vkGetPhysicalDeviceCalibrateableTimeDomainsKHR != nullptr
            ? functions_.vkGetPhysicalDeviceCalibrateableTimeDomainsKHR
            : reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(
                  functions_.vkGetPhysicalDeviceCalibrateableTimeDomainsEXT);
    VkTimeDomainKHR domains[MaxTimeDomains] = {};
    uint32 count = MaxTimeDomains;
    if (domainsOf == nullptr ||
        (domainsOf(physical_, &count, domains) != VK_SUCCESS && count != MaxTimeDomains)) {
        getCalibratedTimestamps_ = nullptr;
        return;
    }

    // 장치와 PlatformBase MonotonicTicks 원천(Windows QPC, Android CLOCK_MONOTONIC)이 모두 있어야
    // 같은 순간의 쌍이 CPU trace 시간축에 놓인다. 없으면 calibration을 "없음"으로 공개한다.
    bool device = false;
    bool host = false;
    for (uint32 index = 0; index < count; ++index) {
        device = device || domains[index] == VK_TIME_DOMAIN_DEVICE_KHR;
        host = host || domains[index] == HostTimeDomain();
    }

    if (!device || !host) {
        getCalibratedTimestamps_ = nullptr;
    }
}

Result<void> VulkanDevice::CreateMessenger() noexcept {
    VkDebugUtilsMessengerCreateInfoEXT info = {};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    info.pfnUserCallback = &VulkanDevice::OnValidationMessage;
    info.pUserData = &validationState_;
    if (functions_.vkCreateDebugUtilsMessengerEXT == nullptr) {
        return FailCode("vkCreateDebugUtilsMessengerEXT", ErrorCode::DependencyFailed,
                        "debug utils messenger function is missing");
    }

    const VkResult result =
        functions_.vkCreateDebugUtilsMessengerEXT(instance_, &info, nullptr, &messenger_);
    if (result != VK_SUCCESS) {
        messenger_ = VK_NULL_HANDLE;
        return Fail("vkCreateDebugUtilsMessengerEXT", result);
    }

    return Result<void>::Success();
}

// 드라이버·층 스레드에서도 불린다. 잠금 없이 원자 연산과 고정 크기 ID 표만 쓴다(여러 스레드가
// 부르므로 직렬화를 전제하는 ValidationMessageFilter 대신 같은 규칙을 원자 연산으로 지킨다). ERROR
// 심각도를 세고(ValidationErrorCount), 메시지 ID마다 한 번 Warning 로그를 남긴다(LoggingSystem
// 13절). 표(MaxLoggedValidationIds)가 차면 새 ID는 남기지 않는다(개수는 계속 센다).
VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDevice::OnValidationMessage(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto* state = static_cast<ValidationState*>(user);
    if (state == nullptr || data == nullptr) {
        return VK_FALSE;
    }

    // 검증 오류 계수는 validation 메시지 종류만 센다. 로더·드라이버의 GENERAL 메시지(예:
    // 레지스트리에 남은 implicit layer JSON을 열지 못함)는 API 사용 오류가 아니므로 로그로만
    // 남긴다.
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0 &&
        (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0) {
        platform::AtomicFetchAdd<platform::MemoryOrder::AcquireRelease>(&state->errors, uint64{1});
    }

    const int32 id = data->messageIdNumber;
    const uint32 logged = platform::AtomicLoad<platform::MemoryOrder::Acquire>(&state->loggedCount);
    for (uint32 index = 0; index < logged && index < MaxLoggedValidationIds; ++index) {
        if (platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&state->loggedIds[index]) == id) {
            return VK_FALSE;
        }
    }

    const uint32 slot =
        platform::AtomicFetchAdd<platform::MemoryOrder::AcquireRelease>(&state->loggedCount, 1u);
    if (slot >= MaxLoggedValidationIds) {
        return VK_FALSE;
    }

    platform::AtomicStore<platform::MemoryOrder::Relaxed>(&state->loggedIds[slot], id);
    Log::Warning(LogGraphicsVulkanValidation, "{MessageId}: {Message}",
                 data->pMessageIdName != nullptr ? data->pMessageIdName : "",
                 data->pMessage != nullptr ? data->pMessage : "");
    return VK_FALSE;
}

void VulkanDevice::HarvestTimestamps() noexcept {
    if (queryPool_ == VK_NULL_HANDLE) {
        return;
    }

    PollCompleted();
    timestamps_.Harvest(CompletedValue(), timestampSource_);
}

void VulkanDevice::CloseMarkers() noexcept {
    for (; markerDepth_ != 0; --markerDepth_) {
        functions_.vkCmdEndDebugUtilsLabelEXT(frame_);
    }
}

usize VulkanDevice::CollectTimestamps(Span<TimestampSample> out) noexcept {
    if (!ownership_.IsOwner()) {
        return 0;
    }

    platform::LockGuard lock(gate_);
    if (queryPool_ == VK_NULL_HANDLE) {
        return 0;
    }

    // 기록 중에는 회수하지 않는다. 회수한 slot은 다음 기록의 render pass 앞에서 초기화되므로, 기록
    // 중에 비우면 초기화 없이 다시 쓰일 수 있다(남은 결과는 다음 호출로 간다).
    if (!recording_) {
        HarvestTimestamps();
    }

    return timestamps_.Collect(out);
}

Result<ClockCalibration> VulkanDevice::CalibrateClocks(QueueKind queue) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<ClockCalibration>::Failure(owner.Error());
    }

    if (queue != QueueKind::Graphics) {
        return Result<ClockCalibration>::Failure(ErrorCode::Unsupported);
    }

    if (queryPool_ == VK_NULL_HANDLE || getCalibratedTimestamps_ == nullptr) {
        return Result<ClockCalibration>::Failure(ErrorCode::Unsupported);
    }

    if (timeline_.IsLost()) {
        return Result<ClockCalibration>::Failure(ErrorCode::Destroyed);
    }

    VkCalibratedTimestampInfoKHR infos[2] = {};
    infos[0].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
    infos[0].timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
    infos[1].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
    infos[1].timeDomain = HostTimeDomain();
    // Vulkan 함수는 uint64_t를 받는다(LP64 Android에서는 unsigned long이라 Rocks uint64와 다른
    // 타입이다).
    uint64_t values[2] = {};
    uint64_t deviation = 0;
    const VkResult result = getCalibratedTimestamps_(device_, 2, infos, values, &deviation);
    if (result != VK_SUCCESS) {
        platform::LockGuard lock(gate_);
        return FailWith<ClockCalibration>("vkGetCalibratedTimestamps", result);
    }

    ClockCalibration calibration;
    calibration.gpuTicks = values[0] & timestampMask_;
    calibration.gpuFrequency = timestampFrequency_;
    // 호스트 domain 값은 PlatformBase MonotonicTicks와 같은 원천·단위다.
    calibration.cpuTicks = values[1];
    calibration.cpuNanoseconds = TicksToNanoseconds(values[1], platform::MonotonicTickFrequency());
    // 드라이버가 준 두 표본의 최대 편차(ns)를 그대로 공개한다(UE는 버린다).
    calibration.uncertaintyNanoseconds = deviation;
    return Result<ClockCalibration>::Success(calibration);
}

Result<uint64> VulkanDevice::WriteTimestamp() noexcept {
    platform::LockGuard lock(gate_);
    if (queryPool_ == VK_NULL_HANDLE) {
        return Result<uint64>::Failure(ErrorCode::Unsupported);
    }

    if (!recording_) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }

    // 기록 중에는 회수하지 않는다(ResetTimestampSlots의 규칙). 빈 slot이 없으면 Busy다.
    auto write = timestamps_.Allocate();
    if (!write) {
        return Result<uint64>::Failure(write.Error());
    }

    functions_.vkCmdWriteTimestamp(frame_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool_,
                                   write.Value().index);
    usedThisFrame_ |= 1u << (write.Value().index / timestamps_.QueriesPerSlot());
    return Result<uint64>::Success(write.Value().query);
}

void VulkanDevice::PushMarker(StringView name) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_ || !Info().debugMarkers) {
        return;
    }

    const StringView clipped = ClipDebugMarkerName(name);
    char text[MaxDebugMarkerNameBytes + 1] = {};
    platform::MemoryCopy(text, clipped.Data(), clipped.ByteCount());
    text[clipped.ByteCount()] = '\0';
    VkDebugUtilsLabelEXT label = {};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = text;
    functions_.vkCmdBeginDebugUtilsLabelEXT(frame_, &label);
    ++markerDepth_;
}

void VulkanDevice::PopMarker() noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_ || markerDepth_ == 0) {
        ++ignoredDraws_;
        return;
    }

    functions_.vkCmdEndDebugUtilsLabelEXT(frame_);
    --markerDepth_;
}

} // namespace rocks::graphics::vulkan
