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

    // UE 대응(VulkanState.cpp:16–48): Point만 NEAREST, mip은 Trilinear·AnisotropicLinear만
    // LINEAR이고 AnisotropicPoint는 NEAREST mip에 이방성을 더한다.
    const bool nearest = desc.filter == Filter::Point;
    const bool linearMip =
        desc.filter == Filter::Trilinear || desc.filter == Filter::AnisotropicLinear;
    VkSamplerCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.minFilter = info.magFilter;
    info.mipmapMode = linearMip ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = AddressMode(desc.addressU);
    info.addressModeV = AddressMode(desc.addressV);
    info.addressModeW = AddressMode(desc.addressW);
    // 이방성은 장치 한계로 자르고, 기능이 없거나 1이면 끈다(SamplerDesc 계약 "장치 한계로 줄인다").
    float anisotropy = static_cast<float>(desc.maxAnisotropy);
    if (anisotropy > properties_.limits.maxSamplerAnisotropy) {
        anisotropy = properties_.limits.maxSamplerAnisotropy;
    }

    info.anisotropyEnable =
        IsAnisotropicFilter(desc.filter) && anisotropy_ && anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = info.anisotropyEnable == VK_TRUE ? anisotropy : 1.0f;
    // VkCompareOp는 CompareFunction과 같은 순서다(NEVER = 0).
    info.compareEnable = desc.comparison ? VK_TRUE : VK_FALSE;
    info.compareOp = desc.comparison ? static_cast<VkCompareOp>(desc.compare) : VK_COMPARE_OP_NEVER;
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

// 용도를 Vulkan buffer usage로 옮긴다. typed view는 texel buffer, 구조화·raw view는 storage
// buffer usage가 필요하다(raw 버퍼는 typed view도 받는다). 복사 원본은 ReadBuffer용이다.
VkBufferUsageFlags VulkanDevice::BufferUsageFor(BufferUsage use) noexcept {
    const bool storageLayout = HasAnyBufferUsage(use, BufferUsage::Structured | BufferUsage::Raw);
    const bool typedViews = !HasAnyBufferUsage(use, BufferUsage::Structured);
    VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        (HasAnyBufferUsage(use, BufferUsage::Vertex) ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT : 0u) |
        (HasAnyBufferUsage(use, BufferUsage::Index) ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : 0u) |
        (HasAnyBufferUsage(use, BufferUsage::Constant) ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : 0u) |
        (HasAnyBufferUsage(use, BufferUsage::Indirect) ? VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT : 0u);
    if (HasAnyBufferUsage(use, BufferUsage::ShaderRead)) {
        usage |= (storageLayout ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0u) |
                 (typedViews ? VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT : 0u);
    }

    if (HasAnyBufferUsage(use, BufferUsage::ShaderWrite)) {
        usage |= (storageLayout ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0u) |
                 (typedViews ? VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT : 0u);
    }

    return usage;
}

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation). 초기 데이터는 업로드 ring에 쓰고
// 복사를 기록한다. 소유 스레드의 다음 제출이 실행한다.
Result<BufferHandle> VulkanDevice::CreateBuffer(const BufferDesc& desc,
                                                Span<const uint8> initial) noexcept {
    return CreateBufferIn(desc, initial, nullptr);
}

Result<BufferHandle> VulkanDevice::CreateBufferIn(const BufferDesc& desc, Span<const uint8> initial,
                                                  const Placement* placement) noexcept {
    // placed 버퍼는 초기 데이터가 없다(내용은 획득 뒤 쓴다).
    auto valid = ValidateBufferDesc(desc);
    if (!valid || initial.Size() != (placement != nullptr ? 0u : desc.size)) {
        return Result<BufferHandle>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(gate_);
    if (desc.size > caps_.limits.bufferBytes) {
        auto failed = FailCode("CreateBuffer", ErrorCode::Unsupported,
                               "buffer size exceeds the device limit (bufferBytes)");
        return Result<BufferHandle>::Failure(failed.Error());
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<BufferHandle>::Failure(reserved.Error());
    }

    const VkBufferUsageFlags usage = BufferUsageFor(desc.usage);
    BufferRecord record;
    record.size = desc.size;
    record.desc = desc;
    if (placement != nullptr) {
        VkBufferCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = desc.size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        const VkResult result = functions_.vkCreateBuffer(device_, &info, nullptr, &record.buffer);
        if (result != VK_SUCCESS) {
            return FailWith<BufferHandle>("vkCreateBuffer", result);
        }

        auto bound =
            BindPlaced(*placement, TransientHeapKind::Buffers, VK_NULL_HANDLE, record.buffer);
        if (!bound) {
            functions_.vkDestroyBuffer(device_, record.buffer, nullptr);
            return Result<BufferHandle>::Failure(bound.Error());
        }

        record.heap = placement->heap;
    } else {
        auto created = CreateBufferWithMemory(desc.size, usage, MemoryUsage::Device, record.buffer,
                                              record.memory);
        if (!created) {
            return Result<BufferHandle>::Failure(created.Error());
        }
    }

    const VkBuffer buffer = record.buffer;
    const MemoryAllocation memory = record.memory;
    const SlotHandle heap = record.heap;
    auto added = buffers_.TryAdd(Move(record));
    if (!added) {
        functions_.vkDestroyBuffer(device_, buffer, nullptr);
        FreeMemoryLocked(memory);
        if (!heap.IsNull()) {
            ReleasePlacement(heap);
        }

        return Result<BufferHandle>::Failure(added.Error());
    }

    // placed 버퍼는 업로드하지 않는다(heap 구간을 다른 자원이 쓰는 중일 수 있다).
    if (placement != nullptr) {
        return Result<BufferHandle>::Success(BufferHandle{added.Value()});
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

// 형식마다 능력을 한 번 채운다(논리 장치 생성 직후, PixelFormat.ko.md 7절 Vulkan 열).
void VulkanDevice::QueryFormatCaps() noexcept {
    constexpr VkFormatFeatureFlags Transfer =
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (uint32 number = 1; number <= MaxPixelFormatNumber; ++number) {
        const auto pixelFormat = static_cast<PixelFormat>(number);
        const TextureFormatInfo format =
            IsValidPixelFormat(pixelFormat) ? TextureFormat(pixelFormat) : TextureFormatInfo{};
        if (format.format == VK_FORMAT_UNDEFINED) {
            continue;
        }

        // ASTC HDR은 확장 형식이므로 확장을 켠 장치에서만 묻는다.
        if (format.format >= VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK &&
            format.format <= VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK && !astcHdr_) {
            continue;
        }

        const PixelFormatInfo& info = GetPixelFormatInfo(pixelFormat);
        const bool depth =
            info.kind == PixelFormatKind::Depth || info.kind == PixelFormatKind::DepthStencil;
        const bool planar = info.kind == PixelFormatKind::Planar;
        // multi-planar 이미지는 samplerYcbcrConversion 기능을 켠 장치에서만 만들 수 있다.
        if (planar && !ycbcr_) {
            continue;
        }

        VkFormatProperties support = {};
        functions_.vkGetPhysicalDeviceFormatProperties(physical_, format.format, &support);
        const VkFormatFeatureFlags features = support.optimalTilingFeatures;
        // 샘플·필터 기능은 셰이더가 읽는 view 형식에서 본다(비디오는 plane 형식, 둘 다 있어야
        // 한다).
        VkFormatFeatureFlags shader = features;
        if (planar) {
            VkFormatProperties luma = {};
            VkFormatProperties chroma = {};
            functions_.vkGetPhysicalDeviceFormatProperties(physical_, format.planeFormats[0],
                                                           &luma);
            functions_.vkGetPhysicalDeviceFormatProperties(physical_, format.planeFormats[1],
                                                           &chroma);
            shader = luma.optimalTilingFeatures & chroma.optimalTilingFeatures;
        }

        PixelFormatCaps caps = PixelFormatCaps::None;
        bool mipmaps = false;
        // 성분 대응이 항등인지다. A8(R8 + swizzle)은 swizzle을 쓸 수 없는 렌더 타깃·storage·texel
        // buffer로 공개하지 않는다.
        const bool identity = format.components.r == VK_COMPONENT_SWIZZLE_IDENTITY &&
                              format.components.a == VK_COMPONENT_SWIZZLE_IDENTITY;
        const VkImageCreateFlags flags = ImageFlagsFor(pixelFormat);
        const VkImageUsageFlags sampledUsage = ImageUsageFor(pixelFormat, false);
        // 깊이 형식은 늘 DEPTH_STENCIL_ATTACHMENT usage로 만든다(D3D와 같은 규칙).
        const bool sampled =
            (shader & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0 &&
            (features & Transfer) == Transfer &&
            (!depth || (features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) &&
            ImageSupported(format.format, sampledUsage, flags, mipmaps);
        if (sampled) {
            caps = caps | PixelFormatCaps::Sampled;
            if (mipmaps && !planar) {
                caps = caps | PixelFormatCaps::Mipmaps;
            }

            if ((shader & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0) {
                caps = caps | PixelFormatCaps::Filterable;
            }

            // gather는 UE와 같이 shaderImageGatherExtended 기능으로 판정한다(PixelFormat.ko.md
            // 7절).
            if (gather_) {
                caps = caps | PixelFormatCaps::Gather;
            }

            // 3D·cube는 같은 usage로 그 차원의 이미지를 만들 수 있는지 묻는다(생성 API는 A0-G).
            VkImageFormatProperties dimension = {};
            if (!planar && functions_.vkGetPhysicalDeviceImageFormatProperties(
                               physical_, format.format, VK_IMAGE_TYPE_3D, VK_IMAGE_TILING_OPTIMAL,
                               sampledUsage, flags, &dimension) == VK_SUCCESS) {
                caps = caps | PixelFormatCaps::Texture3D;
            }

            if (!planar &&
                functions_.vkGetPhysicalDeviceImageFormatProperties(
                    physical_, format.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                    sampledUsage, flags | VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                    &dimension) == VK_SUCCESS &&
                dimension.maxArrayLayers >= 6) {
                caps = caps | PixelFormatCaps::TextureCube;
            }

            // 성분 대응이 항등이 아닌 형식(A8의 swizzle 에뮬레이션)은 렌더 타깃으로 쓰지 않는다.
            bool unused = false;
            if (identity && !depth && !planar &&
                (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0 &&
                ImageSupported(format.format, ImageUsageFor(pixelFormat, true), flags, unused)) {
                caps = caps | PixelFormatCaps::RenderTarget;
                if ((features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0) {
                    caps = caps | PixelFormatCaps::Blendable;
                }
            }

            if (depth) {
                caps = caps | PixelFormatCaps::DepthStencil;
            }

            // MSAA 표본 수는 렌더 타깃·깊이와 같은 usage의 이미지 형식 질의 sampleCounts다
            // (VkSampleCountFlags 비트가 Rocks 표본 수 비트와 같다, 장치 framebuffer·sampled
            // 표본 한도가 이미 반영된다).
            const bool target = HasPixelFormatCaps(caps, PixelFormatCaps::RenderTarget) || depth;
            VkImageFormatProperties multisample = {};
            if (target &&
                functions_.vkGetPhysicalDeviceImageFormatProperties(
                    physical_, format.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                    ImageUsageFor(pixelFormat, !depth), flags, &multisample) == VK_SUCCESS) {
                sampleCounts_[number] =
                    (multisample.sampleCounts & (MaxSampleCount * 2 - 1)) | SampleCountBit(1);
                if (sampleCounts_[number] > SampleCountBit(1)) {
                    caps = caps | PixelFormatCaps::Multisample;
                }
            }
        }

        // storage image·texel buffer는 형식 기능 bit를 옮긴다.
        if (identity && !depth && !planar) {
            if ((features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0) {
                caps = caps | PixelFormatCaps::StorageRead | PixelFormatCaps::StorageWrite;
            }

            if ((features & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT) != 0) {
                caps = caps | PixelFormatCaps::StorageAtomic;
            }

            // 버퍼 view 형식은 버퍼의 모든 texel usage를 지원해야 한다
            // (VUID-VkBufferViewCreateInfo-format-08778, view별 usage는 1.4 maintenance5).
            // 읽기·쓰기 버퍼(ShaderRead | ShaderWrite)의 쓰기 view가 늘 만들어지도록 쓰기·atomic은
            // 읽기도 되는 형식만 공개한다(보수적 값).
            const VkFormatFeatureFlags buffer = support.bufferFeatures;
            const bool texelRead = (buffer & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) != 0;
            if (texelRead) {
                caps = caps | PixelFormatCaps::TexelBufferRead;
            }

            if (texelRead && (buffer & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT) != 0) {
                caps = caps | PixelFormatCaps::TexelBufferWrite;
            }

            if (texelRead && (buffer & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT) != 0) {
                caps = caps | PixelFormatCaps::TexelBufferAtomic;
            }
        }

        formatCaps_[number] = caps;
    }
}

VkImageUsageFlags VulkanDevice::ImageUsageFor(PixelFormat format, bool renderTarget,
                                              bool storage) const noexcept {
    const PixelFormatKind kind = GetPixelFormatInfo(format).kind;
    // 모든 텍스처가 복사 원본이 될 수 있다(readback·그래프 CopySource, D3D12도 허용한다).
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (renderTarget) {
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }

    if (storage) {
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }

    if (kind == PixelFormatKind::Depth || kind == PixelFormatKind::DepthStencil) {
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }

    return usage;
}

VkImageCreateFlags VulkanDevice::ImageFlagsFor(PixelFormat format) const noexcept {
    // sRGB 짝 view와 비디오 plane view는 이미지 형식과 다른 형식으로 읽는다.
    return TextureFormat(format).mutableFormat ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0u;
}

bool VulkanDevice::ImageSupported(VkFormat format, VkImageUsageFlags usage,
                                  VkImageCreateFlags flags, bool& mipmaps) const noexcept {
    VkImageFormatProperties properties = {};
    const VkResult result = functions_.vkGetPhysicalDeviceImageFormatProperties(
        physical_, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, flags, &properties);
    mipmaps = result == VK_SUCCESS && properties.maxMipLevels > 1;
    return result == VK_SUCCESS;
}

// 장치 생성 뒤 바뀌지 않는 표를 읽으므로 어느 스레드에서나 부를 수 있다.
PixelFormatCaps VulkanDevice::GetPixelFormatCaps(PixelFormat format) const noexcept {
    const auto number = static_cast<uint32>(format);
    if (!IsValidPixelFormat(format) || number > MaxPixelFormatNumber) {
        return PixelFormatCaps::None;
    }

    return formatCaps_[number];
}

VulkanDevice::TextureDescCheck VulkanDevice::CheckTextureDesc(const TextureDesc& desc,
                                                              usize pixelBytes) const noexcept {
    TextureDescCheck check;
    // 형식·용도·차원 지원은 장치 능력으로 판정한다(PixelFormat.ko.md 8.3절). 대응하지 않으면 다른
    // 형식으로 바꾸지 않고 Unsupported다. 다른 인자 검사보다 먼저 본다.
    if (IsValidPixelFormat(desc.format) && IsValidTextureDimension(desc.dimension) &&
        !HasPixelFormatCaps(GetPixelFormatCaps(desc.format), RequiredPixelFormatCaps(desc))) {
        check.code = ErrorCode::Unsupported;
        check.unsupportedFormat = true;
        return check;
    }

    auto valid = ValidateTextureDesc(desc);
    if (!valid) {
        check.code = valid.Error();
        return check;
    }

    if (!CheckTextureLimits(caps_, desc)) {
        check.code = ErrorCode::Unsupported;
        check.detail = "texture size, layer count or dimension exceeds the Vulkan device limits";
        return check;
    }

    const bool renderTarget = HasTextureUsage(desc.usage, TextureUsage::RenderTarget);
    const bool depthTarget = HasTextureUsage(desc.usage, TextureUsage::DepthStencil);
    const bool storage = HasTextureUsage(desc.usage, TextureUsage::Storage);
    // 렌더 타깃·깊이·Storage는 초기 픽셀이 없다(업로드하지 않는다).
    const bool attachment = renderTarget || depthTarget || storage;
    const PixelFormatInfo& pixelInfo = GetPixelFormatInfo(desc.format);
    const bool depth =
        pixelInfo.kind == PixelFormatKind::Depth || pixelInfo.kind == PixelFormatKind::DepthStencil;
    const uint64 size = TextureDataBytes(desc);
    // BC는 최상위 크기가 블록 배수여야 한다(PixelFormatInfo::topLevelBlockAligned, D3D와 같은
    // 계약).
    const bool aligned =
        !pixelInfo.topLevelBlockAligned ||
        (desc.width % pixelInfo.blockWidth == 0 && desc.height % pixelInfo.blockHeight == 0);
    if (desc.width > MaxTextureExtent || desc.height > MaxTextureExtent || size == 0 || !aligned ||
        desc.mipLevels > MaxMipLevels || (attachment ? pixelBytes != 0 : pixelBytes != size) ||
        (depthTarget && !depth)) {
        check.code = ErrorCode::InvalidArgument;
        return check;
    }

    if (desc.sampleCount > 1 &&
        (GetPixelFormatSampleCounts(desc.format) & SampleCountBit(desc.sampleCount)) == 0) {
        check.code = ErrorCode::Unsupported;
        check.detail = "MSAA sample count is not supported for this format by the Vulkan device";
        return check;
    }

    return check;
}

VkImageCreateInfo VulkanDevice::ImageInfoFor(const TextureDesc& desc) const noexcept {
    const bool renderTarget = HasTextureUsage(desc.usage, TextureUsage::RenderTarget);
    const bool storage = HasTextureUsage(desc.usage, TextureUsage::Storage);
    const bool volume = desc.dimension == TextureDimension::Texture3D;
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = TextureFormat(desc.format).format;
    info.extent = {desc.width, desc.height, volume ? desc.depth : 1u};
    info.mipLevels = desc.mipLevels;
    // cube·배열은 layer 배열이다(cube 면은 UE·D3D 순서대로 layer 0–5, Vulkan cube 면 순서와
    // 같다).
    info.arrayLayers = TextureSliceCount(desc);
    // VkSampleCountFlagBits 값은 표본 수와 같다(1·2·4·8·16·32).
    info.samples = static_cast<VkSampleCountFlagBits>(desc.sampleCount);
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = ImageUsageFor(desc.format, renderTarget, storage);
    // 3D 렌더 타깃은 깊이 면을 2D attachment view로 붙인다(2D_ARRAY_COMPATIBLE, Vulkan 1.1
    // core, 텍스처 GX2 C부).
    info.flags = ImageFlagsFor(desc.format) |
                 (volume && renderTarget ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0u) |
                 (IsCubeDimension(desc.dimension) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u);
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
}

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation). 초기 픽셀은 업로드 기록으로 간다.
Result<TextureHandle> VulkanDevice::CreateTexture(const TextureDesc& desc,
                                                  Span<const uint8> pixels) noexcept {
    return CreateTextureIn(desc, pixels, nullptr);
}

Result<TextureHandle> VulkanDevice::CreateTextureIn(const TextureDesc& desc,
                                                    Span<const uint8> pixels,
                                                    const Placement* placement) noexcept {
    const TextureDescCheck check = CheckTextureDesc(desc, pixels.Size());
    if (check.code != ErrorCode::None) {
        if (check.unsupportedFormat) {
            platform::LockGuard lock(gate_);
            return UnsupportedFormat<TextureHandle>(desc.format);
        }

        if (check.detail != nullptr) {
            platform::LockGuard lock(gate_);
            return Result<TextureHandle>::Failure(
                FailCode("CreateTexture", check.code, check.detail).Error());
        }

        return Result<TextureHandle>::Failure(check.code);
    }

    const TextureFormatInfo format = TextureFormat(desc.format);
    const bool renderTarget = HasTextureUsage(desc.usage, TextureUsage::RenderTarget);
    const bool depthTarget = HasTextureUsage(desc.usage, TextureUsage::DepthStencil);
    const bool storage = HasTextureUsage(desc.usage, TextureUsage::Storage);
    const bool attachment = renderTarget || depthTarget || storage;
    platform::LockGuard lock(gate_);
    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<TextureHandle>::Failure(reserved.Error());
    }

    auto sampler = SamplerFor(desc.sampler);
    if (!sampler) {
        return Result<TextureHandle>::Failure(sampler.Error());
    }

    const bool volume = desc.dimension == TextureDimension::Texture3D;
    const uint32 slices = TextureSliceCount(desc);
    const VkImageCreateInfo info = ImageInfoFor(desc);
    TextureRecord record;
    record.dimension = desc.dimension;
    record.width = desc.width;
    record.height = desc.height;
    record.depth = desc.depth;
    record.mipLevels = desc.mipLevels;
    record.slices = slices;
    record.format = desc.format;
    record.renderTarget = renderTarget;
    record.depthTarget = depthTarget;
    record.usage = desc.usage;
    record.sampleCount = desc.sampleCount;
    record.viewRange = {0, desc.mipLevels, 0, slices};
    record.aspect = format.aspect;
    record.sampler = sampler.Value();
    record.comparison = desc.sampler.comparison;
    // 업로드로 채운 텍스처와 렌더 타깃 모두 셰이더 읽기 상태로 시작한다(D3D12와 같다, GX5). 아래
    // 업로드 기록이 UNDEFINED에서 그 상태로 바꾼다. placed 텍스처는 heap 구간을 다른 자원이 쓰는
    // 중일 수 있어 아무것도 기록하지 않고 Unknown(UNDEFINED)으로 시작한다(첫 사용은 획득 전이).
    const ResourceAccess startState = placement != nullptr
                                          ? ResourceAccess::Unknown
                                          : CanonicalAccess(ResourceAccess::SrvGraphics);
    auto committed = record.committed.Reset(allocator_, desc.mipLevels, slices, startState);
    auto current = record.current.Reset(allocator_, desc.mipLevels, slices, startState);
    if (!committed || !current) {
        return Result<TextureHandle>::Failure(!committed ? committed.Error() : current.Error());
    }

    if (placement != nullptr) {
        const VkResult result = functions_.vkCreateImage(device_, &info, nullptr, &record.image);
        if (result != VK_SUCCESS) {
            record.image = VK_NULL_HANDLE;
            return FailWith<TextureHandle>("vkCreateImage", result);
        }

        auto bound =
            BindPlaced(*placement, TransientHeapKindOf(desc), record.image, VK_NULL_HANDLE);
        if (!bound) {
            functions_.vkDestroyImage(device_, record.image, nullptr);
            return Result<TextureHandle>::Failure(bound.Error());
        }

        record.heap = placement->heap;
    } else {
        auto image = CreateImageWithMemory(info, record.image, record.memory);
        if (!image) {
            return Result<TextureHandle>::Failure(image.Error());
        }
    }

    // 생성 중 실패하면 아직 GPU가 참조하지 않으므로 바로 파괴한다(placed면 heap의 placed 수도
    // 되돌린다).
    Retired cleanup;
    cleanup.image = record.image;
    cleanup.memory = record.memory;
    cleanup.heap = record.heap;
    // 기본 view: 깊이·스텐실은 깊이 aspect, 비디오는 plane 0(휘도)이다(PixelFormat.ko.md 10·11절).
    // 성분 대응은 형식 흉내(A8 등)의 대응 위에 TextureDesc::swizzle을 합성한다.
    const ViewFormatInfo viewFormat = ViewFormat(desc.format, 0, desc.format);
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = record.image;
    viewInfo.viewType = ViewTypeOf(desc.dimension);
    viewInfo.format = viewFormat.format;
    viewInfo.components = ComposeComponents(desc.swizzle, viewFormat.components);
    viewInfo.subresourceRange = {viewFormat.aspect, 0, desc.mipLevels, 0, slices};
    VkResult result = functions_.vkCreateImageView(device_, &viewInfo, nullptr, &record.view);
    if (result != VK_SUCCESS) {
        DestroyRetired(cleanup);
        return FailWith<TextureHandle>("vkCreateImageView", result);
    }

    cleanup.view = record.view;
    // attachment용 view는 항등 대응·이미지 전체 aspect다. 깊이·스텐실 타깃(기본 view는 깊이
    // aspect만)과, 성분 대응이 항등이 아닌 렌더 타깃(Vulkan은 framebuffer view의 대응이 항등이어야
    // 하고, swizzle은 셰이더 읽기에만 적용한다, TextureDesc::swizzle)에 따로 만든다.
    const VkComponentMapping& components = viewInfo.components;
    const bool identity = components.r == VK_COMPONENT_SWIZZLE_IDENTITY &&
                          components.g == VK_COMPONENT_SWIZZLE_IDENTITY &&
                          components.b == VK_COMPONENT_SWIZZLE_IDENTITY &&
                          components.a == VK_COMPONENT_SWIZZLE_IDENTITY;
    if ((depthTarget && (format.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) ||
        ((renderTarget || depthTarget) && !identity)) {
        viewInfo.subresourceRange.aspectMask = format.aspect;
        viewInfo.components = {};
        result = functions_.vkCreateImageView(device_, &viewInfo, nullptr, &record.attachmentView);
        if (result != VK_SUCCESS) {
            record.attachmentView = VK_NULL_HANDLE;
            DestroyRetired(cleanup);
            return FailWith<TextureHandle>("vkCreateImageView (attachment)", result);
        }

        cleanup.attachmentView = record.attachmentView;
    }

    auto added = textures_.TryAdd(Move(record));
    if (!added) {
        DestroyRetired(cleanup);
        return Result<TextureHandle>::Failure(added.Error());
    }

    const TextureRecord& stored = *textures_.Find(added.Value());
    if (volume && renderTarget) {
        // 표에 넣지 못하면 barrier layer 수가 1로 남는다(maintenance9 전에는 같은 뜻이다).
        auto tracked = volumeAttachmentImages_.TryPushBack(stored.image);
        (void)tracked;
    }

    // placed 텍스처는 업로드 기록에 아무것도 남기지 않는다(위 startState).
    if (placement != nullptr) {
        return Result<TextureHandle>::Success(TextureHandle{added.Value()});
    }

    Result<void> uploaded = Result<void>::Success();
    if (attachment) {
        // 셰이더 읽기 상태(SHADER_READ_ONLY)로 바꿔 둔다. 다음 제출보다 먼저 실행된다.
        uploaded = OpenUploadList();
        if (uploaded) {
            (void)AccessBarrier(upload_, stored.image, 0, stored.mipLevels, ResourceAccess::Unknown,
                                CanonicalAccess(ResourceAccess::SrvGraphics), false, stored.aspect,
                                0, stored.slices);
        }
    } else {
        uploaded = RecordTextureUpload(stored, 0, stored.mipLevels, pixels, true);
    }

    if (!uploaded) {
        // 업로드 기록이 이미 참조했을 수 있으므로 회수 경로로 놓는다.
        if (volume && renderTarget && !volumeAttachmentImages_.IsEmpty() &&
            volumeAttachmentImages_[volumeAttachmentImages_.Size() - 1] == cleanup.image) {
            auto removed =
                volumeAttachmentImages_.TryRemoveAtSwap(volumeAttachmentImages_.Size() - 1);
            (void)removed;
        }

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

    // view가 남아 있으면 버퍼·메모리를 orphan으로 옮겨 마지막 view가 놓일 때 회수한다.
    if (record->viewCount != 0) {
        OrphanBuffer orphan;
        orphan.buffer = record->buffer;
        orphan.memory = record->memory;
        orphan.viewCount = record->viewCount;
        orphan.heap = record->heap;
        auto kept = orphanBuffers_.TryPushBack(orphan);
        if (kept) {
            auto removed = buffers_.TryRemove(handle.slot);
            (void)removed;
            return;
        }

        // 기록할 자리가 없으면 소멸 때까지 놓지 않는다(view가 쓰는 버퍼를 먼저 놓지 않는다).
        Retired entry;
        entry.buffer = record->buffer;
        entry.memory = record->memory;
        entry.heap = record->heap;
        KeepUntilDestruction(Move(entry));
        auto removed = buffers_.TryRemove(handle.slot);
        (void)removed;
        return;
    }

    Retired entry;
    entry.buffer = record->buffer;
    entry.memory = record->memory;
    entry.heap = record->heap;
    RetireEntry(Move(entry));
    auto removed = buffers_.TryRemove(handle.slot);
    (void)removed;
}

// 원 텍스처의 이미지를 공유하는 view다(PixelFormat.ko.md 9–11절). 다른 스레드에서도 부를 수 있다.
Result<TextureHandle> VulkanDevice::CreateTextureView(TextureHandle texture,
                                                      const TextureViewDesc& desc) noexcept {
    platform::LockGuard lock(gate_);
    TextureRecord* source = textures_.Find(texture.slot);
    if (source == nullptr || source->retirePending) {
        return Result<TextureHandle>::Failure(ErrorCode::StaleHandle);
    }

    if (source->isView) {
        return Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
    }

    auto resolved = ResolveTextureViewFormat(source->format, desc, source->comparison);
    if (!resolved) {
        return Result<TextureHandle>::Failure(resolved.Error());
    }

    auto range = ResolveTextureViewRange(source->dimension, source->usage,
                                         {source->mipLevels, source->slices}, desc);
    if (!range) {
        return Result<TextureHandle>::Failure(range.Error());
    }

    // sRGB 짝 view는 짝 형식이, plane view는 원 형식이 샘플 가능해야 한다. 쓰기 view는 view 형식이
    // storage image로 쓸 수 있어야 한다.
    const bool writable = desc.access == ViewAccess::ReadWrite;
    const bool planes = PixelFormatPlaneCount(source->format) == 2;
    const PixelFormat sampledFormat = planes ? source->format : resolved.Value();
    const ViewFormatInfo view = ViewFormat(source->format, desc.plane, resolved.Value());
    const PixelFormatCaps needed =
        writable ? PixelFormatCaps::StorageWrite : PixelFormatCaps::Sampled;
    if (!HasPixelFormatCaps(GetPixelFormatCaps(writable ? resolved.Value() : sampledFormat),
                            needed) ||
        view.format == VK_FORMAT_UNDEFINED) {
        return UnsupportedFormat<TextureHandle>(resolved.Value());
    }

    // cube 텍스처의 cube 배열 view는 imageCubeArray 기능이 필요하다.
    if (TextureViewDimensionOf(source->dimension, desc) == TextureDimension::TextureCubeArray &&
        !caps_.features.textureCubeArray) {
        auto failed = FailCode("texture view", ErrorCode::Unsupported,
                               "cube array views need the imageCubeArray feature");
        return Result<TextureHandle>::Failure(failed.Error());
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<TextureHandle>::Failure(reserved.Error());
    }

    const PixelFormatPlane& layout = GetPixelFormatInfo(source->format).planes[desc.plane];
    TextureRecord record;
    record.image = source->image;
    // 차원은 view 차원이다(image view type, cube 계열 쓰기 view는 2D 배열, 텍스처 GX2 나머지 B부).
    record.dimension = TextureViewDimensionOf(source->dimension, desc);
    record.width = MipExtent(source->width, layout.widthShift);
    record.height = MipExtent(source->height, layout.heightShift);
    record.depth = source->depth;
    record.mipLevels = source->mipLevels;
    record.slices = source->slices;
    record.format = resolved.Value();
    record.usage = source->usage;
    record.sampleCount = source->sampleCount;
    record.viewRange = range.Value();
    record.writable = writable;
    record.aspect = source->aspect;
    record.sampler = source->sampler;
    record.comparison = source->comparison;
    record.isView = true;
    record.source = texture.slot;
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = source->image;
    viewInfo.viewType = ViewTypeOf(record.dimension);
    viewInfo.format = view.format;
    // view swizzle은 스텐실 plane(R → G)·형식 흉내 대응 위에 합성한다(TextureViewDesc::swizzle).
    // storage image view는 성분 대응이 항등이어야
    // 한다(VUID-VkWriteDescriptorSet-descriptorType-00336).
    viewInfo.components =
        writable ? VkComponentMapping{} : ComposeComponents(desc.swizzle, view.components);
    const ResolvedSubresourceRange& viewRange = range.Value();
    viewInfo.subresourceRange = {view.aspect, viewRange.firstMip, viewRange.mipCount,
                                 viewRange.firstSlice, viewRange.sliceCount};
    const VkResult result = functions_.vkCreateImageView(device_, &viewInfo, nullptr, &record.view);
    if (result != VK_SUCCESS) {
        return FailWith<TextureHandle>("vkCreateImageView (texture view)", result);
    }

    Retired cleanup;
    cleanup.view = record.view;
    auto added = textures_.TryAdd(Move(record));
    if (!added) {
        DestroyRetired(cleanup);
        return Result<TextureHandle>::Failure(added.Error());
    }

    ++source->viewCount;
    return Result<TextureHandle>::Success(TextureHandle{added.Value()});
}

void VulkanDevice::Retire(TextureHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    TextureRecord* record = textures_.Find(handle.slot);
    if (record == nullptr || handle == white_ || record->retirePending) {
        return;
    }

    if (record->viewCount != 0) {
        record->retirePending = true;
        return;
    }

    const bool view = record->isView;
    const SlotHandle source = record->source;
    RetireTextureRecord(handle.slot, *record);
    if (view) {
        TextureRecord* owner = textures_.Find(source);
        if (owner != nullptr && owner->viewCount != 0 && --owner->viewCount == 0 &&
            owner->retirePending) {
            RetireTextureRecord(source, *owner);
        }
    }
}

void VulkanDevice::RetireTextureRecord(SlotHandle slot, TextureRecord& record) noexcept {
    Retired entry;
    // view는 이미지·메모리를 쥐지 않는다(원 텍스처 기록이 놓는다).
    if (!record.isView) {
        entry.image = record.image;
        entry.memory = record.memory;
        entry.heap = record.heap;
    }
    // 이 view를 attachment로 쓴 framebuffer를 먼저 회수한다(같은 fence 뒤에 파괴된다).
    RetireFramebuffers(record.view);
    RetireFramebuffers(record.attachmentView);
    if (!record.isView) {
        RetireAttachmentViews(record.image);
        for (usize index = 0; index < volumeAttachmentImages_.Size(); ++index) {
            if (volumeAttachmentImages_[index] == record.image) {
                auto removed = volumeAttachmentImages_.TryRemoveAtSwap(index);
                (void)removed;
                break;
            }
        }
    }
    entry.view = record.view;
    entry.attachmentView = record.attachmentView;
    RetireEntry(Move(entry));
    auto removed = textures_.TryRemove(slot);
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
    entry.pipelineLayout = record->layout;
    entry.setLayouts[0] = record->setLayouts[0];
    entry.setLayouts[1] = record->setLayouts[1];
    RetireEntry(Move(entry));
    auto removed = pipelines_.TryRemove(handle.slot);
    (void)removed;
}

Result<TextureSubresources>
VulkanDevice::GetTextureSubresources(TextureHandle texture) const noexcept {
    platform::LockGuard lock(gate_);
    const TextureRecord* record = textures_.Find(texture.slot);
    if (record == nullptr) {
        return Result<TextureSubresources>::Failure(ErrorCode::StaleHandle);
    }

    return Result<TextureSubresources>::Success(
        TextureSubresources{record->mipLevels, record->slices});
}

} // namespace rocks::graphics::vulkan
