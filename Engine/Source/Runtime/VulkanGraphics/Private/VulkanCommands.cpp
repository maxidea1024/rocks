#include "VulkanDeviceImpl.h"

#include <Platform/Memory.h>

// 프레임 기록(CommandContext)과 EndFrame이다(Docs/Design/VulkanGraphics.ko.md 8·9·11절).
namespace rocks::graphics::vulkan {

namespace {

struct StageAccess {
    VkPipelineStageFlags stage = 0;
    VkAccessFlags access = 0;
};

// layout을 떠날 때 기다릴 단계와 쓰기다(그 layout에서 일어났던 일).
StageAccess SourceOf(VkImageLayout layout) noexcept {
    switch (layout) {
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return {VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0};
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return {VK_PIPELINE_STAGE_TRANSFER_BIT, 0};
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
    default:
        return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};
    }
}

// layout에 들어간 뒤 기다리게 할 단계와 접근이다.
StageAccess DestinationOf(VkImageLayout layout) noexcept {
    switch (layout) {
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return {VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT};
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
    default:
        return {VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0};
    }
}

} // namespace

void VulkanDevice::Barrier(VkCommandBuffer buffer, VkImage image, VkImageLayout from,
                           VkImageLayout to, uint32 baseMip, uint32 mipCount,
                           VkPipelineStageFlags extraSourceStages) const noexcept {
    const StageAccess source = SourceOf(from);
    const StageAccess destination = DestinationOf(to);
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = source.access;
    barrier.dstAccessMask = destination.access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, 1};
    functions_.vkCmdPipelineBarrier(buffer, source.stage | extraSourceStages, destination.stage, 0,
                                    0, nullptr, 0, nullptr, 1, &barrier);
}

void VulkanDevice::ResetTimestampSlots() noexcept {
    resetThisFrame_ = 0;
    usedThisFrame_ = 0;
    if (queryPool_ == VK_NULL_HANDLE) {
        return;
    }

    // 회수는 기록 밖에서만 한다. 그래서 이 기록이 여는 slot은 모두 지금 비어 있고, 다시 쓸 slot은
    // render pass 앞에서 초기화된다(render pass 안에서는 reset할 수 없다).
    HarvestTimestamps();
    const uint32 perSlot = timestamps_.QueriesPerSlot();
    for (uint32 slot = 0; slot < TimestampFrameSlots; ++slot) {
        const uint32 bit = 1u << slot;
        if ((dirtySlots_ & bit) != 0 && timestamps_.IsSlotFree(slot)) {
            functions_.vkCmdResetQueryPool(frame_, queryPool_, slot * perSlot, perSlot);
            resetThisFrame_ |= bit;
        }
    }
}

Result<void> VulkanDevice::BeginRecording() noexcept {
    auto picked = PickCommandBuffer();
    if (!picked) {
        return Result<void>::Failure(picked.Error());
    }

    CommandRecord& record = commands_[picked.Value()];
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    // pool이 RESET_COMMAND_BUFFER_BIT라 시작이 암묵 reset이다.
    const VkResult result = functions_.vkBeginCommandBuffer(record.buffer, &begin);
    if (result != VK_SUCCESS) {
        return Fail("vkBeginCommandBuffer", result);
    }

    record.inUse = true;
    frame_ = record.buffer;
    frameRecord_ = picked.Value();
    recording_ = true;
    insideRenderPass_ = false;
    markerDepth_ = 0;
    boundPipeline_ = {};
    for (BoundVertexBuffer& bound : boundVertex_) {
        bound = {};
    }

    boundIndex_ = {};
    ResetTimestampSlots();
    return Result<void>::Success();
}

void VulkanDevice::BeginRenderPass(bool load, const ClearColor* clear) noexcept {
    auto renderPass = RenderPassFor(target_.viewFormat, load);
    if (!renderPass) {
        return;
    }

    VkClearValue value = {};
    if (clear != nullptr) {
        value.color.float32[0] = clear->r;
        value.color.float32[1] = clear->g;
        value.color.float32[2] = clear->b;
        value.color.float32[3] = clear->a;
    }

    VkRenderPassBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = renderPass.Value();
    begin.framebuffer = target_.framebuffer;
    begin.renderArea.extent = {target_.width, target_.height};
    begin.clearValueCount = load ? 0u : 1u;
    begin.pClearValues = load ? nullptr : &value;
    functions_.vkCmdBeginRenderPass(frame_, &begin, VK_SUBPASS_CONTENTS_INLINE);
    insideRenderPass_ = true;
}

void VulkanDevice::StartPass(const ClearColor& clear) noexcept {
    // attachment 전이다(GX5). offscreen 대상은 BeginOffscreen이 추적 상태에서 바꿨다. swapchain
    // 중간 이미지는 앞 프레임 blit(TRANSFER 읽기) 뒤일 수 있다(WAR).
    if (target_.texture.IsNull()) {
        Barrier(frame_, target_.image, target_.restLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                0, VK_REMAINING_MIP_LEVELS, VK_PIPELINE_STAGE_TRANSFER_BIT);
    }

    BeginRenderPass(false, &clear);
    // NDC y 위를 화면 위로 둔다(음수 viewport 높이, Vulkan 1.1 core, ShaderSystem 7.2절 8항).
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = static_cast<float>(target_.height);
    viewport.width = static_cast<float>(target_.width);
    viewport.height = -static_cast<float>(target_.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    functions_.vkCmdSetViewport(frame_, 0, 1, &viewport);
    VkRect2D scissor = {};
    scissor.extent = {target_.width, target_.height};
    functions_.vkCmdSetScissor(frame_, 0, 1, &scissor);
    const TextureRecord* white = textures_.Find(white_.slot);
    if (white != nullptr) {
        functions_.vkCmdBindDescriptorSets(frame_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                                           1, 1, &white->set, 0, nullptr);
    }

    const DrawConstants identity;
    functions_.vkCmdPushConstants(frame_, pipelineLayout_,
                                  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                  ConstantBytes, &identity);
}

Result<CommandContext*> VulkanDevice::BeginFrame(SwapChainHandle handle,
                                                 const ClearColor& clear) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<CommandContext*>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);
    SwapChainRecord* record = swapChains_.Find(handle.slot);
    if (record == nullptr) {
        return Result<CommandContext*>::Failure(ErrorCode::StaleHandle);
    }

    if (recording_) {
        return Result<CommandContext*>::Failure(ErrorCode::Busy);
    }

    if (timeline_.IsLost()) {
        return Result<CommandContext*>::Failure(ErrorCode::Destroyed);
    }

    if (record->broken || record->target == VK_NULL_HANDLE) {
        auto failed = FailCode("BeginFrame", ErrorCode::DependencyFailed,
                               "swap chain has no images after a failed resize");
        return Result<CommandContext*>::Failure(failed.Error());
    }

    auto begun = BeginRecording();
    if (!begun) {
        return Result<CommandContext*>::Failure(begun.Error());
    }

    // 패스는 swapchain마다 하나인 중간 이미지(SRGB view)에 그린다.
    target_ = {
        record->target, record->framebuffer, VK_FORMAT_R8G8B8A8_SRGB,     record->targetLayout,
        record->width,  record->height,      PixelFormat::RGBA8UnormSrgb, {}};
    presentTarget_ = handle;
    StartPass(clear);
    return Result<CommandContext*>::Success(this);
}

Result<CommandContext*> VulkanDevice::BeginOffscreen(TextureHandle handle,
                                                     const ClearColor& clear) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<CommandContext*>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);
    const TextureRecord* record = textures_.Find(handle.slot);
    if (record == nullptr || !record->renderTarget) {
        return Result<CommandContext*>::Failure(record == nullptr ? ErrorCode::StaleHandle
                                                                  : ErrorCode::InvalidArgument);
    }

    if (recording_) {
        return Result<CommandContext*>::Failure(ErrorCode::Busy);
    }

    if (timeline_.IsLost()) {
        return Result<CommandContext*>::Failure(ErrorCode::Destroyed);
    }

    const VkImage image = record->image;
    const VkFramebuffer framebuffer = record->framebuffer;
    const uint32 width = record->width;
    const uint32 height = record->height;
    const PixelFormat format = record->format;
    auto begun = BeginRecording();
    if (!begun) {
        return Result<CommandContext*>::Failure(begun.Error());
    }

    target_ = {image,
               framebuffer,
               format == PixelFormat::RGBA8UnormSrgb ? VK_FORMAT_R8G8B8A8_SRGB
                                                     : VK_FORMAT_R8G8B8A8_UNORM,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               width,
               height,
               format,
               handle};
    presentTarget_ = {};
    // attachment 전이: 대상 mip 0을 추적한 상태에서 렌더 타깃으로 바꾼다(GX5).
    TextureRecord* targetRecord = textures_.Find(handle.slot);
    TrackTransition(*targetRecord, handle.slot, 0, 1, ResourceAccess::RenderTarget, false);
    renderPassSuspended_ = false;
    StartPass(clear);
    return Result<CommandContext*>::Success(this);
}

Result<GpuFencePoint> VulkanDevice::EndFrame() noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<GpuFencePoint>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);
    if (!recording_) {
        return Result<GpuFencePoint>::Failure(ErrorCode::InvalidArgument);
    }

    CloseMarkers();
    if (insideRenderPass_) {
        functions_.vkCmdEndRenderPass(frame_);
        insideRenderPass_ = false;
    }

    Result<void> outcome = Result<void>::Success();
    SwapChainRecord* present =
        presentTarget_.IsNull() ? nullptr : swapChains_.Find(presentTarget_.slot);
    uint32 imageIndex = 0;
    VkSemaphore acquired = VK_NULL_HANDLE;
    if (present != nullptr) {
        outcome = RecordPresentBlit(*present, imageIndex, acquired);
        present->targetLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }

    // offscreen 대상은 렌더 타깃 상태 그대로 둔다(GX5: 다음 사용이 명시 전이나 암묵 전이로 바꾼다).

    // 기록 중 Retire한 swapchain은 present를 생략한다(중간 이미지는 회수 항목이 쥔다).
    recording_ = false;
    commands_[frameRecord_].inUse = false;
    VkResult result = functions_.vkEndCommandBuffer(frame_);
    if (result != VK_SUCCESS) {
        // 아무것도 제출하지 않았다. 이 기록만 쓰던 자원은 지금까지의 제출 뒤에 놓아도 된다.
        if (acquired != VK_NULL_HANDLE) {
            auto drained = Submit(VK_NULL_HANDLE, acquired, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  VK_NULL_HANDLE);
            ReturnAcquireSemaphore(acquired,
                                   drained ? drained.Value() : RetainUntilDestructionFence);
        }

        commands_[frameRecord_].fenceValue = fenceValue_;
        AssignPendingRetires(fenceValue_);
        timestamps_.Close(fenceValue_, false);
        dirtySlots_ |= usedThisFrame_;
        SettleTracking(false);
        return FailWith<GpuFencePoint>("vkEndCommandBuffer", result);
    }

    // 이 프레임 전에 쌓인 업로드(생성 초기 데이터·내용 갱신)를 먼저 실행한다.
    auto uploads = FlushUploadsLocked();
    Result<uint64> signaled = Result<uint64>::Failure(ErrorCode::Destroyed);
    if (uploads) {
        signaled =
            Submit(frame_, acquired, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   acquired != VK_NULL_HANDLE ? present->renderDone[imageIndex] : VK_NULL_HANDLE);
    }

    if (!signaled) {
        // 완료를 알 수 없다. 이 프레임이 쓴 자원·명령 버퍼는 소멸 때까지 놓지 않는다.
        if (acquired != VK_NULL_HANDLE) {
            // 신호된 acquire semaphore를 빈 제출로 기다려 다시 쓸 수 있게 한다.
            auto drained = Submit(VK_NULL_HANDLE, acquired, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  VK_NULL_HANDLE);
            if (drained) {
                ReturnAcquireSemaphore(acquired, drained.Value());
            } else {
                auto kept = orphanSemaphores_.TryPushBack(acquired);
                (void)kept;
            }
        }

        commands_[frameRecord_].fenceValue = RetainUntilDestructionFence;
        AssignPendingRetires(RetainUntilDestructionFence);
        timestamps_.Close(RetainUntilDestructionFence, false);
        dirtySlots_ |= usedThisFrame_;
        SettleTracking(false);
        return Result<GpuFencePoint>::Failure(uploads ? signaled.Error() : uploads.Error());
    }

    const uint64 value = signaled.Value();
    commands_[frameRecord_].fenceValue = value;
    AssignPendingRetires(value);
    timestamps_.Close(value, true);
    dirtySlots_ = (dirtySlots_ & ~resetThisFrame_) | usedThisFrame_;
    SettleTracking(true);
    if (acquired != VK_NULL_HANDLE) {
        ReturnAcquireSemaphore(acquired, value);
        VkPresentInfoKHR info = {};
        info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &present->renderDone[imageIndex];
        info.swapchainCount = 1;
        info.pSwapchains = &present->swapchain;
        info.pImageIndices = &imageIndex;
        result = functions_.vkQueuePresentKHR(queue_, &info);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            // 크기 경합이다. 프레임은 제출됐고 다음 EndFrame이 acquire 전에 다시 만든다.
            present->outOfDate = true;
        } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            present->surfaceLost = result == VK_ERROR_SURFACE_LOST_KHR;
            if (result == VK_ERROR_DEVICE_LOST) {
                Log::Error(LogVulkan, "Vulkan device lost during present on {Adapter}",
                           adapterName_.CStr());
            }

            outcome = Fail("vkQueuePresentKHR", result);
        }
    }

    ProcessRetired();
    if (!outcome) {
        return Result<GpuFencePoint>::Failure(outcome.Error());
    }

    return Result<GpuFencePoint>::Success(timeline_.Point(value));
}

bool VulkanDevice::StreamsBound() const noexcept {
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_);
    if (pipeline == nullptr || !insideRenderPass_) {
        return false;
    }

    for (uint32 stream = 0; stream < MaxVertexStreams; ++stream) {
        if ((pipeline->streamMask & (1u << stream)) == 0) {
            continue;
        }

        const BufferRecord* buffer = buffers_.Find(boundVertex_[stream].buffer.slot);
        if (buffer == nullptr || boundVertex_[stream].offset >= buffer->size ||
            boundVertex_[stream].offset % VertexOffsetAlignment != 0) {
            return false;
        }
    }

    return true;
}

void VulkanDevice::SetPipeline(PipelineHandle handle) noexcept {
    platform::LockGuard lock(gate_);
    const PipelineRecord* record = pipelines_.Find(handle.slot);
    if (record == nullptr || !recording_) {
        boundPipeline_ = {};
        return;
    }

    // 정점 stride는 파이프라인에 들어 있어 묶인 정점 버퍼가 새 stride로 읽힌다.
    functions_.vkCmdBindPipeline(frame_, VK_PIPELINE_BIND_POINT_GRAPHICS, record->pipeline);
    boundPipeline_ = handle.slot;
}

void VulkanDevice::SetVertexBuffers(uint32 firstStream,
                                    Span<const VertexBufferBinding> buffers) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return;
    }

    for (usize index = 0; index < buffers.Size(); ++index) {
        const usize stream = firstStream + index;
        if (stream >= MaxVertexStreams) {
            return;
        }

        boundVertex_[stream] = {buffers[index].buffer, buffers[index].offset};
        const BufferRecord* record = buffers_.Find(buffers[index].buffer.slot);
        if (record == nullptr || buffers[index].offset >= record->size ||
            buffers[index].offset % VertexOffsetAlignment != 0) {
            continue;
        }

        const VkDeviceSize offset = buffers[index].offset;
        functions_.vkCmdBindVertexBuffers(frame_, static_cast<uint32>(stream), 1, &record->buffer,
                                          &offset);
    }
}

void VulkanDevice::SetIndexBuffer(BufferHandle buffer, IndexFormat format, uint64 offset) noexcept {
    platform::LockGuard lock(gate_);
    boundIndex_ = {buffer, format, offset};
    const BufferRecord* record = buffers_.Find(buffer.slot);
    if (!recording_ || record == nullptr || offset >= record->size ||
        offset % IndexFormatSize(format) != 0) {
        return;
    }

    functions_.vkCmdBindIndexBuffer(frame_, record->buffer, offset,
                                    format == IndexFormat::UInt16 ? VK_INDEX_TYPE_UINT16
                                                                  : VK_INDEX_TYPE_UINT32);
}

void VulkanDevice::SetTexture(TextureHandle handle) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return;
    }

    TextureRecord* record = textures_.Find(handle.slot);
    SlotHandle slot = handle.slot;
    if (record == nullptr) {
        record = textures_.Find(white_.slot);
        slot = white_.slot;
    }

    if (record == nullptr) {
        return;
    }

    // 명시 전이 없이 그래픽 셰이더 읽기 상태가 아닌 텍스처를 읽으면 암묵 전이로 메운다(GX5. 렌더
    // 그래프를 거치지 않는 그리기용). 그래픽 읽기 비트가 있고 layout이 SHADER_READ_ONLY여야 한다
    // (descriptor를 그 layout으로 썼다. D3D12는 PIXEL_SHADER_RESOURCE 비트만 본다). 패스 대상
    // 자신은 attachment라 바꾸지 않는다(샘플링할 수 없다).
    bool readable = true;
    for (uint32 mip = 0; mip < record->mipLevels; ++mip) {
        readable = readable && HasAnyAccess(record->current[mip], ResourceAccess::SrvGraphics) &&
                   (record->current[mip] & ~SrvAccessMask) == ResourceAccess::Unknown;
    }

    if (!readable && !(target_.texture == TextureHandle{slot})) {
        TrackTransition(*record, slot, 0, record->mipLevels, ResourceAccess::SrvGraphics, false);
        ResumeRenderPass();
    }

    functions_.vkCmdBindDescriptorSets(frame_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1,
                                       1, &record->set, 0, nullptr);
}

void VulkanDevice::SetConstants(const DrawConstants& constants) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return;
    }

    // 행렬은 행 우선 그대로 올린다(셰이더는 -Zpr, ShaderSystem 7.2절 6항).
    functions_.vkCmdPushConstants(frame_, pipelineLayout_,
                                  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                  ConstantBytes, &constants);
}

void VulkanDevice::Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_ || !StreamsBound()) {
        ++ignoredDraws_;
        return;
    }

    // base instance는 0으로 고정한다(RenderBase 10.4절).
    functions_.vkCmdDraw(frame_, vertexCount, instanceCount, firstVertex, 0);
}

void VulkanDevice::DrawIndexed(uint32 indexCount, uint32 instanceCount,
                               uint32 firstIndex) noexcept {
    platform::LockGuard lock(gate_);
    const BufferRecord* index = buffers_.Find(boundIndex_.buffer.slot);
    if (!recording_ || !StreamsBound() || index == nullptr || boundIndex_.offset >= index->size ||
        boundIndex_.offset % IndexFormatSize(boundIndex_.format) != 0) {
        ++ignoredDraws_;
        return;
    }

    // base vertex·base instance는 0으로 고정한다.
    functions_.vkCmdDrawIndexed(frame_, indexCount, instanceCount, firstIndex, 0, 0);
}

Result<void> VulkanDevice::CopyPassTarget(TextureHandle destination) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const TextureRecord* record = textures_.Find(destination.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (!record->renderTarget || destination == target_.texture || record->width != target_.width ||
        record->height != target_.height || record->format != target_.format) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto load = RenderPassFor(target_.viewFormat, true);
    if (!load) {
        return Result<void>::Failure(load.Error());
    }

    // render pass 안에서는 복사할 수 없으므로 닫고 복사한 뒤 LOAD render pass로 이어 연다. 묶인
    // 파이프라인·descriptor·push constant·동적 상태는 명령 버퍼 상태라 그대로 남는다.
    if (insideRenderPass_) {
        functions_.vkCmdEndRenderPass(frame_);
        insideRenderPass_ = false;
    }

    // 원본(패스 대상)과 복사 대상은 지금(기록 중) 추적 상태에서 바꿨다가 되돌린다(상태 중립, GX5).
    // swapchain 중간 이미지는 추적 텍스처가 아니므로 attachment layout에서 바꾼다.
    const TextureRecord* source = textures_.Find(target_.texture.slot);
    const ResourceAccess sourceState =
        source != nullptr ? source->current[0] : ResourceAccess::RenderTarget;
    const ResourceAccess resting = record->current[0];
    (void)AccessBarrier(frame_, target_.image, 0, 1, sourceState, ResourceAccess::CopySource,
                        false);
    (void)AccessBarrier(frame_, record->image, 0, 1, resting, ResourceAccess::CopyDestination,
                        false);
    // 중간 이미지(UNORM)와 RenderTarget(SRGB)은 같은 크기 부류라 byte를 그대로 옮긴다.
    VkImageCopy region = {};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {target_.width, target_.height, 1};
    functions_.vkCmdCopyImage(frame_, target_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              record->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    (void)AccessBarrier(frame_, target_.image, 0, 1, ResourceAccess::CopySource, sourceState,
                        false);
    (void)AccessBarrier(frame_, record->image, 0, 1, ResourceAccess::CopyDestination, resting,
                        false);
    if (sourceState == ResourceAccess::RenderTarget) {
        BeginRenderPass(true, nullptr);
    }
    return Result<void>::Success();
}

} // namespace rocks::graphics::vulkan
