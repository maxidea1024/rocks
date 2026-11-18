#include "VulkanDeviceImpl.h"
#include "VulkanFormats.h"

#include <Platform/Memory.h>

// 장치 능력(RenderBase 4.4절 GX1), 버퍼 view·ReadBuffer(GX2), 그리기 descriptor set이다
// (Docs/Design/VulkanGraphics.ko.md 6절). 그리기 descriptor set은 파이프라인 layout마다 다르므로
// 그리기 직전에 기록(명령 버퍼)의 pool에서 받아 쓴다. pool은 그 기록을 다시 쓸 때(앞 제출의 fence가
// 끝났다) 통째로 reset한다(UE 비-bindless의 프레임 단위 pool과 같은 방식).
namespace rocks::graphics::vulkan {

void VulkanDevice::QueryDeviceCaps() noexcept {
    // maxMemoryAllocationSize는 1.1 core maintenance3 속성이다.
    VkPhysicalDeviceMaintenance3Properties maintenance3 = {};
    maintenance3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES;
    VkPhysicalDeviceProperties2 properties2 = {};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties2.pNext = &maintenance3;
    functions_.vkGetPhysicalDeviceProperties2(physical_, &properties2);
    maxAllocationBytes_ = maintenance3.maxMemoryAllocationSize;

    const VkPhysicalDeviceLimits& device = properties_.limits;
    StageResourceLimits stage;
    stage.constantBuffers = device.maxPerStageDescriptorUniformBuffers;
    stage.sampledTextures = device.maxPerStageDescriptorSampledImages;
    stage.samplers = device.maxPerStageDescriptorSamplers;
    stage.storageBuffers = device.maxPerStageDescriptorStorageBuffers;
    stage.storageImages = device.maxPerStageDescriptorStorageImages;
    stage.uniformVectors = NotApplicableLimit;
    DeviceLimits& limits = caps_.limits;
    limits.vertex = stage;
    limits.pixel = stage;
    limits.compute = stage;
    limits.pipelineSampledTextures = device.maxDescriptorSetSampledImages;
    limits.pipelineSamplers = device.maxDescriptorSetSamplers;
    limits.pipelineStorageBuffers = device.maxDescriptorSetStorageBuffers;
    limits.pipelineStorageImages = device.maxDescriptorSetStorageImages;
    limits.perStageResources = device.maxPerStageResources;
    limits.pipelineUavs = NotApplicableLimit;
    limits.boundDescriptorSets = device.maxBoundDescriptorSets;
    limits.colorAttachments = device.maxColorAttachments;
    limits.viewports = enabledFeatures_.multiViewport == VK_TRUE ? device.maxViewports : 1u;
    limits.constantBufferBytes = device.maxUniformBufferRange;
    limits.rootConstantBytes = device.maxPushConstantsSize;
    limits.constantBufferOffsetAlignment =
        static_cast<uint32>(device.minUniformBufferOffsetAlignment);
    limits.storageBufferOffsetAlignment =
        static_cast<uint32>(device.minStorageBufferOffsetAlignment);
    limits.texelBufferOffsetAlignment = static_cast<uint32>(device.minTexelBufferOffsetAlignment);
    limits.storageBufferBytes = device.maxStorageBufferRange;
    limits.texelBufferElements = device.maxTexelBufferElements;
    limits.bufferBytes = maxAllocationBytes_;
    limits.vertexAttributes = device.maxVertexInputAttributes;
    limits.vertexStreams = device.maxVertexInputBindings;
    limits.interStageVectors = device.maxVertexOutputComponents / 4;
    for (uint32 axis = 0; axis < 3; ++axis) {
        limits.computeGroupCount[axis] = device.maxComputeWorkGroupCount[axis];
        limits.computeGroupSize[axis] = device.maxComputeWorkGroupSize[axis];
    }

    limits.computeGroupInvocations = device.maxComputeWorkGroupInvocations;
    limits.computeSharedMemoryBytes = device.maxComputeSharedMemorySize;
    limits.textureSize2D = device.maxImageDimension2D;
    limits.textureSize3D = device.maxImageDimension3D;
    limits.textureSizeCube = device.maxImageDimensionCube;
    limits.textureArrayLayers = device.maxImageArrayLayers;
    limits.samplerAnisotropy = anisotropy_ ? static_cast<uint32>(device.maxSamplerAnisotropy) : 1;

    DeviceFeatures& features = caps_.features;
    features.computeShaders = true;
    features.depthClamp = enabledFeatures_.depthClamp == VK_TRUE;
    features.depthBiasClamp = enabledFeatures_.depthBiasClamp == VK_TRUE;
    features.alphaToCoverage = true;
    features.independentBlend = enabledFeatures_.independentBlend == VK_TRUE;
    const bool multiViewport = enabledFeatures_.multiViewport == VK_TRUE;
    features.vertexViewportLayerOutput = viewportLayer_ && multiViewport;
    features.atomic64 = atomic64_;
    features.splitTransitions = sync2_;
    features.rayTracingPipeline = rayTracingPipeline_;
    features.rayQuery = rayQuery_;
    features.indirectFirstInstance = enabledFeatures_.drawIndirectFirstInstance == VK_TRUE;
    features.multiDrawIndirect = enabledFeatures_.multiDrawIndirect == VK_TRUE;
    features.textureCubeArray = enabledFeatures_.imageCubeArray == VK_TRUE;
    // VkComponentMapping은 core다. AnisotropicPoint는 mipmapMode NEAREST에 이방성 배율을 더하므로
    // 늘 된다(배율은 samplerAnisotropy 한도로 줄인다).
    features.textureViewSwizzle = true;
    features.anisotropicPointFilter = true;
    // sRGB 짝 형식은 MUTABLE_FORMAT 이미지의 view로 읽는다(PixelFormat.ko.md 14.2절).
    features.srgbPairView = true;
    features.vertexStores = enabledFeatures_.vertexPipelineStoresAndAtomics == VK_TRUE;
    features.pixelStores = enabledFeatures_.fragmentStoresAndAtomics == VK_TRUE;
    // HDR 출력은 surface 형식·색 공간 질의가 필요하다(스왑체인 HDR 단계). 장치만으로는 모른다.
    features.hdrOutput = false;
    features.resourceBindingTier = 0;
    features.shaderModel = 0;
}

const DeviceCaps& VulkanDevice::Caps() const noexcept {
    return caps_;
}

uint32 VulkanDevice::GetPixelFormatSampleCounts(PixelFormat format) const noexcept {
    const uint32 number = PixelFormatNumber(format);
    return number <= MaxPixelFormatNumber ? sampleCounts_[number] : 0;
}

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation, 내부 잠금으로 직렬화한다).
Result<BufferViewHandle> VulkanDevice::CreateBufferView(BufferHandle handle,
                                                        const BufferViewDesc& desc) noexcept {
    platform::LockGuard lock(gate_);
    BufferRecord* buffer = buffers_.Find(handle.slot);
    if (buffer == nullptr) {
        return Result<BufferViewHandle>::Failure(ErrorCode::StaleHandle);
    }

    auto resolved = ResolveBufferView(buffer->desc, desc);
    if (!resolved) {
        auto failed = FailCode("CreateBufferView", resolved.Error(),
                               "buffer view breaks the view rules (kind, access, format or range)");
        return Result<BufferViewHandle>::Failure(failed.Error());
    }

    const ResolvedBufferView range = resolved.Value();
    auto limited =
        CheckBufferViewLimits(caps_.limits, GetPixelFormatCaps(desc.format), desc, range);
    if (!limited) {
        auto failed = FailCode("CreateBufferView", limited.Error(),
                               "buffer view needs a format capability, alignment or range the "
                               "Vulkan device does not provide");
        return Result<BufferViewHandle>::Failure(failed.Error());
    }

    // typed view 형식은 버퍼의 모든 texel usage(읽기 용도면 UNIFORM, 쓰기 용도면 STORAGE)를
    // 지원해야 한다(VUID-VkBufferViewCreateInfo-format-08778). 다른 형식으로 바꾸지 않고
    // Unsupported다.
    if (desc.kind == BufferViewKind::Typed) {
        PixelFormatCaps required = PixelFormatCaps::None;
        if (HasAnyBufferUsage(buffer->desc.usage, BufferUsage::ShaderRead)) {
            required = required | PixelFormatCaps::TexelBufferRead;
        }

        if (HasAnyBufferUsage(buffer->desc.usage, BufferUsage::ShaderWrite)) {
            required = required | PixelFormatCaps::TexelBufferWrite;
        }

        if (!HasPixelFormatCaps(GetPixelFormatCaps(desc.format), required)) {
            auto failed = FailCode("CreateBufferView", ErrorCode::Unsupported,
                                   "typed view format must support every texel usage of the "
                                   "buffer (read and write) on Vulkan");
            return Result<BufferViewHandle>::Failure(failed.Error());
        }
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<BufferViewHandle>::Failure(reserved.Error());
    }

    BufferViewRecord record;
    record.buffer = buffer->buffer;
    record.source = handle.slot;
    record.offset = range.offset;
    record.range = range.Bytes();
    record.format = desc.format;
    record.kind = desc.kind;
    record.access = desc.access;
    record.stride = desc.kind == BufferViewKind::Structured ? range.elementBytes : 0u;
    record.shaderType = desc.kind == BufferViewKind::Typed
                            ? GetPixelFormatInfo(desc.format).shaderType
                            : PixelShaderType::None;
    if (desc.kind == BufferViewKind::Typed) {
        VkBufferViewCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
        info.buffer = buffer->buffer;
        info.format = TextureFormat(desc.format).format;
        info.offset = range.offset;
        info.range = range.Bytes();
        const VkResult result =
            functions_.vkCreateBufferView(device_, &info, nullptr, &record.view);
        if (result != VK_SUCCESS) {
            return FailWith<BufferViewHandle>("vkCreateBufferView", result);
        }
    }

    const VkBufferView view = record.view;
    auto added = bufferViews_.TryAdd(Move(record));
    if (!added) {
        if (view != VK_NULL_HANDLE) {
            functions_.vkDestroyBufferView(device_, view, nullptr);
        }

        return Result<BufferViewHandle>::Failure(added.Error());
    }

    ++buffer->viewCount;
    return Result<BufferViewHandle>::Success(BufferViewHandle{added.Value()});
}

void VulkanDevice::ReleaseViewSource(const BufferViewRecord& view) noexcept {
    BufferRecord* source = buffers_.Find(view.source);
    if (source != nullptr) {
        --source->viewCount;
        return;
    }

    for (usize index = 0; index < orphanBuffers_.Size(); ++index) {
        OrphanBuffer& orphan = orphanBuffers_[index];
        if (orphan.buffer != view.buffer) {
            continue;
        }

        if (--orphan.viewCount == 0) {
            Retired entry;
            entry.buffer = orphan.buffer;
            entry.memory = orphan.memory;
            RetireEntry(Move(entry));
            auto removed = orphanBuffers_.TryRemoveAtSwap(index);
            (void)removed;
        }

        return;
    }
}

void VulkanDevice::Retire(BufferViewHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    BufferViewRecord* record = bufferViews_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }

    Retired entry;
    entry.bufferView = record->view;
    RetireEntry(Move(entry));
    ReleaseViewSource(*record);
    auto removed = bufferViews_.TryRemove(handle.slot);
    (void)removed;
}

// 버퍼 구간을 host 버퍼로 복사해 읽는다(시험·캡처용). 업로드 기록에 쌓으므로 앞선 업로드 뒤에
// 실행된다.
Result<void> VulkanDevice::ReadBuffer(BufferHandle handle, uint64 offset,
                                      Span<uint8> out) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return owner;
    }

    platform::LockGuard lock(gate_);
    const BufferRecord* record = buffers_.Find(handle.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (recording_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (out.Size() == 0 || offset > record->size || out.Size() > record->size - offset) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return reserved;
    }

    Retired readback;
    auto created = CreateBufferWithMemory(out.Size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          MemoryUsage::Readback, readback.buffer, readback.memory);
    if (!created) {
        return created;
    }

    auto opened = OpenUploadList();
    if (!opened) {
        DestroyRetired(readback);
        return opened;
    }

    RecordBufferCopy(record->buffer, offset, readback.buffer, 0, out.Size());
    VkBufferMemoryBarrier host = {};
    host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = readback.buffer;
    host.size = VK_WHOLE_SIZE;
    functions_.vkCmdPipelineBarrier(upload_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0,
                                    nullptr);
    auto flushed = FlushUploadsLocked();
    if (!flushed) {
        KeepUntilDestruction(Move(readback));
        return Result<void>::Failure(flushed.Error());
    }

    Result<void> outcome = WaitFenceOwner(flushed.Value());
    if (outcome) {
        {
            platform::LockGuard memoryLock(memoryGate_);
            memory_.Invalidate(readback.memory);
        }

        platform::MemoryCopy(out.Data(), readback.memory.mapped, out.Size());
    }

    RetireEntry(Move(readback));
    return outcome;
}

void VulkanDevice::SetBufferView(uint32 slot, BufferViewHandle handle) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_ || slot >= MaxDrawBufferViews) {
        return;
    }

    // 그래픽 단계에는 UAV가 없으므로 ReadWrite view와 stale handle은 slot을 비운다.
    const BufferViewRecord* view = bufferViews_.Find(handle.slot);
    const bool readable = view != nullptr && view->access == ViewAccess::Read;
    boundViews_[slot] = readable ? handle : BufferViewHandle{};
    descriptorsDirty_ = true;
}

Result<VkDescriptorSet> VulkanDevice::AllocateFrameSet(VkDescriptorSetLayout layout) noexcept {
    CommandRecord& record = commands_[frameRecord_];
    VkDescriptorSetAllocateInfo allocate = {};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &layout;
    while (record.poolCursor < MaxDescriptorPoolsPerRecord) {
        if (record.poolCursor == record.poolCount) {
            // 새 pool은 앞 pool의 두 배다(한 프레임이 많이 그리면 pool 수가 아니라 크기로
            // 늘어난다).
            const uint32 sets = DescriptorSetsPerPool << record.poolCount;
            const VkDescriptorPoolSize sizes[] = {
                {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sets},
                {VK_DESCRIPTOR_TYPE_SAMPLER, sets},
                {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, sets * 2},
                {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, sets},
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets * 2}};
            VkDescriptorPoolCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            info.maxSets = sets;
            info.poolSizeCount = static_cast<uint32>(sizeof(sizes) / sizeof(sizes[0]));
            info.pPoolSizes = sizes;
            VkDescriptorPool created = VK_NULL_HANDLE;
            const VkResult result =
                functions_.vkCreateDescriptorPool(device_, &info, nullptr, &created);
            if (result != VK_SUCCESS) {
                return FailWith<VkDescriptorSet>("vkCreateDescriptorPool", result);
            }

            record.pools[record.poolCount++] = created;
        }

        allocate.descriptorPool = record.pools[record.poolCursor];
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult result = functions_.vkAllocateDescriptorSets(device_, &allocate, &set);
        if (result == VK_SUCCESS) {
            return Result<VkDescriptorSet>::Success(set);
        }

        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) {
            return FailWith<VkDescriptorSet>("vkAllocateDescriptorSets", result);
        }

        ++record.poolCursor;
    }

    // 상한(DescriptorSetsPerPool × (2^MaxDescriptorPoolsPerRecord - 1))을 넘은 그리기는 무시된다.
    // 조용히 지나가지 않게 처음 한 번 경고를 남기고 횟수를 센다.
    if (descriptorExhaustions_++ == 0) {
        Log::Warning(LogGraphicsVulkan,
                     "Vulkan draw descriptor pools of a frame are exhausted; further draws in the "
                     "frame are ignored");
    }

    auto failed = FailCode("AllocateFrameSet", ErrorCode::CapacityExceeded,
                           "draw descriptor set pools of this frame are exhausted");
    return Result<VkDescriptorSet>::Failure(failed.Error());
}

void VulkanDevice::ResetRecordPools(CommandRecord& record) noexcept {
    for (uint32 index = 0; index < record.poolCount; ++index) {
        (void)functions_.vkResetDescriptorPool(device_, record.pools[index], 0);
    }

    record.poolCursor = 0;
    // 앞 제출이 끝났고 그 기록 끝에서 event를 reset했으므로 다시 쓴다.
    record.eventCursor = 0;
}

void VulkanDevice::DestroyRecordPools() noexcept {
    for (CommandRecord& record : commands_) {
        for (uint32 index = 0; index < record.poolCount; ++index) {
            functions_.vkDestroyDescriptorPool(device_, record.pools[index], nullptr);
        }

        record.poolCount = 0;
        record.poolCursor = 0;
        for (uint32 index = 0; index < record.eventCount; ++index) {
            functions_.vkDestroyEvent(device_, record.events[index], nullptr);
        }

        record.eventCount = 0;
        record.eventCursor = 0;
    }
}

bool VulkanDevice::PrepareDescriptors(VkPipelineBindPoint bindPoint) noexcept {
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_);
    if (pipeline == nullptr) {
        return false;
    }

    // 묶인 view가 파이프라인 slot 배치를 채우는지는 매 그리기 본다(view가 Retire되면 stale이다).
    BoundBufferViewInfo bound[MaxDrawBufferViews] = {};
    BoundBufferViewInfo written[MaxDrawBufferViews] = {};
    const BufferViewRecord* views[MaxDrawBufferViews] = {};
    const BufferViewRecord* writeViews[MaxDrawBufferViews] = {};
    for (uint32 slot = 0; slot < MaxDrawBufferViews; ++slot) {
        views[slot] = bufferViews_.Find(boundViews_[slot].slot);
        if (views[slot] != nullptr) {
            bound[slot] = {true, views[slot]->kind, views[slot]->shaderType, views[slot]->stride};
        }

        writeViews[slot] = bufferViews_.Find(boundWriteViews_[slot].slot);
        if (writeViews[slot] != nullptr) {
            written[slot] = {true, writeViews[slot]->kind, writeViews[slot]->shaderType,
                             writeViews[slot]->stride};
        }
    }

    if (!DrawBufferSlotsSatisfied(pipeline->bufferSlots, bound) ||
        !DrawBufferSlotsSatisfied(pipeline->writeSlots, written)) {
        return false;
    }

    if (!descriptorsDirty_) {
        return true;
    }

    const TextureRecord* texture = textures_.Find(boundTexture_.slot);
    if (texture == nullptr) {
        texture = textures_.Find(white_.slot);
    }

    VkDescriptorImageInfo images[MaxPipelineBindings] = {};
    VkDescriptorBufferInfo buffers[MaxPipelineBindings] = {};
    VkBufferView texelViews[MaxPipelineBindings] = {};
    VkWriteDescriptorSet writes[MaxPipelineBindings] = {};
    for (uint32 set = 0; set < 2; ++set) {
        if ((pipeline->setMask & (1u << set)) == 0) {
            continue;
        }

        auto allocated = AllocateFrameSet(pipeline->setLayouts[set]);
        if (!allocated) {
            return false;
        }

        uint32 count = 0;
        for (uint32 index = 0; index < pipeline->bindingCount; ++index) {
            const PipelineBinding& binding = pipeline->bindings[index];
            if (binding.set != set) {
                continue;
            }

            VkWriteDescriptorSet& write = writes[count];
            write = {};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = allocated.Value();
            write.dstBinding = binding.binding;
            write.descriptorCount = 1;
            write.descriptorType = binding.type;
            if (binding.source == BindingSource::Texture) {
                images[count] = {VK_NULL_HANDLE,
                                 texture != nullptr ? texture->view : VK_NULL_HANDLE,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                write.pImageInfo = &images[count];
            } else if (binding.source == BindingSource::Sampler) {
                images[count] = {texture != nullptr ? texture->sampler : VK_NULL_HANDLE,
                                 VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
                write.pImageInfo = &images[count];
            } else {
                const BufferViewRecord* view = binding.source == BindingSource::WritableBufferView
                                                   ? writeViews[binding.slot]
                                                   : views[binding.slot];
                if (binding.type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                    binding.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                    texelViews[count] = view->view;
                    write.pTexelBufferView = &texelViews[count];
                } else {
                    buffers[count] = {view->buffer, view->offset, view->range};
                    write.pBufferInfo = &buffers[count];
                }
            }

            ++count;
        }

        functions_.vkUpdateDescriptorSets(device_, count, writes, 0, nullptr);
        const VkDescriptorSet descriptorSet = allocated.Value();
        functions_.vkCmdBindDescriptorSets(frame_, bindPoint, pipeline->layout, set, 1,
                                           &descriptorSet, 0, nullptr);
    }

    descriptorsDirty_ = false;
    return true;
}

} // namespace rocks::graphics::vulkan
