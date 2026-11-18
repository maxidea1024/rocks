#include "VulkanDeviceImpl.h"

// compute 파이프라인과 GX4 명령 기록이다: dispatch·간접 그리기·간접 dispatch,
// viewport·scissor·스텐실 기준, 버퍼·텍스처 복사, UAV clear(Docs/Design/VulkanGraphics.ko.md
// 8·11절). 버퍼를 쓰는 명령은 render pass 밖에서 기록하고 앞뒤에 전역 메모리 barrier를 둔다(렌더
// 그래프 밖 기록의 보수적 순서).
namespace rocks::graphics::vulkan {

namespace {

// 버퍼를 읽거나 쓰는 모든 단계다(간접 인자·정점 입력·셰이더·복사).
constexpr VkPipelineStageFlags BufferStages =
    VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
constexpr VkAccessFlags BufferAccess =
    VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
    VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
    VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;

} // namespace

Result<PipelineHandle>
VulkanDevice::CreateComputePipeline(const ComputePipelineDesc& desc) noexcept {
    platform::LockGuard lock(gate_);
    if (desc.shader.hlsl.ByteCount() == 0 || !ValidateComputePipelineDesc(desc)) {
        auto failed = Reject("compute pipeline needs HLSL source and valid drawConstantBytes");
        return Result<PipelineHandle>::Failure(failed.Error());
    }

    if (shaderCompiler_ == nullptr) {
        auto failed = FailCode("CreateComputePipeline", ErrorCode::Unsupported,
                               "Vulkan needs cooked SPIR-V (ShaderSystem SH2) or a development "
                               "shader compiler (DeviceDesc::shaderCompiler)");
        return Result<PipelineHandle>::Failure(failed.Error());
    }

    CompiledShader shader(allocator_);
    lastError_.Clear();
    auto compiled =
        shaderCompiler_->CompileSpirv(ShaderStage::Compute, desc.shader.hlsl, shader, lastError_);
    if (!compiled) {
        return Result<PipelineHandle>::Failure(compiled.Error());
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<PipelineHandle>::Failure(reserved.Error());
    }

    PipelineRecord record;
    record.compute = true;
    record.drawConstantBytes = desc.drawConstantBytes;
    auto layout = BuildPipelineLayout(shader, nullptr, true, record);
    if (!layout) {
        return Result<PipelineHandle>::Failure(layout.Error());
    }

    auto module = CreateShaderModule(shader);
    if (!module) {
        DestroyPipelineLayout(record);
        return Result<PipelineHandle>::Failure(module.Error());
    }

    VkComputePipelineCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module.Value();
    info.stage.pName = "CSMain";
    info.layout = record.layout;
    const VkResult result = functions_.vkCreateComputePipelines(device_, pipelineCache_, 1, &info,
                                                                nullptr, &record.pipeline);
    functions_.vkDestroyShaderModule(device_, module.Value(), nullptr);
    if (result != VK_SUCCESS) {
        DestroyPipelineLayout(record);
        return FailWith<PipelineHandle>("vkCreateComputePipelines", result);
    }

    const VkPipeline pipeline = record.pipeline;
    PipelineRecord cleanup = record;
    auto added = pipelines_.TryAdd(Move(record));
    if (!added) {
        functions_.vkDestroyPipeline(device_, pipeline, nullptr);
        DestroyPipelineLayout(cleanup);
        return Result<PipelineHandle>::Failure(added.Error());
    }

    return Result<PipelineHandle>::Success(PipelineHandle{added.Value()});
}

void VulkanDevice::SetWritableBufferView(uint32 slot, BufferViewHandle view) noexcept {
    platform::LockGuard lock(gate_);
    if (slot >= MaxDrawBufferViews) {
        return;
    }

    const BufferViewRecord* record = bufferViews_.Find(view.slot);
    const bool writable = record != nullptr && record->access == ViewAccess::ReadWrite;
    boundWriteViews_[slot] = writable ? view : BufferViewHandle{};
    // 쓰기 slot은 버퍼·텍스처가 같은 번호 공간이다(나중에 묶은 하나만 남는다).
    boundWriteTextures_[slot] = {};
    descriptorsDirty_ = true;
}

void VulkanDevice::SetWritableTexture(uint32 slot, TextureHandle view) noexcept {
    platform::LockGuard lock(gate_);
    if (slot >= MaxDrawBufferViews) {
        return;
    }

    const TextureRecord* record = textures_.Find(view.slot);
    const bool writable = record != nullptr && record->writable;
    boundWriteTextures_[slot] = writable ? view : TextureHandle{};
    boundWriteViews_[slot] = {};
    descriptorsDirty_ = true;
}

bool VulkanDevice::PrepareDraw(bool indexed) noexcept {
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_);
    if (!recording_ || pipeline == nullptr || pipeline->compute || !StreamsBound() ||
        (target_.depthReadOnly && pipeline->writesDepthStencil)) {
        return false;
    }

    if (indexed) {
        const BufferRecord* index = buffers_.Find(boundIndex_.buffer.slot);
        if (index == nullptr || boundIndex_.offset >= index->size ||
            boundIndex_.offset % IndexFormatSize(boundIndex_.format) != 0) {
            return false;
        }
    }

    return PrepareDescriptors(VK_PIPELINE_BIND_POINT_GRAPHICS);
}

bool VulkanDevice::PrepareDispatch() noexcept {
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_);
    if (!recording_ || pipeline == nullptr || !pipeline->compute ||
        !PrepareDescriptors(VK_PIPELINE_BIND_POINT_COMPUTE)) {
        return false;
    }

    // 쓰는 텍스처 view의 범위를 UAV(GENERAL)로 맞춘다. 같은 상태가 이어지면 UAV 순서 barrier로 앞
    // dispatch의 쓰기를 기다린다.
    for (uint32 slot = 0; slot < MaxDrawBufferViews; ++slot) {
        if ((pipeline->writeTextures & (1u << slot)) == 0) {
            continue;
        }

        const TextureRecord* view = textures_.Find(boundWriteTextures_[slot].slot);
        TextureRecord* owner = view != nullptr ? textures_.Find(view->source) : nullptr;
        if (owner != nullptr) {
            TrackTransition(*owner, view->source, view->viewRange, ResourceAccess::UavCompute,
                            true);
        }
    }

    // dispatch는 render pass 밖에서만 기록할 수 있다. 뒤 그리기를 위해 LOAD로 다시 연다.
    SuspendRenderPass();
    BeforeBufferWrite(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    return true;
}

void VulkanDevice::BeforeBufferWrite(VkPipelineStageFlags writeStage) noexcept {
    // 앞선 읽기가 끝난 뒤에 쓴다(WAR, 실행 의존만). 앞선 쓰기는 그 명령 뒤 barrier가 덮었다.
    functions_.vkCmdPipelineBarrier(frame_, BufferStages, writeStage, 0, 0, nullptr, 0, nullptr, 0,
                                    nullptr);
}

void VulkanDevice::AfterBufferWrite(VkPipelineStageFlags writeStage) noexcept {
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = writeStage == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                ? VK_ACCESS_SHADER_WRITE_BIT
                                : VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = BufferAccess;
    functions_.vkCmdPipelineBarrier(frame_, writeStage, BufferStages, 0, 1, &barrier, 0, nullptr, 0,
                                    nullptr);
}

VkBuffer VulkanDevice::IndirectArguments(BufferHandle arguments, uint64 offset,
                                         uint64 bytes) noexcept {
    const BufferRecord* record = buffers_.Find(arguments.slot);
    if (record == nullptr || !HasAnyBufferUsage(record->desc.usage, BufferUsage::Indirect) ||
        offset % 4 != 0 || offset > record->size || record->size - offset < bytes) {
        return VK_NULL_HANDLE;
    }

    return record->buffer;
}

void VulkanDevice::Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) noexcept {
    platform::LockGuard lock(gate_);
    const uint32 counts[3] = {groupCountX, groupCountY, groupCountZ};
    for (uint32 axis = 0; axis < 3; ++axis) {
        if (counts[axis] > caps_.limits.computeGroupCount[axis]) {
            ++ignoredDraws_;
            return;
        }
    }

    if (!PrepareDispatch()) {
        ++ignoredDraws_;
        return;
    }

    functions_.vkCmdDispatch(frame_, groupCountX, groupCountY, groupCountZ);
    AfterBufferWrite(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ResumeRenderPass();
}

void VulkanDevice::DispatchIndirect(BufferHandle arguments, uint64 offset) noexcept {
    platform::LockGuard lock(gate_);
    const VkBuffer buffer = IndirectArguments(arguments, offset, sizeof(DispatchIndirectArguments));
    if (buffer == VK_NULL_HANDLE || !PrepareDispatch()) {
        ++ignoredDraws_;
        return;
    }

    functions_.vkCmdDispatchIndirect(frame_, buffer, offset);
    AfterBufferWrite(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ResumeRenderPass();
}

void VulkanDevice::DrawIndirect(BufferHandle arguments, uint64 offset) noexcept {
    platform::LockGuard lock(gate_);
    const VkBuffer buffer = IndirectArguments(arguments, offset, sizeof(DrawIndirectArguments));
    if (buffer == VK_NULL_HANDLE || !PrepareDraw(false)) {
        ++ignoredDraws_;
        return;
    }

    functions_.vkCmdDrawIndirect(frame_, buffer, offset, 1, sizeof(DrawIndirectArguments));
}

void VulkanDevice::DrawIndexedIndirect(BufferHandle arguments, uint64 offset) noexcept {
    platform::LockGuard lock(gate_);
    const VkBuffer buffer =
        IndirectArguments(arguments, offset, sizeof(DrawIndexedIndirectArguments));
    if (buffer == VK_NULL_HANDLE || !PrepareDraw(true)) {
        ++ignoredDraws_;
        return;
    }

    functions_.vkCmdDrawIndexedIndirect(frame_, buffer, offset, 1,
                                        sizeof(DrawIndexedIndirectArguments));
}

void VulkanDevice::SetViewport(const Viewport& viewport) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_ || viewport.width <= 0.0f || viewport.height <= 0.0f) {
        return;
    }

    // NDC y 위를 화면 위로 두는 음수 높이다(StartPass와 같다).
    VkViewport value = {};
    value.x = viewport.x;
    value.y = viewport.y + viewport.height;
    value.width = viewport.width;
    value.height = -viewport.height;
    value.minDepth = viewport.minDepth;
    value.maxDepth = viewport.maxDepth;
    functions_.vkCmdSetViewport(frame_, 0, 1, &value);
}

void VulkanDevice::SetScissor(const ScissorRect& scissor) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return;
    }

    VkRect2D rect = {};
    rect.offset = {static_cast<int32>(scissor.x), static_cast<int32>(scissor.y)};
    rect.extent = {scissor.width, scissor.height};
    functions_.vkCmdSetScissor(frame_, 0, 1, &rect);
}

void VulkanDevice::SetStencilReference(uint32 reference) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return;
    }

    functions_.vkCmdSetStencilReference(frame_, VK_STENCIL_FACE_FRONT_AND_BACK, reference);
}

Result<void> VulkanDevice::CopyBuffer(BufferHandle destination, uint64 destinationOffset,
                                      BufferHandle source, uint64 sourceOffset,
                                      uint64 size) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const BufferRecord* to = buffers_.Find(destination.slot);
    const BufferRecord* from = buffers_.Find(source.slot);
    if (to == nullptr || from == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (size == 0 || destination == source || destinationOffset % 4 != 0 || sourceOffset % 4 != 0 ||
        size % 4 != 0 || destinationOffset > to->size || to->size - destinationOffset < size ||
        sourceOffset > from->size || from->size - sourceOffset < size) {
        return FailCode("CopyBuffer", ErrorCode::InvalidArgument,
                        "buffer copy range is empty, out of range, unaligned or within one buffer");
    }

    SuspendRenderPass();
    BeforeBufferWrite(VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferCopy region = {};
    region.srcOffset = sourceOffset;
    region.dstOffset = destinationOffset;
    region.size = size;
    functions_.vkCmdCopyBuffer(frame_, from->buffer, to->buffer, 1, &region);
    AfterBufferWrite(VK_PIPELINE_STAGE_TRANSFER_BIT);
    ResumeRenderPass();
    return Result<void>::Success();
}

Result<void> VulkanDevice::CopyTexture(TextureHandle destination, TextureHandle source) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    TextureRecord* to = textures_.Find(destination.slot);
    TextureRecord* from = textures_.Find(source.slot);
    if (to == nullptr || from == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    const PixelFormatInfo& info = GetPixelFormatInfo(from->format);
    if (destination == source || to->isView || from->isView || to->width != from->width ||
        to->height != from->height || to->format != from->format || info.planeCount > 1 ||
        to->dimension != TextureDimension::Texture2D ||
        from->dimension != TextureDimension::Texture2D || IsPassAttachment(destination.slot) ||
        IsPassAttachment(source.slot)) {
        return FailCode("CopyTexture", ErrorCode::InvalidArgument,
                        "texture copy needs two distinct non-view single-plane 2D textures of the "
                        "same size and format outside the current pass");
    }

    // 두 텍스처의 mip 0을 복사 상태로 바꾼다(TrackTransition이 render pass를 닫는다). 상태는 복사
    // 상태로 남는다(GX5: 다음 사용이 명시 전이나 암묵 전이로 바꾼다).
    SuspendRenderPass();
    TrackTransition(*from, source.slot, {0, 1, 0, 1}, ResourceAccess::CopySource, false);
    TrackTransition(*to, destination.slot, {0, 1, 0, 1}, ResourceAccess::CopyDestination, false);
    VkImageCopy region = {};
    region.srcSubresource = {from->aspect, 0, 0, 1};
    region.dstSubresource = {to->aspect, 0, 0, 1};
    region.extent = {from->width, from->height, 1};
    functions_.vkCmdCopyImage(frame_, from->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ResumeRenderPass();
    return Result<void>::Success();
}

Result<void> VulkanDevice::ClearWritableBufferView(BufferViewHandle view, uint32 value) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const BufferViewRecord* record = bufferViews_.Find(view.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (record->access != ViewAccess::ReadWrite) {
        return FailCode("ClearWritableBufferView", ErrorCode::InvalidArgument,
                        "only writable views can be cleared");
    }

    const bool clearable =
        record->kind == BufferViewKind::Raw ||
        (record->kind == BufferViewKind::Typed &&
         (record->format == PixelFormat::R32Uint || record->format == PixelFormat::R32Sint));
    if (!clearable) {
        return FailCode("ClearWritableBufferView", ErrorCode::Unsupported,
                        "only raw and R32Uint or R32Sint typed views can be cleared");
    }

    // 32-bit 원소마다 value를 채운다(vkCmdFillBuffer, offset·크기는 4 byte 배수).
    SuspendRenderPass();
    BeforeBufferWrite(VK_PIPELINE_STAGE_TRANSFER_BIT);
    functions_.vkCmdFillBuffer(frame_, record->buffer, record->offset, record->range, value);
    AfterBufferWrite(VK_PIPELINE_STAGE_TRANSFER_BIT);
    ResumeRenderPass();
    return Result<void>::Success();
}

} // namespace rocks::graphics::vulkan
