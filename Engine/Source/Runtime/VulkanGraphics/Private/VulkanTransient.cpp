#include "VulkanDeviceImpl.h"
#include "VulkanFormats.h"

// transient heap·placed 자원(RenderBase 8절 GX12)이다. heap은 종류(Buffers·Textures·Targets)마다
// DEVICE_LOCAL VkDeviceMemory 하나이고, placed 자원은 그 offset에 vkBindImageMemory·
// vkBindBufferMemory로 묶는다. 종류를 나누므로 선형(버퍼)과 최적(이미지) 자원이 한 heap에 섞이지
// 않아 bufferImageGranularity를 따로 맞추지 않는다. 같은 구간을 쓰는 자원은 처음 쓰기 전에 획득
// 전이(VulkanAccess.cpp AcquireBarrier)를 낸다.
namespace rocks::graphics::vulkan {

namespace {

// 메모리 요구와 전용 할당 요구다.
struct Requirements {
    VkMemoryRequirements memory = {};
    bool dedicated = false;
};

Requirements ImageRequirements(const Functions& functions, VkDevice device,
                               VkImage image) noexcept {
    VkMemoryDedicatedRequirements dedicated = {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    VkMemoryRequirements2 requirements = {};
    requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    requirements.pNext = &dedicated;
    VkImageMemoryRequirementsInfo2 query = {};
    query.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    query.image = image;
    functions.vkGetImageMemoryRequirements2(device, &query, &requirements);
    return {requirements.memoryRequirements, dedicated.requiresDedicatedAllocation == VK_TRUE};
}

Requirements BufferRequirements(const Functions& functions, VkDevice device,
                                VkBuffer buffer) noexcept {
    VkMemoryDedicatedRequirements dedicated = {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    VkMemoryRequirements2 requirements = {};
    requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    requirements.pNext = &dedicated;
    VkBufferMemoryRequirementsInfo2 query = {};
    query.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
    query.buffer = buffer;
    functions.vkGetBufferMemoryRequirements2(device, &query, &requirements);
    return {requirements.memoryRequirements, dedicated.requiresDedicatedAllocation == VK_TRUE};
}

// 잠깐 만든 이미지의 메모리 요구다(placement 질의, heap 메모리 종류). 만들지 못하면 failure에 원본
// 결과를 두고 memoryTypeBits가 0이다.
Requirements ProbeImage(const Functions& functions, VkDevice device, const VkImageCreateInfo& info,
                        VkResult& failure) noexcept {
    VkImage image = VK_NULL_HANDLE;
    failure = functions.vkCreateImage(device, &info, nullptr, &image);
    if (failure != VK_SUCCESS) {
        return {};
    }

    const Requirements requirements = ImageRequirements(functions, device, image);
    functions.vkDestroyImage(device, image, nullptr);
    return requirements;
}

Requirements ProbeBuffer(const Functions& functions, VkDevice device, uint64 size,
                         VkBufferUsageFlags usage, VkResult& failure) noexcept {
    VkBufferCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    failure = functions.vkCreateBuffer(device, &info, nullptr, &buffer);
    if (failure != VK_SUCCESS) {
        return {};
    }

    const Requirements requirements = BufferRequirements(functions, device, buffer);
    functions.vkDestroyBuffer(device, buffer, nullptr);
    return requirements;
}

// 잠깐 만드는 자원의 생성 실패다(메모리 부족과 그 밖).
ErrorCode ProbeError(VkResult result) noexcept {
    return result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY
               ? ErrorCode::OutOfMemory
               : ErrorCode::DependencyFailed;
}

// placed로 둘 수 있는 텍스처 용도다(초기 데이터가 없는 렌더 타깃·깊이·Storage).
bool PlaceableTexture(const TextureDesc& desc) noexcept {
    return HasTextureUsage(desc.usage, TextureUsage::RenderTarget | TextureUsage::DepthStencil |
                                           TextureUsage::Storage);
}

bool IsValidHeapKind(TransientHeapKind kind) noexcept {
    return kind == TransientHeapKind::Buffers || kind == TransientHeapKind::Textures ||
           kind == TransientHeapKind::Targets;
}

} // namespace

Result<uint32> VulkanDevice::HeapMemoryType(TransientHeapKind kind) const noexcept {
    // 종류의 대표 자원으로 메모리 종류 bit를 정한다. 버퍼는 모든 용도, 텍스처는 Storage 이미지,
    // 렌더 타깃은 색 타깃과 (만들 수 있으면) 깊이 타깃 bit의 교집합이다.
    VkResult failure = VK_SUCCESS;
    uint32 typeBits = 0;
    if (kind == TransientHeapKind::Buffers) {
        const BufferUsage all = BufferUsage::Vertex | BufferUsage::Index | BufferUsage::Indirect |
                                BufferUsage::ShaderRead | BufferUsage::ShaderWrite |
                                BufferUsage::Raw;
        typeBits = ProbeBuffer(functions_, device_, HeapProbeBufferBytes,
                               BufferUsageFor(all) | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, failure)
                       .memory.memoryTypeBits;
    } else {
        TextureDesc desc;
        desc.width = HeapProbeImageExtent;
        desc.height = HeapProbeImageExtent;
        desc.format = PixelFormat::RGBA8Unorm;
        desc.usage =
            kind == TransientHeapKind::Targets ? TextureUsage::RenderTarget : TextureUsage::Storage;
        typeBits =
            ProbeImage(functions_, device_, ImageInfoFor(desc), failure).memory.memoryTypeBits;
        // 깊이 형식은 지원할 때만 만든다(지원하지 않는 형식의 이미지 생성은 검증 오류다).
        if (kind == TransientHeapKind::Targets &&
            HasPixelFormatCaps(GetPixelFormatCaps(PixelFormat::D32Float),
                               PixelFormatCaps::DepthStencil)) {
            desc.format = PixelFormat::D32Float;
            desc.usage = TextureUsage::DepthStencil;
            VkResult ignored = VK_SUCCESS;
            const uint32 depthBits =
                ProbeImage(functions_, device_, ImageInfoFor(desc), ignored).memory.memoryTypeBits;
            if ((typeBits & depthBits) != 0) {
                typeBits &= depthBits;
            }
        }
    }

    if (typeBits == 0) {
        return Result<uint32>::Failure(failure != VK_SUCCESS ? ProbeError(failure)
                                                             : ErrorCode::Unsupported);
    }

    platform::LockGuard lock(memoryGate_);
    uint32 type = 0;
    if (!memory_.PickDeviceType(typeBits, type)) {
        return Result<uint32>::Failure(ErrorCode::Unsupported);
    }

    return Result<uint32>::Success(type);
}

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation).
Result<TransientHeapHandle>
VulkanDevice::CreateTransientHeap(const TransientHeapDesc& desc) noexcept {
    if (desc.bytes == 0 || !IsValidHeapKind(desc.kind)) {
        return Result<TransientHeapHandle>::Failure(ErrorCode::InvalidArgument);
    }

    auto type = HeapMemoryType(desc.kind);
    platform::LockGuard lock(gate_);
    if (!type) {
        auto failed = FailCode("CreateTransientHeap", type.Error(),
                               "no device memory type fits the transient heap kind");
        return Result<TransientHeapHandle>::Failure(failed.Error());
    }

    if (maxAllocationBytes_ != 0 && desc.bytes > maxAllocationBytes_) {
        auto failed =
            FailCode("CreateTransientHeap", ErrorCode::Unsupported,
                     "transient heap size exceeds the device limit (maxMemoryAllocationSize)");
        return Result<TransientHeapHandle>::Failure(failed.Error());
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<TransientHeapHandle>::Failure(reserved.Error());
    }

    TransientHeapRecord record;
    record.size = desc.bytes;
    record.kind = desc.kind;
    record.memoryType = type.Value();
    VkMemoryAllocateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.allocationSize = desc.bytes;
    info.memoryTypeIndex = record.memoryType;
    const VkResult result = functions_.vkAllocateMemory(device_, &info, nullptr, &record.memory);
    if (result != VK_SUCCESS) {
        return FailWith<TransientHeapHandle>("vkAllocateMemory (transient heap)", result);
    }

    const VkDeviceMemory memory = record.memory;
    auto added = heaps_.TryAdd(Move(record));
    if (!added) {
        functions_.vkFreeMemory(device_, memory, nullptr);
        return Result<TransientHeapHandle>::Failure(added.Error());
    }

    return Result<TransientHeapHandle>::Success(TransientHeapHandle{added.Value()});
}

void VulkanDevice::RetireTransientHeap(TransientHeapHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    TransientHeapRecord* record = heaps_.Find(handle.slot);
    if (record == nullptr || record->retirePending) {
        return;
    }

    // placed 자원이 남아 있으면 마지막 자원이 파괴될 때(ReleasePlacement) 놓는다. 없으면 heap의
    // 마지막 사용(지금까지의 제출과 기록 중 프레임) 뒤에 놓는다.
    record->retirePending = true;
    if (record->placedCount != 0) {
        return;
    }

    Retired entry;
    entry.heapMemory = record->memory;
    RetireEntry(Move(entry));
    auto removed = heaps_.TryRemove(handle.slot);
    (void)removed;
}

void VulkanDevice::ReleasePlacement(SlotHandle slot) noexcept {
    TransientHeapRecord* record = heaps_.Find(slot);
    if (record == nullptr || record->placedCount == 0) {
        return;
    }

    // placed 자원은 그 회수 fence가 끝난 뒤 파괴된다(DestroyRetired). heap 메모리를 쓰는 것은
    // placed 자원뿐이므로 마지막 자원이 파괴되면 heap의 마지막 사용도 끝났다.
    if (--record->placedCount == 0 && record->retirePending) {
        functions_.vkFreeMemory(device_, record->memory, nullptr);
        auto removed = heaps_.TryRemove(slot);
        (void)removed;
    }
}

// 스레드 안전하다(장치 생성 뒤 바뀌지 않는 표만 읽고 잠깐 만든 이미지는 이 호출 안에서 파괴한다).
Result<TransientPlacement>
VulkanDevice::GetTexturePlacement(const TextureDesc& desc) const noexcept {
    const TextureDescCheck check = CheckTextureDesc(desc, 0);
    if (check.code != ErrorCode::None) {
        return Result<TransientPlacement>::Failure(check.code);
    }

    // Sampled 텍스처는 초기 데이터가 필요하므로 placed로 두지 않는다.
    if (!PlaceableTexture(desc)) {
        return Result<TransientPlacement>::Failure(ErrorCode::InvalidArgument);
    }

    VkResult failure = VK_SUCCESS;
    const Requirements requirements = ProbeImage(functions_, device_, ImageInfoFor(desc), failure);
    if (failure != VK_SUCCESS) {
        return Result<TransientPlacement>::Failure(ProbeError(failure));
    }

    if (requirements.dedicated) {
        return Result<TransientPlacement>::Failure(ErrorCode::Unsupported);
    }

    TransientPlacement placement;
    placement.bytes = requirements.memory.size;
    placement.alignment = requirements.memory.alignment;
    placement.kind = TransientHeapKindOf(desc);
    return Result<TransientPlacement>::Success(placement);
}

Result<TransientPlacement> VulkanDevice::GetBufferPlacement(const BufferDesc& desc) const noexcept {
    if (!ValidateBufferDesc(desc)) {
        return Result<TransientPlacement>::Failure(ErrorCode::InvalidArgument);
    }

    if (desc.size > caps_.limits.bufferBytes) {
        return Result<TransientPlacement>::Failure(ErrorCode::Unsupported);
    }

    VkResult failure = VK_SUCCESS;
    const Requirements requirements =
        ProbeBuffer(functions_, device_, desc.size, BufferUsageFor(desc.usage), failure);
    if (failure != VK_SUCCESS) {
        return Result<TransientPlacement>::Failure(ProbeError(failure));
    }

    if (requirements.dedicated) {
        return Result<TransientPlacement>::Failure(ErrorCode::Unsupported);
    }

    TransientPlacement placement;
    placement.bytes = requirements.memory.size;
    placement.alignment = requirements.memory.alignment;
    placement.kind = TransientHeapKind::Buffers;
    return Result<TransientPlacement>::Success(placement);
}

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation).
Result<TextureHandle> VulkanDevice::CreatePlacedTexture(TransientHeapHandle heap, uint64 offset,
                                                        const TextureDesc& desc) noexcept {
    if (!PlaceableTexture(desc)) {
        return Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
    }

    const Placement placement{heap.slot, offset};
    return CreateTextureIn(desc, {}, &placement);
}

Result<BufferHandle> VulkanDevice::CreatePlacedBuffer(TransientHeapHandle heap, uint64 offset,
                                                      const BufferDesc& desc) noexcept {
    const Placement placement{heap.slot, offset};
    return CreateBufferIn(desc, {}, &placement);
}

Result<void> VulkanDevice::BindPlaced(const Placement& placement, TransientHeapKind kind,
                                      VkImage image, VkBuffer buffer) noexcept {
    TransientHeapRecord* heap = heaps_.Find(placement.heap);
    if (heap == nullptr || heap->retirePending) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (heap->kind != kind) {
        return FailCode("CreatePlaced", ErrorCode::InvalidArgument,
                        "resource kind differs from the transient heap kind");
    }

    const Requirements requirements = image != VK_NULL_HANDLE
                                          ? ImageRequirements(functions_, device_, image)
                                          : BufferRequirements(functions_, device_, buffer);
    const uint64 bytes = requirements.memory.size;
    const uint64 alignment = requirements.memory.alignment != 0 ? requirements.memory.alignment : 1;
    if (placement.offset % alignment != 0 || bytes > heap->size ||
        placement.offset > heap->size - bytes) {
        return FailCode("CreatePlaced", ErrorCode::InvalidArgument,
                        "placement offset is misaligned or outside the transient heap");
    }

    if ((requirements.memory.memoryTypeBits & (1u << heap->memoryType)) == 0 ||
        requirements.dedicated) {
        return FailCode("CreatePlaced", ErrorCode::Unsupported,
                        "resource cannot live in the memory type of the transient heap");
    }

    const VkResult result =
        image != VK_NULL_HANDLE
            ? functions_.vkBindImageMemory(device_, image, heap->memory, placement.offset)
            : functions_.vkBindBufferMemory(device_, buffer, heap->memory, placement.offset);
    if (result != VK_SUCCESS) {
        return Fail(image != VK_NULL_HANDLE ? "vkBindImageMemory (placed)"
                                            : "vkBindBufferMemory (placed)",
                    result);
    }

    ++heap->placedCount;
    return Result<void>::Success();
}

} // namespace rocks::graphics::vulkan
