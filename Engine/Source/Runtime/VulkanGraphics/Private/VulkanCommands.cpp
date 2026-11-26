#include "VulkanDeviceImpl.h"

#include <Platform/Memory.h>

// 프레임 기록(CommandContext)과 EndFrame이다(Docs/Design/VulkanGraphics.ko.md 8·9·11절).
namespace rocks::graphics::vulkan {

namespace {

struct VulkanCommandsStageAccess {
    VkPipelineStageFlags stage = 0;
    VkAccessFlags access = 0;
};

// layout을 떠날 때 기다릴 단계와 쓰기다(그 layout에서 일어났던 일).
VulkanCommandsStageAccess SourceOf(VkImageLayout layout) noexcept {
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
VulkanCommandsStageAccess DestinationOf(VkImageLayout layout) noexcept {
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
    const VulkanCommandsStageAccess source = SourceOf(from);
    const VulkanCommandsStageAccess destination = DestinationOf(to);
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
    // 앞 제출이 끝난 기록이므로 그 그리기 descriptor set도 더 쓰이지 않는다.
    ResetRecordPools(record);
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
    for (uint32 stage = 0; stage < ConstantBufferStageCount; ++stage) {
        for (uint32 slot = 0; slot < MaxResourceSlots; ++slot) {
            boundTextures_[stage][slot] = {};
            boundSamplers_[stage][slot] = VK_NULL_HANDLE;
        }
    }

    for (BufferViewHandle& view : boundViews_) {
        view = {};
    }

    for (BufferViewHandle& view : boundWriteViews_) {
        view = {};
    }

    for (TextureHandle& view : boundWriteTextures_) {
        view = {};
    }

    for (auto& stage : boundConstants_) {
        for (BoundConstant& bound : stage) {
            bound = {};
        }
    }

    descriptorsDirty_ = true;
    ResetTimestampSlots();
    return Result<void>::Success();
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
    target_ = {};
    target_.image = record->target;
    target_.framebuffer = record->framebuffer;
    target_.viewFormat = VK_FORMAT_R8G8B8A8_SRGB;
    target_.restLayout = record->targetLayout;
    target_.width = record->width;
    target_.height = record->height;
    target_.format = PixelFormat::RGBA8UnormSrgb;
    target_.colorCount = 1;
    target_.key.colorCount = 1;
    target_.key.colors[0] = VK_FORMAT_R8G8B8A8_SRGB;
    target_.key.colorLoad[0] = VK_ATTACHMENT_LOAD_OP_CLEAR;
    target_.key.colorStore[0] = VK_ATTACHMENT_STORE_OP_STORE;
    target_.clears[0].color.float32[0] = clear.r;
    target_.clears[0].color.float32[1] = clear.g;
    target_.clears[0].color.float32[2] = clear.b;
    target_.clears[0].color.float32[3] = clear.a;
    presentTarget_ = handle;
    renderPassSuspended_ = false;
    StartPass();
    return Result<CommandContext*>::Success(this);
}

// 패스 끝에 MSAA 색 attachment를 resolve 대상에 박스 평균으로 resolve한다(ColorAttachment::
// resolveTarget, 결정 R17). render pass를 닫은 뒤 원본은 CopySource(TRANSFER_SRC), 대상은
// CopyDestination(TRANSFER_DST)으로 바꿔 vkCmdResolveImage를 기록한다. subpass resolve attachment는
// render pass 호환 조건이라 쓰지 않는다(RenderBase 4.4절 텍스처 GX2 B부).
void VulkanDevice::ResolveAttachments() noexcept {
    for (uint32 index = 0; index < target_.colorCount; ++index) {
        if (target_.resolves[index].IsNull()) {
            continue;
        }

        TextureRecord* source = textures_.Find(target_.colors[index].slot);
        TextureRecord* resolved = textures_.Find(target_.resolves[index].slot);
        if (source == nullptr || resolved == nullptr) {
            continue;
        }

        // MSAA 원본은 mip 1이라 그 attachment 층(2D는 0)이고, 대상은 resolve mip·slice 하나다.
        const uint32 sourceSlice = target_.colorSlices[index];
        const ResolvedSubresourceRange whole = {0, 1, sourceSlice, 1};
        const ResolvedSubresourceRange destination = AttachmentSubresource(
            resolved->dimension, target_.resolveMips[index], target_.resolveSlices[index]);
        TrackTransition(*source, target_.colors[index].slot, whole, ResourceAccess::CopySource,
                        false);
        TrackTransition(*resolved, target_.resolves[index].slot, destination,
                        ResourceAccess::ResolveDestination, false);
        VkImageResolve region = {};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, sourceSlice, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, destination.firstMip,
                                 destination.firstSlice, 1};
        region.extent = {source->width, source->height, 1};
        functions_.vkCmdResolveImage(frame_, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     resolved->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                     &region);
        // 패스 뒤 상태 계약: 원본은 attachment 상태로 되돌리고 대상은 ResolveDestination으로 둔다
        // (RenderBase 8절 RG1 남은 것 설계 2).
        TrackTransition(*source, target_.colors[index].slot, whole, ResourceAccess::RenderTarget,
                        false);
    }
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

    ResolveAttachments();

    CloseSplitTransitions();

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
        constantRing_.CloseBatch(fenceValue_);
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
        constantRing_.CloseBatch(RetainUntilDestructionFence);
        AssignPendingRetires(RetainUntilDestructionFence);
        timestamps_.Close(RetainUntilDestructionFence, false);
        dirtySlots_ |= usedThisFrame_;
        SettleTracking(false);
        return Result<GpuFencePoint>::Failure(uploads ? signaled.Error() : uploads.Error());
    }

    const uint64 value = signaled.Value();
    commands_[frameRecord_].fenceValue = value;
    constantRing_.CloseBatch(value);
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
                Log::Error(LogGraphicsVulkan, "Vulkan device lost during present on {Adapter}",
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
    if (pipeline == nullptr || pipeline->compute || !insideRenderPass_) {
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

    // 정점 stride는 파이프라인에 들어 있어 묶인 정점 버퍼가 새 stride로 읽힌다. descriptor set
    // layout은 파이프라인마다 다르므로 다음 그리기가 새로 묶는다.
    functions_.vkCmdBindPipeline(
        frame_, record->compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
        record->pipeline);
    boundPipeline_ = handle.slot;
    descriptorsDirty_ = true;
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
    if (!recording_ || record == nullptr || offset >= record->size || !IsValidIndexFormat(format) ||
        offset % IndexFormatSize(format) != 0) {
        return;
    }

    functions_.vkCmdBindIndexBuffer(frame_, record->buffer, offset,
                                    format == IndexFormat::UInt16 ? VK_INDEX_TYPE_UINT16
                                                                  : VK_INDEX_TYPE_UINT32);
}

void VulkanDevice::SetTexture(ShaderStage stage, uint32 binding, TextureHandle handle) noexcept {
    platform::LockGuard lock(gate_);
    const uint32 stageIndex = ConstantBufferStageIndex(stage);
    if (!recording_ || stageIndex >= ConstantBufferStageCount || binding >= MaxResourceSlots) {
        return;
    }

    TextureRecord* record = textures_.Find(handle.slot);
    SlotHandle slot = handle.slot;
    if (record == nullptr || record->view == VK_NULL_HANDLE || record->writable) {
        record = textures_.Find(white_.slot);
        slot = white_.slot;
        ++nullTextureBindings_;
    }

    if (record == nullptr) {
        return;
    }

    // view는 image view만 자기 것이고 이미지 상태는 원 텍스처 기록이 추적한다. descriptor는 다음
    // 그리기가 쓴다(PrepareDescriptors).
    boundTextures_[stageIndex][binding] = slot;
    descriptorsDirty_ = true;
    // view는 자기 mip·slice 범위만 전이한다(텍스처 GX2 A부).
    const ResolvedSubresourceRange range = record->viewRange;
    if (record->isView) {
        slot = record->source;
        record = textures_.Find(slot);
        if (record == nullptr) {
            return;
        }
    }

    // 명시 전이 없이 셰이더 읽기 상태가 아닌 텍스처를 읽으면 암묵 전이로 메운다(GX5. 렌더 그래프를
    // 거치지 않는 그리기용). 묶인 파이프라인 종류의 읽기 비트(compute는 SrvCompute, 그 밖은
    // SrvGraphics)가 있고 layout이 SHADER_READ_ONLY여야 한다(descriptor를 그 layout으로 썼다.
    // D3D12도 같은 규칙이다). 그래서 compute 파이프라인을 먼저 묶어야 그래프가 낸 SrvCompute를 다시
    // 바꾸지 않는다(RG1 남은 것 B부). 패스 대상 자신은 attachment라 바꾸지 않는다(샘플링할 수
    // 없다).
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_);
    const ResourceAccess readState = pipeline != nullptr && pipeline->compute
                                         ? ResourceAccess::SrvCompute
                                         : ResourceAccess::SrvGraphics;
    bool readable = true;
    for (uint32 layer = range.firstSlice; layer < range.firstSlice + range.sliceCount; ++layer) {
        for (uint32 mip = range.firstMip; mip < range.firstMip + range.mipCount; ++mip) {
            const ResourceAccess state = record->current.At(mip, layer);
            readable = readable && HasAnyAccess(state, readState) &&
                       (state & ~SrvAccessMask) == ResourceAccess::Unknown;
        }
    }

    if (!readable && !OverlapsPassAttachment(slot, range)) {
        TrackTransition(*record, slot, range, readState, false);
        ResumeRenderPass();
    }
}

Result<void> VulkanDevice::SetSampler(ShaderStage stage, uint32 slot,
                                      const SamplerDesc* sampler) noexcept {
    platform::LockGuard lock(gate_);
    const uint32 stageIndex = ConstantBufferStageIndex(stage);
    if (!recording_ || stageIndex >= ConstantBufferStageCount || slot >= MaxResourceSlots ||
        (sampler != nullptr && !IsValidSamplerDesc(*sampler))) {
        return FailCode("SetSampler", ErrorCode::InvalidArgument,
                        "needs a recording, a stage and slot in range and a valid sampler "
                        "description");
    }

    VkSampler bound = VK_NULL_HANDLE;
    if (sampler != nullptr) {
        auto made = SamplerFor(*sampler);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        bound = made.Value();
    }

    boundSamplers_[stageIndex][slot] = bound;
    descriptorsDirty_ = true;
    return Result<void>::Success();
}

const TextureRecord* VulkanDevice::BoundTexture(uint32 stage, uint32 slot) const noexcept {
    const TextureRecord* texture =
        slot < MaxResourceSlots ? textures_.Find(boundTextures_[stage][slot]) : nullptr;
    if (texture == nullptr || texture->view == VK_NULL_HANDLE || texture->writable) {
        texture = textures_.Find(white_.slot);
    }

    return texture;
}

Result<TextureViewInfo> VulkanDevice::GetTextureViewInfo(TextureHandle texture) noexcept {
    platform::LockGuard lock(gate_);
    const TextureRecord* record = textures_.Find(texture.slot);
    if (record == nullptr) {
        return Result<TextureViewInfo>::Failure(ErrorCode::StaleHandle);
    }

    TextureViewInfo info;
    info.dimension = record->dimension;
    info.format = record->format;
    info.sampleCount = record->sampleCount;
    info.access = record->writable ? ViewAccess::ReadWrite : ViewAccess::Read;
    info.sampled = record->view != VK_NULL_HANDLE && !record->writable;
    info.comparison = record->comparison;
    return Result<TextureViewInfo>::Success(info);
}

Result<BufferViewInfo> VulkanDevice::GetBufferViewInfo(BufferViewHandle view) noexcept {
    platform::LockGuard lock(gate_);
    const BufferViewRecord* record = bufferViews_.Find(view.slot);
    if (record == nullptr) {
        return Result<BufferViewInfo>::Failure(ErrorCode::StaleHandle);
    }

    return Result<BufferViewInfo>::Success(
        BufferViewInfo{record->kind, record->access, record->shaderType, record->stride});
}

Result<void> VulkanDevice::SetDrawConstants(Span<const uint8> bytes) noexcept {
    platform::LockGuard lock(gate_);
    const PipelineRecord* pipeline = recording_ ? pipelines_.Find(boundPipeline_) : nullptr;
    if (pipeline == nullptr || bytes.Size() != pipeline->drawConstantBytes) {
        return RejectDrawConstantSize(pipeline != nullptr, bytes.Size(),
                                      pipeline != nullptr ? pipeline->drawConstantBytes : 0u);
    }

    // 모든 layout의 push constant 범위가 같으므로 공용 layout으로 앞 n byte만 올린다. 값은 다른
    // 파이프라인(compute 포함)으로 바꿔도 남는다. 행렬은 행 우선 그대로다(셰이더는 -Zpr,
    // ShaderSystem 7.2절 6항).
    if (bytes.Size() != 0) {
        functions_.vkCmdPushConstants(frame_, pipelineLayout_, ConstantStages, 0,
                                      static_cast<uint32>(bytes.Size()), bytes.Data());
    }

    return Result<void>::Success();
}

void VulkanDevice::Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept {
    platform::LockGuard lock(gate_);
    if (!PrepareDraw(false)) {
        ++ignoredDraws_;
        return;
    }

    // base instance는 0으로 고정한다(RenderBase 10.4절).
    functions_.vkCmdDraw(frame_, vertexCount, instanceCount, firstVertex, 0);
}

void VulkanDevice::DrawIndexed(uint32 indexCount, uint32 instanceCount,
                               uint32 firstIndex) noexcept {
    platform::LockGuard lock(gate_);
    if (!PrepareDraw(true)) {
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

    // 패스 대상은 2D mip 0 attachment여야 한다(이미지 mip 0 복사, 텍스처 GX2 C부).
    const TextureRecord* passTexture = textures_.Find(target_.texture.slot);
    const bool wholeTarget =
        passTexture == nullptr ||
        (passTexture->dimension == TextureDimension::Texture2D && target_.colorMips[0] == 0);
    if (target_.image == VK_NULL_HANDLE || !record->renderTarget || record->sampleCount > 1 ||
        target_.sampleCount > 1 || !wholeTarget || record->mipLevels != 1 ||
        record->dimension != TextureDimension::Texture2D || destination == target_.texture ||
        record->width != target_.width || record->height != target_.height ||
        record->format != target_.format) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
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
    if (sourceState == ResourceAccess::RenderTarget && AttachmentsReady()) {
        BeginRenderPass(true);
    }
    return Result<void>::Success();
}

} // namespace rocks::graphics::vulkan
