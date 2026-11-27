#include "VulkanDeviceImpl.h"

// 창 출력(swapchain)이다(Docs/Design/VulkanGraphics.ko.md 9절). 패스는 중간 이미지에 그리고
// EndFrame이 swapchain 이미지를 얻어 blit한 뒤 present한다(UE 기본 경로
// r.Vulkan.DelayAcquireBackBuffer=1과 같다). 그래서 swapchain 형식(BGRA·RGBA)과 상관없이
// 파이프라인은 RGBA8UnormSrgb 하나로 호환된다.
namespace rocks::graphics::vulkan {

namespace {

constexpr uint32 MaxSurfaceFormats = 64;
constexpr uint32 MaxPresentModes = 16;
// surface가 크기를 swapchain에 맡긴다는 currentExtent 표지다(사양 VkSurfaceCapabilitiesKHR).
constexpr uint32 SurfaceExtentUndefined = NumericLimits<uint32>::Max();

// swapchain 이미지 형식의 선호 순서다(모두 UNORM, 색 공간 SRGB_NONLINEAR). 중간 이미지가 sRGB로
// 인코딩한 byte를 blit이 그대로(채널 순서만 맞춰) 옮긴다.
constexpr VkFormat PreferredFormats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                         VK_FORMAT_A8B8G8R8_UNORM_PACK32};

uint32 Clamp(uint32 value, uint32 low, uint32 high) noexcept {
    return value < low ? low : (value > high ? high : value);
}

} // namespace

Result<VkSemaphore> VulkanDevice::TakeAcquireSemaphore() noexcept {
    const uint64 completed = CompletedValue();
    for (usize index = 0; index < acquireSemaphores_.Size(); ++index) {
        if (acquireSemaphores_[index].fenceValue <= completed) {
            const VkSemaphore semaphore = acquireSemaphores_[index].semaphore;
            auto removed = acquireSemaphores_.TryRemoveAtSwap(index);
            (void)removed;
            return Result<VkSemaphore>::Success(semaphore);
        }
    }

    VkSemaphoreCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    const VkResult result = functions_.vkCreateSemaphore(device_, &info, nullptr, &semaphore);
    if (result != VK_SUCCESS) {
        return FailWith<VkSemaphore>("vkCreateSemaphore", result);
    }

    return Result<VkSemaphore>::Success(semaphore);
}

void VulkanDevice::ReturnAcquireSemaphore(VkSemaphore semaphore, uint64 fenceValue) noexcept {
    if (!acquireSemaphores_.TryPushBack(AcquireSemaphore{semaphore, fenceValue})) {
        // 목록을 늘리지 못하면 소멸 때 파괴한다(GPU가 아직 기다릴 수 있다).
        auto kept = orphanSemaphores_.TryPushBack(semaphore);
        (void)kept;
    }
}

Result<void> VulkanDevice::CreateSwapChainTarget(SwapChainRecord& record) noexcept {
    // 중간 이미지는 UNORM으로 두고 그리기 view만 SRGB다(MUTABLE_FORMAT). blit은 UNORM끼리라 값을
    // 바꾸지 않고, CopyPassTarget은 같은 크기 부류(SRGB RenderTarget)로 byte를 그대로 옮긴다.
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {record.width, record.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    auto image = CreateImageWithMemory(info, record.target, record.targetMemory);
    if (!image) {
        return image;
    }

    VkImageViewCreateInfo view = {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = record.target;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_SRGB;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkResult result = functions_.vkCreateImageView(device_, &view, nullptr, &record.targetView);
    if (result != VK_SUCCESS) {
        record.targetView = VK_NULL_HANDLE;
        DestroySwapChainTarget(record);
        return Fail("vkCreateImageView", result);
    }

    auto renderPass = RenderPassFor(VK_FORMAT_R8G8B8A8_SRGB, false);
    auto loadPass = renderPass ? RenderPassFor(VK_FORMAT_R8G8B8A8_SRGB, true) : Move(renderPass);
    if (!renderPass || !loadPass) {
        DestroySwapChainTarget(record);
        return Result<void>::Failure(!renderPass ? renderPass.Error() : loadPass.Error());
    }

    VkFramebufferCreateInfo framebuffer = {};
    framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer.renderPass = renderPass.Value();
    framebuffer.attachmentCount = 1;
    framebuffer.pAttachments = &record.targetView;
    framebuffer.width = record.width;
    framebuffer.height = record.height;
    framebuffer.layers = 1;
    result = functions_.vkCreateFramebuffer(device_, &framebuffer, nullptr, &record.framebuffer);
    if (result != VK_SUCCESS) {
        record.framebuffer = VK_NULL_HANDLE;
        DestroySwapChainTarget(record);
        return Fail("vkCreateFramebuffer", result);
    }

    record.targetLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    record.frameOpen = false;
    return Result<void>::Success();
}

void VulkanDevice::DestroySwapChainTarget(SwapChainRecord& record) noexcept {
    if (record.framebuffer != VK_NULL_HANDLE) {
        functions_.vkDestroyFramebuffer(device_, record.framebuffer, nullptr);
    }

    if (record.targetView != VK_NULL_HANDLE) {
        functions_.vkDestroyImageView(device_, record.targetView, nullptr);
    }

    if (record.target != VK_NULL_HANDLE) {
        functions_.vkDestroyImage(device_, record.target, nullptr);
    }

    FreeMemoryLocked(record.targetMemory);
    record.framebuffer = VK_NULL_HANDLE;
    record.targetView = VK_NULL_HANDLE;
    record.target = VK_NULL_HANDLE;
    record.targetMemory = MemoryAllocation();
    record.targetLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    record.frameOpen = false;
}

void VulkanDevice::DestroySwapChainImages(SwapChainRecord& record, bool keepSwapchain) noexcept {
    for (uint32 index = 0; index < MaxSwapChainImages; ++index) {
        if (record.renderDone[index] != VK_NULL_HANDLE) {
            functions_.vkDestroySemaphore(device_, record.renderDone[index], nullptr);
            record.renderDone[index] = VK_NULL_HANDLE;
        }

        record.images[index] = VK_NULL_HANDLE;
    }

    record.imageCount = 0;
    if (!keepSwapchain && record.swapchain != VK_NULL_HANDLE) {
        functions_.vkDestroySwapchainKHR(device_, record.swapchain, nullptr);
        record.swapchain = VK_NULL_HANDLE;
    }
}

// swapchain을 (다시) 만든다. 앞 swapchain이 있으면 oldSwapchain으로 넘기고 만든 뒤 파괴한다.
// 호출자가 queue idle을 보장한다(앞 present의 semaphore 대기가 끝났다).
Result<void> VulkanDevice::BuildSwapChain(SwapChainRecord& record) noexcept {
    VkSurfaceCapabilitiesKHR capabilities = {};
    VkResult result = functions_.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physical_, record.surface, &capabilities);
    if (result != VK_SUCCESS) {
        return Fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
    }

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == SurfaceExtentUndefined) {
        extent.width = Clamp(record.width, capabilities.minImageExtent.width,
                             capabilities.maxImageExtent.width);
        extent.height = Clamp(record.height, capabilities.minImageExtent.height,
                              capabilities.maxImageExtent.height);
    }

    if (extent.width == 0 || extent.height == 0) {
        return FailCode("BuildSwapChain", ErrorCode::DependencyFailed,
                        "surface extent is zero (minimized window)");
    }

    if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
        return FailCode("BuildSwapChain", ErrorCode::Unsupported,
                        "swap chain images cannot be transfer destinations");
    }

    VkSurfaceFormatKHR formats[MaxSurfaceFormats];
    uint32 formatCount = 0;
    result = functions_.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, record.surface,
                                                             &formatCount, nullptr);
    formatCount = formatCount < MaxSurfaceFormats ? formatCount : MaxSurfaceFormats;
    if (result == VK_SUCCESS) {
        result = functions_.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, record.surface,
                                                                 &formatCount, formats);
    }

    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return Fail("vkGetPhysicalDeviceSurfaceFormatsKHR", result);
    }

    VkFormat chosen = VK_FORMAT_UNDEFINED;
    for (const VkFormat preferred : PreferredFormats) {
        for (uint32 index = 0; index < formatCount && chosen == VK_FORMAT_UNDEFINED; ++index) {
            VkFormatProperties support = {};
            functions_.vkGetPhysicalDeviceFormatProperties(physical_, preferred, &support);
            if (formats[index].format == preferred &&
                formats[index].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                (support.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0) {
                chosen = preferred;
            }
        }
    }

    if (chosen == VK_FORMAT_UNDEFINED) {
        return FailCode("BuildSwapChain", ErrorCode::Unsupported,
                        "surface offers no 8-bit UNORM format in the sRGB color space");
    }

    VkPresentModeKHR modes[MaxPresentModes];
    uint32 modeCount = 0;
    result = functions_.vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, record.surface,
                                                                  &modeCount, nullptr);
    modeCount = modeCount < MaxPresentModes ? modeCount : MaxPresentModes;
    if (result == VK_SUCCESS) {
        result = functions_.vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, record.surface,
                                                                      &modeCount, modes);
    }

    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return Fail("vkGetPhysicalDeviceSurfacePresentModesKHR", result);
    }

    // vsync면 FIFO(모든 장치가 지원), 아니면 IMMEDIATE → MAILBOX → FIFO다(UE
    // VulkanSwapChain.cpp:477–493).
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!record.vsync) {
        const VkPresentModeKHR order[] = {VK_PRESENT_MODE_IMMEDIATE_KHR,
                                          VK_PRESENT_MODE_MAILBOX_KHR};
        bool found = false;
        for (const VkPresentModeKHR wanted : order) {
            for (uint32 index = 0; index < modeCount && !found; ++index) {
                if (modes[index] == wanted) {
                    presentMode = wanted;
                    found = true;
                }
            }
        }
    }

    uint32 imageCount = RequestedSwapChainImages > capabilities.minImageCount
                            ? RequestedSwapChainImages
                            : capabilities.minImageCount;
    if (capabilities.maxImageCount != 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0) {
        for (uint32 bit = 1; bit != 0 && bit <= capabilities.supportedCompositeAlpha; bit <<= 1) {
            if ((capabilities.supportedCompositeAlpha & bit) != 0) {
                alpha = static_cast<VkCompositeAlphaFlagBitsKHR>(bit);
                break;
            }
        }
    }

    const VkSwapchainKHR previous = record.swapchain;
    VkSwapchainCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = record.surface;
    info.minImageCount = imageCount;
    info.imageFormat = chosen;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = previous;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    result = functions_.vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain);
    DestroySwapChainImages(record, false);
    if (result != VK_SUCCESS) {
        return Fail("vkCreateSwapchainKHR", result);
    }

    record.swapchain = swapchain;
    record.extent = extent;
    record.imageFormat = chosen;
    uint32 count = MaxSwapChainImages;
    result = functions_.vkGetSwapchainImagesKHR(device_, swapchain, &count, record.images);
    if (result != VK_SUCCESS) {
        DestroySwapChainImages(record, false);
        return Fail("vkGetSwapchainImagesKHR", result);
    }

    record.imageCount = count;
    for (uint32 index = 0; index < count; ++index) {
        VkSemaphoreCreateInfo semaphore = {};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        result =
            functions_.vkCreateSemaphore(device_, &semaphore, nullptr, &record.renderDone[index]);
        if (result != VK_SUCCESS) {
            record.renderDone[index] = VK_NULL_HANDLE;
            DestroySwapChainImages(record, false);
            return Fail("vkCreateSemaphore", result);
        }
    }

    record.outOfDate = false;
    return Result<void>::Success();
}

Result<void> VulkanDevice::RecreateSwapChain(SwapChainRecord& record) noexcept {
    // present의 semaphore 대기까지 끝난 뒤에 앞 객체를 놓는다(fence는 present를 덮지 않는다).
    (void)functions_.vkQueueWaitIdle(queue_);
    if (record.surfaceLost) {
        // 같은 창에 surface를 겹쳐 만들지 않도록 swapchain과 surface를 먼저 파괴한다.
        DestroySwapChainImages(record, false);
        if (record.surface != VK_NULL_HANDLE) {
            functions_.vkDestroySurfaceKHR(instance_, record.surface, nullptr);
            record.surface = VK_NULL_HANDLE;
        }

        const VkResult result = CreateSurface(functions_, instance_, record.native.window,
                                              record.native.instance, record.surface);
        if (result != VK_SUCCESS) {
            record.surface = VK_NULL_HANDLE;
            record.broken = true;
            return Fail("CreateSurface", result);
        }

        record.surfaceLost = false;
    }

    auto built = BuildSwapChain(record);
    record.broken = !built;
    return built;
}

Result<SwapChainHandle> VulkanDevice::CreateSwapChain(const SwapChainDesc& desc) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<SwapChainHandle>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);
    if (desc.surface.window == nullptr || desc.width == 0 || desc.height == 0) {
        return Result<SwapChainHandle>::Failure(ErrorCode::InvalidArgument);
    }

    if (!swapchainExtension_) {
        auto failed = FailCode("CreateSwapChain", ErrorCode::Unsupported,
                               "VK_KHR_swapchain or the platform surface extension is missing");
        return Result<SwapChainHandle>::Failure(failed.Error());
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<SwapChainHandle>::Failure(reserved.Error());
    }

    SwapChainRecord record;
    record.native = desc.surface;
    record.width = desc.width;
    record.height = desc.height;
    record.vsync = desc.vsync;
    VkResult result = CreateSurface(functions_, instance_, desc.surface.window,
                                    desc.surface.instance, record.surface);
    if (result != VK_SUCCESS) {
        return FailWith<SwapChainHandle>("CreateSurface", result);
    }

    VkBool32 presentable = VK_FALSE;
    result = functions_.vkGetPhysicalDeviceSurfaceSupportKHR(physical_, queueFamily_,
                                                             record.surface, &presentable);
    if (result != VK_SUCCESS || presentable != VK_TRUE) {
        functions_.vkDestroySurfaceKHR(instance_, record.surface, nullptr);
        auto failed = FailCode("vkGetPhysicalDeviceSurfaceSupportKHR", ErrorCode::Unsupported,
                               "the graphics queue cannot present to this surface");
        return Result<SwapChainHandle>::Failure(failed.Error());
    }

    auto target = CreateSwapChainTarget(record);
    if (!target) {
        functions_.vkDestroySurfaceKHR(instance_, record.surface, nullptr);
        return Result<SwapChainHandle>::Failure(target.Error());
    }

    auto built = BuildSwapChain(record);
    if (!built) {
        DestroySwapChainTarget(record);
        functions_.vkDestroySurfaceKHR(instance_, record.surface, nullptr);
        return Result<SwapChainHandle>::Failure(built.Error());
    }

    auto added = swapChains_.TryAdd(Move(record));
    if (!added) {
        DestroySwapChainTarget(record);
        DestroySwapChainImages(record, false);
        functions_.vkDestroySurfaceKHR(instance_, record.surface, nullptr);
        return Result<SwapChainHandle>::Failure(added.Error());
    }

    return Result<SwapChainHandle>::Success(SwapChainHandle{added.Value()});
}

Result<void> VulkanDevice::ResizeSwapChain(SwapChainHandle handle, uint32 width,
                                           uint32 height) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);
    SwapChainRecord* record = swapChains_.Find(handle.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (width == 0 || height == 0 || recording_) {
        return Result<void>::Failure(recording_ ? ErrorCode::Busy : ErrorCode::InvalidArgument);
    }

    // 중간 이미지·swapchain 이미지를 쓰는 GPU 작업과 present가 모두 끝난 뒤 다시 만든다.
    auto idle = WaitIdleLocked();
    if (!idle) {
        return idle;
    }

    (void)functions_.vkQueueWaitIdle(queue_);
    record = swapChains_.Find(handle.slot);
    DestroySwapChainTarget(*record);
    record->width = width;
    record->height = height;
    record->broken = true;
    auto target = CreateSwapChainTarget(*record);
    if (!target) {
        return target;
    }

    return RecreateSwapChain(*record);
}

void VulkanDevice::Retire(SwapChainHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    SwapChainRecord* record = swapChains_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }

    // 중간 이미지·swapchain·surface·semaphore를 마지막 사용 fence 뒤에(그리고 present가 끝나도록
    // queue idle 뒤에) 놓는다. 기록 중인 프레임이 이 swapchain에 그리고 있으면 present만 생략된다.
    Retired entry;
    entry.image = record->target;
    entry.view = record->targetView;
    entry.framebuffer = record->framebuffer;
    entry.memory = record->targetMemory;
    entry.swapchain = record->swapchain;
    entry.surface = record->surface;
    for (uint32 index = 0; index < MaxSwapChainImages; ++index) {
        if (record->renderDone[index] != VK_NULL_HANDLE) {
            entry.semaphores[entry.semaphoreCount++] = record->renderDone[index];
        }
    }

    entry.presentIdle = true;
    RetireEntry(Move(entry));
    auto removed = swapChains_.TryRemove(handle.slot);
    (void)removed;
}

Result<void> VulkanDevice::RecordPresentBlit(SwapChainRecord& record, uint32& imageIndex,
                                             VkSemaphore& acquired) noexcept {
    // 중간 이미지는 blit 원본으로 쉰다(다음 패스가 TRANSFER 뒤에서 그리기로 바꾼다).
    Barrier(frame_, record.target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (record.broken) {
        return FailCode("EndFrame", ErrorCode::DependencyFailed,
                        "swap chain has no images; present skipped");
    }

    if (record.outOfDate || record.surfaceLost || record.swapchain == VK_NULL_HANDLE) {
        auto rebuilt = RecreateSwapChain(record);
        if (!rebuilt) {
            return rebuilt;
        }
    }

    auto semaphore = TakeAcquireSemaphore();
    if (!semaphore) {
        return Result<void>::Failure(semaphore.Error());
    }

    uint32 index = 0;
    VkResult result = VK_ERROR_OUT_OF_DATE_KHR;
    // OUT_OF_DATE·SURFACE_LOST는 다시 만들고 한 번 더 얻는다(UE는 최대 4회,
    // VulkanViewport.cpp:198–228).
    for (uint32 attempt = 0; attempt < 2; ++attempt) {
        result = functions_.vkAcquireNextImageKHR(device_, record.swapchain,
                                                  NumericLimits<uint64>::Max(), semaphore.Value(),
                                                  VK_NULL_HANDLE, &index);
        if (result != VK_ERROR_OUT_OF_DATE_KHR && result != VK_ERROR_SURFACE_LOST_KHR) {
            break;
        }

        record.surfaceLost = result == VK_ERROR_SURFACE_LOST_KHR;
        auto rebuilt = RecreateSwapChain(record);
        if (!rebuilt) {
            // 실패한 acquire는 semaphore를 신호하지 않으므로 바로 다시 쓸 수 있다.
            ReturnAcquireSemaphore(semaphore.Value(), 0);
            return rebuilt;
        }
    }

    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        ReturnAcquireSemaphore(semaphore.Value(), 0);
        return Fail("vkAcquireNextImageKHR", result);
    }

    const VkImage image = record.images[index];
    // semaphore 대기 단계(TRANSFER)와 사슬을 잇도록 TRANSFER에서 전이한다.
    Barrier(frame_, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit region = {};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = {static_cast<int32>(record.width), static_cast<int32>(record.height), 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[1] = {static_cast<int32>(record.extent.width),
                            static_cast<int32>(record.extent.height), 1};
    const bool sameSize =
        record.extent.width == record.width && record.extent.height == record.height;
    functions_.vkCmdBlitImage(frame_, record.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                              sameSize ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    Barrier(frame_, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0,
            1);
    imageIndex = index;
    acquired = semaphore.Value();
    return Result<void>::Success();
}

} // namespace rocks::graphics::vulkan
