#include "VulkanDeviceImpl.h"

#include <Platform/Atomics.h>
#include <Platform/Fatal.h>

// 제출·완료·회수(GX7, Docs/Design/VulkanGraphics.ko.md 4절)다. 제출마다 VkFence 하나로 완료를 본다.
// 소유 스레드는 vkGetFenceStatus로 완료 값을 올리고, 관찰 스레드(FenceTimeline)는 token으로 받은
// fence를 vkWaitForFences로 기다린다. fence는 양쪽이 다 놓은 뒤에만 reset한다(사양 "Fences": 대기와
// 조회는 외부 동기화가 필요 없고 vkResetFences만 필요하다).
namespace rocks::graphics::vulkan {

bool FenceWatcher::WaitSubmission(uint64 value, void* token) noexcept {
    (void)value;
    const auto* slot = static_cast<const SubmissionFence*>(token);
    const VkFence fence = slot->fence;
    return functions->vkWaitForFences(device, 1, &fence, VK_TRUE, NumericLimits<uint64>::Max()) ==
           VK_SUCCESS;
}

void FenceWatcher::ReleaseToken(void* token) noexcept {
    auto* slot = static_cast<SubmissionFence*>(token);
    platform::AtomicStore<platform::MemoryOrder::Release>(&slot->watcherHold, 0u);
}

Result<void> VulkanDevice::CreateCommandPool() noexcept {
    VkCommandPoolCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = queueFamily_;
    const VkResult result = functions_.vkCreateCommandPool(device_, &info, nullptr, &commandPool_);
    if (result != VK_SUCCESS) {
        commandPool_ = VK_NULL_HANDLE;
        return Fail("vkCreateCommandPool", result);
    }

    return Result<void>::Success();
}

uint64 VulkanDevice::CompletedValue() const noexcept {
    // 손실 뒤에는 제출한 일이 더 실행되지 않으므로 모두 놓아도 된다(사양 "Lost Device").
    return timeline_.IsLost() ? fenceValue_ : completedValue_;
}

void VulkanDevice::PollCompleted() noexcept {
    // 같은 queue의 fence 신호는 앞 제출의 완료를 포함하므로(사양 "vkQueueSubmit" fence signal의 첫
    // 동기 범위) 작은 값부터 끝난 것까지만 본다.
    for (;;) {
        SubmissionFence* oldest = nullptr;
        for (uint32 index = 0; index < fenceCount_; ++index) {
            SubmissionFence& slot = fences_[index];
            if (slot.inFlight && !slot.ownerDone &&
                (oldest == nullptr || slot.value < oldest->value)) {
                oldest = &slot;
            }
        }

        if (oldest == nullptr) {
            break;
        }

        const VkResult status = functions_.vkGetFenceStatus(device_, oldest->fence);
        if (status != VK_SUCCESS) {
            DetectLost(status);
            break;
        }

        oldest->ownerDone = true;
        completedValue_ = oldest->value > completedValue_ ? oldest->value : completedValue_;
    }

    for (uint32 index = 0; index < fenceCount_; ++index) {
        SubmissionFence& slot = fences_[index];
        if (slot.inFlight && slot.ownerDone &&
            platform::AtomicLoad<platform::MemoryOrder::Acquire>(&slot.watcherHold) == 0) {
            (void)functions_.vkResetFences(device_, 1, &slot.fence);
            slot.inFlight = false;
            slot.ownerDone = false;
            slot.value = 0;
        }
    }

    if (!timeline_.IsLost()) {
        timeline_.Publish(completedValue_);
    }
}

Result<uint32> VulkanDevice::AcquireFenceSlot() noexcept {
    PollCompleted();
    for (uint32 index = 0; index < fenceCount_; ++index) {
        if (!fences_[index].inFlight) {
            return Result<uint32>::Success(index);
        }
    }

    if (fenceCount_ < MaxSubmissionFences) {
        VkFenceCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        SubmissionFence& slot = fences_[fenceCount_];
        const VkResult result = functions_.vkCreateFence(device_, &info, nullptr, &slot.fence);
        if (result != VK_SUCCESS) {
            slot.fence = VK_NULL_HANDLE;
            return FailWith<uint32>("vkCreateFence", result);
        }

        return Result<uint32>::Success(fenceCount_++);
    }

    // 모두 비행 중이다(관찰 스레드가 놓지 않은 것 포함). 가장 오래된 제출을 기다린다.
    uint64 oldest = NumericLimits<uint64>::Max();
    for (uint32 index = 0; index < fenceCount_; ++index) {
        if (!fences_[index].ownerDone && fences_[index].value < oldest) {
            oldest = fences_[index].value;
        }
    }

    if (oldest != NumericLimits<uint64>::Max()) {
        auto waited = WaitFenceOwner(oldest);
        if (!waited) {
            return Result<uint32>::Failure(waited.Error());
        }
    }

    PollCompleted();
    for (uint32 index = 0; index < fenceCount_; ++index) {
        if (!fences_[index].inFlight) {
            return Result<uint32>::Success(index);
        }
    }

    return Result<uint32>::Failure(ErrorCode::Busy);
}

Result<uint64> VulkanDevice::Submit(VkCommandBuffer buffer, VkSemaphore wait,
                                    VkPipelineStageFlags waitStage, VkSemaphore signal) noexcept {
    if (timeline_.IsLost()) {
        return Result<uint64>::Failure(ErrorCode::Destroyed);
    }

    auto slotIndex = AcquireFenceSlot();
    if (!slotIndex) {
        return Result<uint64>::Failure(slotIndex.Error());
    }

    SubmissionFence& slot = fences_[slotIndex.Value()];
    VkSubmitInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    info.waitSemaphoreCount = wait != VK_NULL_HANDLE ? 1u : 0u;
    info.pWaitSemaphores = &wait;
    info.pWaitDstStageMask = &waitStage;
    info.commandBufferCount = buffer != VK_NULL_HANDLE ? 1u : 0u;
    info.pCommandBuffers = &buffer;
    info.signalSemaphoreCount = signal != VK_NULL_HANDLE ? 1u : 0u;
    info.pSignalSemaphores = &signal;
    const bool empty =
        wait == VK_NULL_HANDLE && buffer == VK_NULL_HANDLE && signal == VK_NULL_HANDLE;
    // 빈 제출은 배치 없이 fence만 신호한다(지금까지 제출한 모든 일의 완료 지점).
    const VkResult result =
        functions_.vkQueueSubmit(queue_, empty ? 0u : 1u, empty ? nullptr : &info, slot.fence);
    if (result != VK_SUCCESS) {
        return FailWith<uint64>("vkQueueSubmit", result);
    }

    const uint64 value = ++fenceValue_;
    slot.value = value;
    slot.inFlight = true;
    slot.ownerDone = false;
    platform::AtomicStore<platform::MemoryOrder::Release>(&slot.watcherHold, 1u);
    timeline_.Submitted(value, &slot);
    return Result<uint64>::Success(value);
}

Result<void> VulkanDevice::WaitFenceOwner(uint64 value) noexcept {
    if (value <= completedValue_ || value == 0) {
        return timeline_.IsLost() ? Result<void>::Failure(ErrorCode::Destroyed)
                                  : Result<void>::Success();
    }

    // value를 덮는 가장 이른 비행 중 제출을 기다린다(뒤 fence는 앞 제출의 완료를 포함한다).
    const SubmissionFence* target = nullptr;
    for (uint32 index = 0; index < fenceCount_; ++index) {
        const SubmissionFence& slot = fences_[index];
        if (slot.inFlight && slot.value >= value &&
            (target == nullptr || slot.value < target->value)) {
            target = &slot;
        }
    }

    if (target != nullptr && !target->ownerDone) {
        const VkFence fence = target->fence;
        const VkResult result =
            functions_.vkWaitForFences(device_, 1, &fence, VK_TRUE, NumericLimits<uint64>::Max());
        if (result != VK_SUCCESS) {
            DetectLost(result);
            if (!timeline_.IsLost()) {
                return Fail("vkWaitForFences", result);
            }
        }
    }

    PollCompleted();
    if (timeline_.IsLost()) {
        return Result<void>::Failure(ErrorCode::Destroyed);
    }

    return Result<void>::Success();
}

Result<uint32> VulkanDevice::PickCommandBuffer() noexcept {
    PollCompleted();
    const uint64 completed = CompletedValue();
    uint32 oldest = NumericLimits<uint32>::Max();
    for (uint32 index = 0; index < commandCount_; ++index) {
        if (commands_[index].inUse) {
            continue;
        }

        if (commands_[index].fenceValue <= completed) {
            return Result<uint32>::Success(index);
        }

        if (oldest == NumericLimits<uint32>::Max() ||
            commands_[index].fenceValue < commands_[oldest].fenceValue) {
            oldest = index;
        }
    }

    if (commandCount_ < MaxCommandBuffers) {
        VkCommandBufferAllocateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        info.commandPool = commandPool_;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        CommandRecord& record = commands_[commandCount_];
        const VkResult result = functions_.vkAllocateCommandBuffers(device_, &info, &record.buffer);
        if (result != VK_SUCCESS) {
            record.buffer = VK_NULL_HANDLE;
            return FailWith<uint32>("vkAllocateCommandBuffers", result);
        }

        record.fenceValue = 0;
        return Result<uint32>::Success(commandCount_++);
    }

    // 소유 스레드만 GPU를 기다린다. 다른 스레드(생성·갱신)는 자리가 없으면 Busy다.
    if (oldest == NumericLimits<uint32>::Max() || !ownership_.IsOwner()) {
        return Result<uint32>::Failure(ErrorCode::Busy);
    }

    auto waited = WaitFenceOwner(commands_[oldest].fenceValue);
    if (!waited) {
        return Result<uint32>::Failure(waited.Error());
    }

    return Result<uint32>::Success(oldest);
}

void VulkanDevice::DestroySubmissionObjects() noexcept {
    for (uint32 index = 0; index < fenceCount_; ++index) {
        functions_.vkDestroyFence(device_, fences_[index].fence, nullptr);
        fences_[index] = {};
    }

    fenceCount_ = 0;
    if (commandPool_ != VK_NULL_HANDLE) {
        // 명령 버퍼는 pool과 함께 해제된다.
        functions_.vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }

    commandCount_ = 0;
}

Result<void> VulkanDevice::WaitIdleLocked() noexcept {
    // 쌓인 업로드도 제출해 함께 기다린다.
    auto uploads = FlushUploadsLocked();
    if (!uploads) {
        ProcessRetired();
        return Result<void>::Failure(uploads.Error());
    }

    auto signaled = Submit(VK_NULL_HANDLE, VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    if (!signaled) {
        ProcessRetired();
        return Result<void>::Failure(signaled.Error());
    }

    auto waited = WaitFenceOwner(signaled.Value());
    // 기록 중인 프레임의 보류 항목은 아직 제출되지 않았으므로 남긴다.
    ProcessRetired();
    return waited;
}

Result<void> VulkanDevice::WaitIdle() noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);
    return WaitIdleLocked();
}

void VulkanDevice::ReclaimCompleted() noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    ProcessRetired();
}

void VulkanDevice::AssignPendingRetires(uint64 value) noexcept {
    for (Retired& entry : retired_) {
        if (entry.pending) {
            entry.pending = false;
            entry.fenceValue = value;
        }
    }
}

void VulkanDevice::AssignPendingUploads(uint64 value) noexcept {
    for (Retired& entry : retired_) {
        if (entry.uploadPending) {
            entry.uploadPending = false;
            entry.fenceValue = value > entry.fenceValue ? value : entry.fenceValue;
        }
    }
}

Result<void> VulkanDevice::ReserveRetireCapacity(usize extra) noexcept {
    const usize live = buffers_.Size() + bufferViews_.Size() + orphanBuffers_.Size() +
                       textures_.Size() + pipelines_.Size() + readbacks_.Size() +
                       swapChains_.Size() + framebuffers_.Size() + heaps_.Size();
    return retired_.TryEnsureCapacity(retired_.Size() + live + extra);
}

void VulkanDevice::RetireEntry(Retired&& entry) noexcept {
    // 제출한 일은 모두 마지막 fence 값 앞에 있다. 기록 중이면 아직 제출하지 않은 프레임이 쓸 수
    // 있으므로 그 프레임의 값을 EndFrame이 정한다. 열린 업로드 기록이 쓸 수 있으면 그 묶음 값이다.
    entry.fenceValue = fenceValue_;
    entry.pending = recording_;
    entry.uploadPending = uploadOpen_;
    // 용량은 자원을 만들 때 확보했으므로(ReserveRetireCapacity) 실패하지 않는다.
    if (!retired_.TryPushBack(Move(entry))) {
        platform::Fatal("Vulkan retire list capacity invariant violated");
    }
}

void VulkanDevice::KeepUntilDestruction(Retired&& entry) noexcept {
    entry.fenceValue = RetainUntilDestructionFence;
    entry.pending = false;
    entry.uploadPending = false;
    if (!retired_.TryPushBack(Move(entry))) {
        // 기록할 자리조차 없으면 파괴하지 않고 버린다(GPU가 쓰는 중일 수 있는 객체를 놓는 것보다
        // 낫다).
        return;
    }
}

void VulkanDevice::DestroyRetired(Retired& entry) noexcept {
    if (entry.framebuffer != VK_NULL_HANDLE) {
        functions_.vkDestroyFramebuffer(device_, entry.framebuffer, nullptr);
    }

    if (entry.bufferView != VK_NULL_HANDLE) {
        functions_.vkDestroyBufferView(device_, entry.bufferView, nullptr);
    }

    if (entry.view != VK_NULL_HANDLE) {
        functions_.vkDestroyImageView(device_, entry.view, nullptr);
    }

    if (entry.attachmentView != VK_NULL_HANDLE) {
        functions_.vkDestroyImageView(device_, entry.attachmentView, nullptr);
    }

    if (entry.image != VK_NULL_HANDLE) {
        functions_.vkDestroyImage(device_, entry.image, nullptr);
    }

    if (entry.buffer != VK_NULL_HANDLE) {
        functions_.vkDestroyBuffer(device_, entry.buffer, nullptr);
    }

    if (entry.pipeline != VK_NULL_HANDLE) {
        functions_.vkDestroyPipeline(device_, entry.pipeline, nullptr);
    }

    if (entry.pipelineLayout != VK_NULL_HANDLE) {
        functions_.vkDestroyPipelineLayout(device_, entry.pipelineLayout, nullptr);
    }

    for (VkDescriptorSetLayout layout : entry.setLayouts) {
        if (layout != VK_NULL_HANDLE) {
            functions_.vkDestroyDescriptorSetLayout(device_, layout, nullptr);
        }
    }

    FreeMemoryLocked(entry.memory);
    // placed 자원은 메모리를 heap이 가진다. 자원을 파괴했으므로 heap의 placed 수를 줄인다.
    if (!entry.heap.IsNull()) {
        ReleasePlacement(entry.heap);
    }

    if (entry.heapMemory != VK_NULL_HANDLE) {
        functions_.vkFreeMemory(device_, entry.heapMemory, nullptr);
    }

    for (uint32 index = 0; index < entry.semaphoreCount; ++index) {
        functions_.vkDestroySemaphore(device_, entry.semaphores[index], nullptr);
    }

    // swapchain 이미지는 swapchain이 소유한다. surface는 swapchain 뒤에 파괴한다.
    if (entry.swapchain != VK_NULL_HANDLE) {
        functions_.vkDestroySwapchainKHR(device_, entry.swapchain, nullptr);
    }

    if (entry.surface != VK_NULL_HANDLE) {
        functions_.vkDestroySurfaceKHR(instance_, entry.surface, nullptr);
    }

    entry = {};
}

void VulkanDevice::ProcessRetired() noexcept {
    PollCompleted();
    const uint64 completed = CompletedValue();
    ring_.Reclaim(completed);
    if (staging_) {
        staging_->Reclaim(completed);
    }

    bool idled = false;
    for (usize index = retired_.Size(); index > 0; --index) {
        Retired& entry = retired_[index - 1];
        if (entry.pending || entry.uploadPending || entry.fenceValue > completed) {
            continue;
        }

        if (entry.presentIdle && !idled) {
            // present의 semaphore 대기는 fence가 덮지 않으므로 queue idle로 끝낸다.
            (void)functions_.vkQueueWaitIdle(queue_);
            idled = true;
        }

        DestroyRetired(entry);
        auto removed = retired_.TryRemoveAtSwap(index - 1);
        (void)removed;
    }
}

} // namespace rocks::graphics::vulkan
