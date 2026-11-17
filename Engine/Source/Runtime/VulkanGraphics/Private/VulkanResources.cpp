#include "VulkanDeviceImpl.h"
#include "VulkanFormats.h"

// 버퍼·텍스처·샘플러·descriptor set과 Retire다(Docs/Design/VulkanGraphics.ko.md 6절).
namespace rocks::graphics::vulkan {

namespace {

VkSamplerAddressMode AddressMode(graphics::AddressMode mode) noexcept {
    switch (mode) {
    case graphics::AddressMode::Wrap:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case graphics::AddressMode::Clamp:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case graphics::AddressMode::Mirror:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case graphics::AddressMode::Border:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }

    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

// 세 고정 border 색은 Vulkan 내장 색과 일대일이다.
VkBorderColor BorderColorOf(graphics::BorderColor color) noexcept {
    switch (color) {
    case graphics::BorderColor::TransparentBlack:
        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    case graphics::BorderColor::OpaqueBlack:
        return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    case graphics::BorderColor::OpaqueWhite:
        return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    }

    return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
}

} // namespace

MemoryAllocation VulkanDevice::AllocateMemoryLocked(const MemoryRequest& request, MemoryUsage usage,
                                                    ResourceShape shape, VkResult& result,
                                                    ErrorCode& error) noexcept {
    platform::LockGuard lock(memoryGate_);
    auto allocated = memory_.Allocate(request, usage, shape, result);
    if (!allocated) {
        error = allocated.Error();
        return MemoryAllocation();
    }

    error = ErrorCode::None;
    return allocated.Value();
}

void VulkanDevice::FreeMemoryLocked(const MemoryAllocation& allocation) noexcept {
    if (allocation.IsNull()) {
        return;
    }

    platform::LockGuard lock(memoryGate_);
    memory_.Free(allocation);
}

Result<void> VulkanDevice::CreateBufferWithMemory(uint64 size, VkBufferUsageFlags usage,
                                                  MemoryUsage memory, VkBuffer& buffer,
                                                  MemoryAllocation& allocation) noexcept {
    VkBufferCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = functions_.vkCreateBuffer(device_, &info, nullptr, &buffer);
    if (result != VK_SUCCESS) {
        buffer = VK_NULL_HANDLE;
        return Fail("vkCreateBuffer", result);
    }

    VkMemoryDedicatedRequirements dedicated = {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    VkMemoryRequirements2 requirements = {};
    requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    requirements.pNext = &dedicated;
    VkBufferMemoryRequirementsInfo2 query = {};
    query.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
    query.buffer = buffer;
    functions_.vkGetBufferMemoryRequirements2(device_, &query, &requirements);
    MemoryRequest request;
    request.size = requirements.memoryRequirements.size;
    request.alignment = requirements.memoryRequirements.alignment;
    request.typeBits = requirements.memoryRequirements.memoryTypeBits;
    request.dedicated = dedicated.requiresDedicatedAllocation == VK_TRUE ||
                        dedicated.prefersDedicatedAllocation == VK_TRUE;
    request.buffer = buffer;
    ErrorCode error = ErrorCode::None;
    allocation = AllocateMemoryLocked(request, memory, ResourceShape::Linear, result, error);
    if (allocation.IsNull()) {
        functions_.vkDestroyBuffer(device_, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        auto failed = Fail("vkAllocateMemory", result);
        (void)failed;
        return Result<void>::Failure(error);
    }

    result = functions_.vkBindBufferMemory(device_, buffer, allocation.memory, allocation.offset);
    if (result != VK_SUCCESS) {
        functions_.vkDestroyBuffer(device_, buffer, nullptr);
        FreeMemoryLocked(allocation);
        buffer = VK_NULL_HANDLE;
        allocation = MemoryAllocation();
        return Fail("vkBindBufferMemory", result);
    }

    return Result<void>::Success();
}

Result<void> VulkanDevice::CreateImageWithMemory(const VkImageCreateInfo& info, VkImage& image,
                                                 MemoryAllocation& memory) noexcept {
    VkResult result = functions_.vkCreateImage(device_, &info, nullptr, &image);
    if (result != VK_SUCCESS) {
        image = VK_NULL_HANDLE;
        return Fail("vkCreateImage", result);
    }

    VkMemoryDedicatedRequirements dedicated = {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    VkMemoryRequirements2 requirements = {};
    requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    requirements.pNext = &dedicated;
    VkImageMemoryRequirementsInfo2 query = {};
    query.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    query.image = image;
    functions_.vkGetImageMemoryRequirements2(device_, &query, &requirements);
    MemoryRequest request;
    request.size = requirements.memoryRequirements.size;
    request.alignment = requirements.memoryRequirements.alignment;
    request.typeBits = requirements.memoryRequirements.memoryTypeBits;
    request.dedicated = dedicated.requiresDedicatedAllocation == VK_TRUE ||
                        dedicated.prefersDedicatedAllocation == VK_TRUE;
    request.image = image;
    ErrorCode error = ErrorCode::None;
    memory =
        AllocateMemoryLocked(request, MemoryUsage::Device, ResourceShape::Optimal, result, error);
    if (memory.IsNull()) {
        functions_.vkDestroyImage(device_, image, nullptr);
        image = VK_NULL_HANDLE;
        auto failed = Fail("vkAllocateMemory", result);
        (void)failed;
        return Result<void>::Failure(error);
    }

    result = functions_.vkBindImageMemory(device_, image, memory.memory, memory.offset);
    if (result != VK_SUCCESS) {
        functions_.vkDestroyImage(device_, image, nullptr);
        FreeMemoryLocked(memory);
        image = VK_NULL_HANDLE;
        memory = MemoryAllocation();
        return Fail("vkBindImageMemory", result);
    }

    return Result<void>::Success();
}

Result<VkSampler> VulkanDevice::SamplerFor(const SamplerDesc& desc) noexcept {
    for (uint32 index = 0; index < samplerCount_; ++index) {
        if (samplers_[index].desc == desc) {
            return Result<VkSampler>::Success(samplers_[index].sampler);
        }
    }

    if (samplerCount_ >= SamplerCapacity) {
        return Result<VkSampler>::Failure(ErrorCode::CapacityExceeded);
    }

    // UE 대응: Point만 NEAREST, mip은 Trilinear·Anisotropic만 LINEAR(VulkanState.cpp:16–48).
    const bool nearest = desc.filter == Filter::Nearest;
    const bool linearMip = desc.filter == Filter::Trilinear || desc.filter == Filter::Anisotropic;
    VkSamplerCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.minFilter = info.magFilter;
    info.mipmapMode = linearMip ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = AddressMode(desc.addressU);
    info.addressModeV = AddressMode(desc.addressV);
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    // 이방성은 장치 한계로 자르고, 기능이 없거나 1이면 끈다(SamplerDesc 계약 "장치 한계로 줄인다").
    float anisotropy = static_cast<float>(desc.maxAnisotropy);
    if (anisotropy > properties_.limits.maxSamplerAnisotropy) {
        anisotropy = properties_.limits.maxSamplerAnisotropy;
    }

    info.anisotropyEnable =
        desc.filter == Filter::Anisotropic && anisotropy_ && anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = info.anisotropyEnable == VK_TRUE ? anisotropy : 1.0f;
    info.compareOp = VK_COMPARE_OP_NEVER;
    info.minLod = 0.0f;
    info.maxLod = VK_LOD_CLAMP_NONE;
    info.borderColor = BorderColorOf(desc.borderColor);
    VkSampler sampler = VK_NULL_HANDLE;
    const VkResult result = functions_.vkCreateSampler(device_, &info, nullptr, &sampler);
    if (result != VK_SUCCESS) {
        return FailWith<VkSampler>("vkCreateSampler", result);
    }

    samplers_[samplerCount_++] = {desc, sampler};
    return Result<VkSampler>::Success(sampler);
}

Result<void> VulkanDevice::AllocateTextureSet(VkImageView view, VkSampler sampler,
                                              VkDescriptorSet& set, uint32& pool) noexcept {
    VkDescriptorSetAllocateInfo allocate = {};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &textureSetLayout_;
    set = VK_NULL_HANDLE;
    for (uint32 index = 0; index < descriptorPools_.Size() && set == VK_NULL_HANDLE; ++index) {
        allocate.descriptorPool = descriptorPools_[index];
        const VkResult result = functions_.vkAllocateDescriptorSets(device_, &allocate, &set);
        if (result == VK_SUCCESS) {
            pool = index;
        } else if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) {
            set = VK_NULL_HANDLE;
            return Fail("vkAllocateDescriptorSets", result);
        } else {
            set = VK_NULL_HANDLE;
        }
    }

    if (set == VK_NULL_HANDLE) {
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, DescriptorSetsPerPool},
            {VK_DESCRIPTOR_TYPE_SAMPLER, DescriptorSetsPerPool}};
        VkDescriptorPoolCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = DescriptorSetsPerPool;
        info.poolSizeCount = 2;
        info.pPoolSizes = sizes;
        VkDescriptorPool created = VK_NULL_HANDLE;
        VkResult result = functions_.vkCreateDescriptorPool(device_, &info, nullptr, &created);
        if (result != VK_SUCCESS) {
            return Fail("vkCreateDescriptorPool", result);
        }

        auto pushed = descriptorPools_.TryPushBack(created);
        if (!pushed) {
            functions_.vkDestroyDescriptorPool(device_, created, nullptr);
            return pushed;
        }

        allocate.descriptorPool = created;
        result = functions_.vkAllocateDescriptorSets(device_, &allocate, &set);
        if (result != VK_SUCCESS) {
            set = VK_NULL_HANDLE;
            return Fail("vkAllocateDescriptorSets", result);
        }

        pool = static_cast<uint32>(descriptorPools_.Size() - 1);
    }

    VkDescriptorImageInfo image = {};
    image.imageView = view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo samplerInfo = {};
    samplerInfo.sampler = sampler;
    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = &image;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[1].pImageInfo = &samplerInfo;
    functions_.vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    return Result<void>::Success();
}

void VulkanDevice::FreeTextureSet(VkDescriptorSet set, uint32 pool) noexcept {
    if (set != VK_NULL_HANDLE && pool < descriptorPools_.Size()) {
        (void)functions_.vkFreeDescriptorSets(device_, descriptorPools_[pool], 1, &set);
    }
}

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation). 초기 데이터는 업로드 ring에 쓰고
// 복사를 기록한다. 소유 스레드의 다음 제출이 실행한다.
Result<BufferHandle> VulkanDevice::CreateBuffer(const BufferDesc& desc,
                                                Span<const uint8> initial) noexcept {
    if (desc.size == 0 || initial.Size() != desc.size ||
        (desc.usage != BufferUsage::Vertex && desc.usage != BufferUsage::Index) ||
        desc.size > NumericLimits<uint32>::Max()) {
        return Result<BufferHandle>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(gate_);
    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<BufferHandle>::Failure(reserved.Error());
    }

    const VkBufferUsageFlags usage =
        (desc.usage == BufferUsage::Vertex ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                                           : VK_BUFFER_USAGE_INDEX_BUFFER_BIT) |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    BufferRecord record;
    record.size = desc.size;
    auto created =
        CreateBufferWithMemory(desc.size, usage, MemoryUsage::Device, record.buffer, record.memory);
    if (!created) {
        return Result<BufferHandle>::Failure(created.Error());
    }

    const VkBuffer buffer = record.buffer;
    const MemoryAllocation memory = record.memory;
    auto added = buffers_.TryAdd(Move(record));
    if (!added) {
        functions_.vkDestroyBuffer(device_, buffer, nullptr);
        FreeMemoryLocked(memory);
        return Result<BufferHandle>::Failure(added.Error());
    }

    auto copied = RecordBufferUpload(buffer, 0, initial);
    if (!copied) {
        // 업로드 기록이 이미 이 버퍼를 참조했을 수 있으므로 회수 경로로 놓는다.
        BufferRecord* stored = buffers_.Find(added.Value());
        Retired entry;
        entry.buffer = stored->buffer;
        entry.memory = stored->memory;
        RetireEntry(Move(entry));
        auto removed = buffers_.TryRemove(added.Value());
        (void)removed;
        return Result<BufferHandle>::Failure(copied.Error());
    }

    return Result<BufferHandle>::Success(BufferHandle{added.Value()});
}

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation). 초기 픽셀은 업로드 기록으로 간다.
Result<TextureHandle> VulkanDevice::CreateTexture(const TextureDesc& desc,
                                                  Span<const uint8> pixels) noexcept {
    const bool renderTarget = desc.usage == TextureUsage::RenderTarget;
    const uint64 size = MipChainBytes(desc.width, desc.height, desc.mipLevels, desc.format);
    const uint32 limit = properties_.limits.maxImageDimension2D < MaxTextureExtent
                             ? properties_.limits.maxImageDimension2D
                             : MaxTextureExtent;
    if (desc.width > limit || desc.height > limit || size == 0 || desc.mipLevels > MaxMipLevels ||
        !IsValidSamplerDesc(desc.sampler) ||
        (renderTarget ? (pixels.Size() != 0 || desc.mipLevels != 1) : pixels.Size() != size)) {
        return Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
    }

    const TextureFormatInfo format = TextureFormat(desc.format);
    VkFormatProperties support = {};
    functions_.vkGetPhysicalDeviceFormatProperties(physical_, format.format, &support);
    const VkFormatFeatureFlags required =
        renderTarget ? format.renderTargetFeatures : format.sampledFeatures;
    if (format.format == VK_FORMAT_UNDEFINED ||
        (support.optimalTilingFeatures & required) != required) {
        return Result<TextureHandle>::Failure(ErrorCode::Unsupported);
    }

    platform::LockGuard lock(gate_);
    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<TextureHandle>::Failure(reserved.Error());
    }

    auto sampler = SamplerFor(desc.sampler);
    if (!sampler) {
        return Result<TextureHandle>::Failure(sampler.Error());
    }

    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format.format;
    info.extent = {desc.width, desc.height, 1};
    info.mipLevels = desc.mipLevels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (renderTarget) {
        info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    TextureRecord record;
    record.width = desc.width;
    record.height = desc.height;
    record.mipLevels = desc.mipLevels;
    record.format = desc.format;
    record.renderTarget = renderTarget;
    auto image = CreateImageWithMemory(info, record.image, record.memory);
    if (!image) {
        return Result<TextureHandle>::Failure(image.Error());
    }

    // 생성 중 실패하면 아직 GPU가 참조하지 않으므로 바로 파괴한다.
    Retired cleanup;
    cleanup.image = record.image;
    cleanup.memory = record.memory;
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = record.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format.format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, desc.mipLevels, 0, 1};
    VkResult result = functions_.vkCreateImageView(device_, &viewInfo, nullptr, &record.view);
    if (result != VK_SUCCESS) {
        DestroyRetired(cleanup);
        return FailWith<TextureHandle>("vkCreateImageView", result);
    }

    cleanup.view = record.view;
    auto set = AllocateTextureSet(record.view, sampler.Value(), record.set, record.pool);
    if (!set) {
        DestroyRetired(cleanup);
        return Result<TextureHandle>::Failure(set.Error());
    }

    cleanup.set = record.set;
    cleanup.setPool = record.pool;
    if (renderTarget) {
        auto renderPass = RenderPassFor(format.format, false);
        if (!renderPass) {
            DestroyRetired(cleanup);
            return Result<TextureHandle>::Failure(renderPass.Error());
        }

        VkFramebufferCreateInfo framebuffer = {};
        framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass = renderPass.Value();
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &record.view;
        framebuffer.width = desc.width;
        framebuffer.height = desc.height;
        framebuffer.layers = 1;
        result =
            functions_.vkCreateFramebuffer(device_, &framebuffer, nullptr, &record.framebuffer);
        if (result != VK_SUCCESS) {
            record.framebuffer = VK_NULL_HANDLE;
            DestroyRetired(cleanup);
            return FailWith<TextureHandle>("vkCreateFramebuffer", result);
        }

        cleanup.framebuffer = record.framebuffer;
    }

    auto added = textures_.TryAdd(Move(record));
    if (!added) {
        DestroyRetired(cleanup);
        return Result<TextureHandle>::Failure(added.Error());
    }

    const TextureRecord& stored = *textures_.Find(added.Value());
    Result<void> uploaded = Result<void>::Success();
    if (renderTarget) {
        // 쉬는 layout(SHADER_READ_ONLY)으로 바꿔 둔다. 다음 제출보다 먼저 실행된다.
        uploaded = OpenUploadList();
        if (uploaded) {
            Barrier(upload_, stored.image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    } else {
        uploaded = RecordTextureUpload(stored, 0, stored.mipLevels, pixels, true);
    }

    if (!uploaded) {
        // 업로드 기록이 이미 참조했을 수 있으므로 회수 경로로 놓는다.
        RetireEntry(Move(cleanup));
        auto removed = textures_.TryRemove(added.Value());
        (void)removed;
        return Result<TextureHandle>::Failure(uploaded.Error());
    }

    return Result<TextureHandle>::Success(TextureHandle{added.Value()});
}

void VulkanDevice::Retire(BufferHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    BufferRecord* record = buffers_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }

    Retired entry;
    entry.buffer = record->buffer;
    entry.memory = record->memory;
    RetireEntry(Move(entry));
    auto removed = buffers_.TryRemove(handle.slot);
    (void)removed;
}

void VulkanDevice::Retire(TextureHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    TextureRecord* record = textures_.Find(handle.slot);
    if (record == nullptr || handle == white_) {
        return;
    }

    Retired entry;
    entry.image = record->image;
    entry.view = record->view;
    entry.memory = record->memory;
    entry.framebuffer = record->framebuffer;
    entry.set = record->set;
    entry.setPool = record->pool;
    RetireEntry(Move(entry));
    auto removed = textures_.TryRemove(handle.slot);
    (void)removed;
}

void VulkanDevice::Retire(PipelineHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    PipelineRecord* record = pipelines_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }

    Retired entry;
    entry.pipeline = record->pipeline;
    RetireEntry(Move(entry));
    auto removed = pipelines_.TryRemove(handle.slot);
    (void)removed;
}

} // namespace rocks::graphics::vulkan
