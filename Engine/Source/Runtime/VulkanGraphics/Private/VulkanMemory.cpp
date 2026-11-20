#include "VulkanMemory.h"

namespace rocks::graphics::vulkan {

namespace {

// 페이지 크기다(장치 전용 64 MiB, 호스트 가시 16 MiB). 힙의 1/8을 넘지 않는다. 페이지 절반 이상의
// 요청은 전용 할당이다(UE는 GPU 128 MB·staging 32 MB, VulkanMemory.cpp:437–441).
constexpr uint64 DevicePageBytes = uint64{64} << 20;
constexpr uint64 HostPageBytes = uint64{16} << 20;
constexpr uint64 HeapPageDivisor = 8;

uint64 AlignUp(uint64 value, uint64 alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

ErrorCode AllocationError(VkResult result) noexcept {
    return result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY
               ? ErrorCode::OutOfMemory
               : ErrorCode::DependencyFailed;
}

bool HostVisible(MemoryUsage usage) noexcept {
    return usage != MemoryUsage::Device;
}

} // namespace

MemoryManager::MemoryManager(Allocator& allocator) noexcept
    : allocator_(allocator), pages_(allocator) {
}

MemoryManager::~MemoryManager() noexcept {
    Clear();
}

void MemoryManager::Initialize(const Functions& functions, VkDevice device,
                               const VkPhysicalDeviceMemoryProperties& properties,
                               uint64 nonCoherentAtomSize) noexcept {
    functions_ = &functions;
    device_ = device;
    properties_ = properties;
    atom_ = nonCoherentAtomSize != 0 ? nonCoherentAtomSize : 1;
}

uint32 MemoryManager::PickType(uint32 typeBits, MemoryUsage usage) const noexcept {
    // 앞 후보부터 요구 bit를 모두 가진 첫 종류를 고른다(UE와 같다, VulkanMemory.cpp:5154–5174).
    VkMemoryPropertyFlags candidates[2] = {};
    uint32 candidateCount = 1;
    switch (usage) {
    case MemoryUsage::Device:
        candidates[0] = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        candidates[1] = 0;
        candidateCount = 2;
        break;
    case MemoryUsage::Upload:
        candidates[0] = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        break;
    case MemoryUsage::Readback:
        candidates[0] = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        candidates[1] = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        candidateCount = 2;
        break;
    }

    for (uint32 candidate = 0; candidate < candidateCount; ++candidate) {
        for (uint32 type = 0; type < properties_.memoryTypeCount; ++type) {
            const VkMemoryPropertyFlags flags = properties_.memoryTypes[type].propertyFlags;
            if ((typeBits & (1u << type)) != 0 &&
                (flags & candidates[candidate]) == candidates[candidate]) {
                return type;
            }
        }
    }

    return NoMemoryType;
}

uint64 MemoryManager::PageBytes(uint32 type, MemoryUsage usage) const noexcept {
    const uint64 heap = properties_.memoryHeaps[properties_.memoryTypes[type].heapIndex].size;
    const uint64 wanted = HostVisible(usage) ? HostPageBytes : DevicePageBytes;
    const uint64 limit = heap / HeapPageDivisor;
    return limit != 0 && limit < wanted ? limit : wanted;
}

Result<MemoryAllocation> MemoryManager::AllocateDedicated(const MemoryRequest& request, uint32 type,
                                                          VkResult& failure) noexcept {
    VkMemoryDedicatedAllocateInfo dedicated = {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.buffer = request.buffer;
    dedicated.image = request.image;
    VkMemoryAllocateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.pNext =
        request.buffer != VK_NULL_HANDLE || request.image != VK_NULL_HANDLE ? &dedicated : nullptr;
    info.allocationSize = AlignUp(request.size, atom_);
    info.memoryTypeIndex = type;
    MemoryAllocation allocation;
    failure = functions_->vkAllocateMemory(device_, &info, nullptr, &allocation.memory);
    if (failure != VK_SUCCESS) {
        return Result<MemoryAllocation>::Failure(AllocationError(failure));
    }

    const VkMemoryPropertyFlags flags = properties_.memoryTypes[type].propertyFlags;
    if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        void* mapped = nullptr;
        failure = functions_->vkMapMemory(device_, allocation.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (failure != VK_SUCCESS) {
            functions_->vkFreeMemory(device_, allocation.memory, nullptr);
            return Result<MemoryAllocation>::Failure(AllocationError(failure));
        }

        allocation.mapped = static_cast<uint8*>(mapped);
    }

    allocation.size = request.size;
    allocation.coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    allocation.page = DedicatedPage;
    ++dedicatedCount_;
    return Result<MemoryAllocation>::Success(allocation);
}

Result<MemoryAllocation> MemoryManager::Allocate(const MemoryRequest& request, MemoryUsage usage,
                                                 ResourceShape shape, VkResult& failure) noexcept {
    failure = VK_SUCCESS;
    const uint32 type = PickType(request.typeBits, usage);
    if (type == NoMemoryType) {
        failure = VK_ERROR_FEATURE_NOT_PRESENT;
        return Result<MemoryAllocation>::Failure(ErrorCode::Unsupported);
    }

    const VkMemoryPropertyFlags flags = properties_.memoryTypes[type].propertyFlags;
    const bool coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    // coherent가 아닌 호스트 메모리는 invalidate 구간이 atom 경계여야 하므로 정렬과 크기를 올린다.
    uint64 alignment = request.alignment != 0 ? request.alignment : 1;
    uint64 size = request.size;
    if (HostVisible(usage) && !coherent) {
        alignment = alignment > atom_ ? alignment : atom_;
        size = AlignUp(size, atom_);
    }

    const uint64 pageBytes = PageBytes(type, usage);
    if (request.dedicated || size >= pageBytes / 2) {
        MemoryRequest whole = request;
        whole.size = size;
        return AllocateDedicated(whole, type, failure);
    }

    for (uint32 index = 0; index < pages_.Size(); ++index) {
        Page* page = pages_[index].Get();
        if (page == nullptr || page->type != type || page->shape != shape) {
            continue;
        }

        auto block = page->ledger.Allocate(size, alignment);
        if (!block) {
            continue;
        }

        MemoryAllocation allocation;
        allocation.memory = page->memory;
        allocation.offset = block.Value().offset;
        allocation.size = size;
        allocation.mapped = page->mapped != nullptr ? page->mapped + allocation.offset : nullptr;
        allocation.coherent = coherent;
        allocation.page = index;
        allocation.block = block.Value();
        return Result<MemoryAllocation>::Success(allocation);
    }

    // 새 페이지를 만든다. 빈 칸이 있으면 그 번호를 다시 쓴다.
    auto created = UniquePtr<Page>::TryMake(allocator_, allocator_);
    if (!created) {
        return Result<MemoryAllocation>::Failure(created.Error());
    }

    Page& page = *created.Value();
    page.type = type;
    page.shape = shape;
    auto reset = page.ledger.Reset(pageBytes);
    if (!reset) {
        return Result<MemoryAllocation>::Failure(reset.Error());
    }

    VkMemoryAllocateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.allocationSize = pageBytes;
    info.memoryTypeIndex = type;
    failure = functions_->vkAllocateMemory(device_, &info, nullptr, &page.memory);
    if (failure != VK_SUCCESS) {
        return Result<MemoryAllocation>::Failure(AllocationError(failure));
    }

    if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        void* mapped = nullptr;
        failure = functions_->vkMapMemory(device_, page.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (failure != VK_SUCCESS) {
            functions_->vkFreeMemory(device_, page.memory, nullptr);
            return Result<MemoryAllocation>::Failure(AllocationError(failure));
        }

        page.mapped = static_cast<uint8*>(mapped);
    }

    uint32 slot = static_cast<uint32>(pages_.Size());
    for (uint32 index = 0; index < pages_.Size(); ++index) {
        if (pages_[index].Get() == nullptr) {
            slot = index;
            break;
        }
    }

    if (slot == pages_.Size()) {
        auto pushed = pages_.TryPushBack(UniquePtr<Page>());
        if (!pushed) {
            functions_->vkFreeMemory(device_, page.memory, nullptr);
            return Result<MemoryAllocation>::Failure(pushed.Error());
        }
    }

    auto block = page.ledger.Allocate(size, alignment);
    MemoryAllocation allocation;
    allocation.memory = page.memory;
    allocation.offset = block.Value().offset;
    allocation.size = size;
    allocation.mapped = page.mapped != nullptr ? page.mapped + allocation.offset : nullptr;
    allocation.coherent = coherent;
    allocation.page = slot;
    allocation.block = block.Value();
    pages_[slot] = Move(created.Value());
    return Result<MemoryAllocation>::Success(allocation);
}

void MemoryManager::Free(const MemoryAllocation& allocation) noexcept {
    if (allocation.IsNull()) {
        return;
    }

    if (allocation.page == DedicatedPage) {
        functions_->vkFreeMemory(device_, allocation.memory, nullptr);
        --dedicatedCount_;
        return;
    }

    Page* page = pages_[allocation.page].Get();
    page->ledger.Free(allocation.block);
    if (!page->ledger.IsEmpty()) {
        return;
    }

    // 같은 묶음에 빈 페이지가 또 있으면 이 페이지를 놓는다(빈 페이지 하나는 다음 할당에 남긴다).
    for (uint32 index = 0; index < pages_.Size(); ++index) {
        const Page* other = pages_[index].Get();
        if (index != allocation.page && other != nullptr && other->type == page->type &&
            other->shape == page->shape && other->ledger.IsEmpty()) {
            functions_->vkFreeMemory(device_, page->memory, nullptr);
            pages_[allocation.page] = UniquePtr<Page>();
            return;
        }
    }
}

void MemoryManager::Invalidate(const MemoryAllocation& allocation) noexcept {
    if (allocation.coherent || allocation.IsNull()) {
        return;
    }

    VkMappedMemoryRange range = {};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = allocation.memory;
    range.offset = allocation.offset;
    range.size = allocation.size;
    (void)functions_->vkInvalidateMappedMemoryRanges(device_, 1, &range);
}

void MemoryManager::Clear() noexcept {
    for (UniquePtr<Page>& page : pages_) {
        if (page.Get() != nullptr) {
            functions_->vkFreeMemory(device_, page->memory, nullptr);
            page = UniquePtr<Page>();
        }
    }

    pages_.Clear();
}

usize MemoryManager::DeviceMemoryCount() const noexcept {
    usize count = dedicatedCount_;
    for (const UniquePtr<Page>& page : pages_) {
        count += page.Get() != nullptr ? 1u : 0u;
    }

    return count;
}

bool MemoryManager::PickDeviceType(uint32 typeBits, uint32& type) const noexcept {
    type = PickType(typeBits, MemoryUsage::Device);
    return type != NoMemoryType;
}

} // namespace rocks::graphics::vulkan
