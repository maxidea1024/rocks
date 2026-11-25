#include "VulkanDeviceImpl.h"

#include <Platform/Memory.h>

// 상수 버퍼(Docs/Design/RenderBase 9.11절 GX6)다. handle은 host-visible page 버퍼의 구간, 기록
// 명령(SetConstantData)은 host-visible ring이다. GPU가 그대로 읽는다(coherent, 복사 명령 없음).
// descriptor는 UNIFORM_BUFFER_DYNAMIC(버퍼, 0, 선언 크기)이고 구간 위치는 동적 offset이다.
namespace rocks::graphics::vulkan {

namespace {

constexpr VkBufferUsageFlags ConstantUsage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

} // namespace

Result<void> VulkanDevice::InitializeConstants() noexcept {
    auto reset = constantRing_.Reset(DefaultConstantRingBytes);
    if (!reset) {
        return reset;
    }

    return CreateBufferWithMemory(DefaultConstantRingBytes, ConstantUsage, MemoryUsage::Upload,
                                  constantRingBuffer_, constantRingMemory_);
}

void VulkanDevice::DestroyConstants() noexcept {
    for (UniquePtr<ConstantPage>& page : constantPages_) {
        functions_.vkDestroyBuffer(device_, page->buffer, nullptr);
        FreeMemoryLocked(page->memory);
    }

    constantPages_.Clear();
    constantBuffers_.Clear();
    if (constantRingBuffer_ != VK_NULL_HANDLE) {
        functions_.vkDestroyBuffer(device_, constantRingBuffer_, nullptr);
        constantRingBuffer_ = VK_NULL_HANDLE;
    }

    FreeMemoryLocked(constantRingMemory_);
}

Result<void> VulkanDevice::AllocateConstantBlock(uint64 bytes, uint32& page,
                                                 GpuPageAllocation& block) noexcept {
    const uint64 size = AlignConstantBufferBytes(bytes);
    for (usize index = 0; index < constantPages_.Size(); ++index) {
        auto allocated = constantPages_[index]->ranges.Allocate(size, ConstantBufferAlignment);
        if (allocated) {
            page = static_cast<uint32>(index);
            block = allocated.Value();
            return Result<void>::Success();
        }

        if (allocated.Error() != ErrorCode::CapacityExceeded) {
            return Result<void>::Failure(allocated.Error());
        }
    }

    auto made = UniquePtr<ConstantPage>::TryMake(allocator_, allocator_);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    ConstantPage& fresh = *made.Value();
    auto reset = fresh.ranges.Reset(ConstantPageBytes);
    if (!reset) {
        return reset;
    }

    auto created = CreateBufferWithMemory(ConstantPageBytes, ConstantUsage, MemoryUsage::Upload,
                                          fresh.buffer, fresh.memory);
    if (!created) {
        return created;
    }

    auto allocated = fresh.ranges.Allocate(size, ConstantBufferAlignment);
    if (!allocated || !constantPages_.TryPushBack(Move(made.Value()))) {
        functions_.vkDestroyBuffer(device_, fresh.buffer, nullptr);
        FreeMemoryLocked(fresh.memory);
        return Result<void>::Failure(!allocated ? allocated.Error() : ErrorCode::OutOfMemory);
    }

    page = static_cast<uint32>(constantPages_.Size() - 1);
    block = allocated.Value();
    return Result<void>::Success();
}

void VulkanDevice::RetireConstantBlock(uint32 page, const GpuPageAllocation& block) noexcept {
    Retired entry;
    entry.constantPage = static_cast<int64>(page);
    entry.constantBlock = block;
    RetireEntry(Move(entry));
}

uint8* VulkanDevice::ConstantData(uint32 page, const GpuPageAllocation& block) const noexcept {
    return constantPages_[page]->memory.mapped + block.offset;
}

Result<ConstantBufferHandle>
VulkanDevice::CreateConstantBuffer(ConstantBufferLifetime lifetime,
                                   Span<const uint8> contents) noexcept {
    platform::LockGuard lock(gate_);
    if ((lifetime != ConstantBufferLifetime::SingleFrame &&
         lifetime != ConstantBufferLifetime::MultiFrame) ||
        !IsValidConstantBufferBytes(contents.Size())) {
        auto failed = FailCode("CreateConstantBuffer", ErrorCode::InvalidArgument,
                               "constant buffer contents must be 16-byte multiples of 16 to 65536 "
                               "bytes");
        return Result<ConstantBufferHandle>::Failure(failed.Error());
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<ConstantBufferHandle>::Failure(reserved.Error());
    }

    ConstantBufferRecord record;
    record.lifetime = lifetime;
    record.bytes = static_cast<uint32>(contents.Size());
    auto allocated = AllocateConstantBlock(contents.Size(), record.page, record.block);
    if (!allocated) {
        return Result<ConstantBufferHandle>::Failure(allocated.Error());
    }

    platform::MemoryCopy(ConstantData(record.page, record.block), contents.Data(), contents.Size());
    const uint32 page = record.page;
    const GpuPageAllocation block = record.block;
    auto added = constantBuffers_.TryAdd(Move(record));
    if (!added) {
        constantPages_[page]->ranges.Free(block);
        return Result<ConstantBufferHandle>::Failure(added.Error());
    }

    return Result<ConstantBufferHandle>::Success(ConstantBufferHandle{added.Value()});
}

Result<void> VulkanDevice::UpdateConstantBuffer(ConstantBufferHandle buffer,
                                                Span<const uint8> contents) noexcept {
    platform::LockGuard lock(gate_);
    ConstantBufferRecord* record = constantBuffers_.Find(buffer.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (record->lifetime != ConstantBufferLifetime::MultiFrame ||
        contents.Size() != record->bytes) {
        return FailCode("UpdateConstantBuffer", ErrorCode::InvalidArgument,
                        "only MultiFrame constant buffers can be updated, with their size");
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return reserved;
    }

    // 새 자리를 받아 옮긴다(rename): 앞에 기록한 그리기의 descriptor는 옛 구간을 가리킨다.
    uint32 page = 0;
    GpuPageAllocation block;
    auto allocated = AllocateConstantBlock(contents.Size(), page, block);
    if (!allocated) {
        return allocated;
    }

    platform::MemoryCopy(ConstantData(page, block), contents.Data(), contents.Size());
    RetireConstantBlock(record->page, record->block);
    record->page = page;
    record->block = block;
    return Result<void>::Success();
}

void VulkanDevice::Retire(ConstantBufferHandle buffer) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    ConstantBufferRecord* record = constantBuffers_.Find(buffer.slot);
    if (record == nullptr) {
        return;
    }

    RetireConstantBlock(record->page, record->block);
    auto removed = constantBuffers_.TryRemove(buffer.slot);
    (void)removed;
}

void VulkanDevice::SetConstantBuffer(ShaderStage stage, uint32 slot,
                                     ConstantBufferHandle buffer) noexcept {
    platform::LockGuard lock(gate_);
    const uint32 index = ConstantBufferStageIndex(stage);
    if (index >= ConstantBufferStageCount || slot >= MaxConstantBufferSlots) {
        return;
    }

    const bool live = constantBuffers_.Find(buffer.slot) != nullptr;
    boundConstants_[index][slot] = live ? BoundConstant{buffer.slot, false, 0, 0} : BoundConstant{};
}

Result<void> VulkanDevice::SetConstantData(ShaderStage stage, uint32 slot,
                                           Span<const uint8> bytes) noexcept {
    platform::LockGuard lock(gate_);
    const uint32 index = ConstantBufferStageIndex(stage);
    if (!recording_ || index >= ConstantBufferStageCount || slot >= MaxConstantBufferSlots ||
        !IsValidConstantBufferBytes(bytes.Size())) {
        return FailCode("SetConstantData", ErrorCode::InvalidArgument,
                        "SetConstantData needs a recording, a stage slot below 14 and 16-byte "
                        "multiples of 16 to 65536 bytes");
    }

    // 자리가 없으면 끝난 제출을 회수하고, 앞 제출이 쓰는 가장 오래된 묶음을 기다린다.
    const uint64 size = AlignConstantBufferBytes(bytes.Size());
    Result<uint64> offset = Result<uint64>::Failure(ErrorCode::CapacityExceeded);
    for (;;) {
        constantRing_.Reclaim(CompletedValue());
        offset = constantRing_.Allocate(size, ConstantBufferAlignment);
        if (offset || offset.Error() != ErrorCode::CapacityExceeded) {
            break;
        }

        const uint64 oldest = constantRing_.OldestFence();
        if (oldest == 0 || oldest == RetainUntilDestructionFence || !ownership_.IsOwner()) {
            break;
        }

        auto waited = WaitFenceOwner(oldest);
        if (!waited) {
            offset = Result<uint64>::Failure(waited.Error());
            break;
        }
    }

    if (!offset) {
        boundConstants_[index][slot] = {};
        return FailCode("SetConstantData", offset.Error(),
                        "the constant ring of this recording is full");
    }

    platform::MemoryCopy(constantRingMemory_.mapped + offset.Value(), bytes.Data(), bytes.Size());
    boundConstants_[index][slot] =
        BoundConstant{SlotHandle{}, true, offset.Value(), static_cast<uint32>(bytes.Size())};
    return Result<void>::Success();
}

VkBuffer VulkanDevice::ResolveConstant(uint32 stage, uint32 slot, VkDeviceSize& offset,
                                       uint32& bytes) const noexcept {
    const BoundConstant& bound = boundConstants_[stage][slot];
    if (bound.data) {
        offset = bound.offset;
        bytes = bound.bytes;
        return constantRingBuffer_;
    }

    const ConstantBufferRecord* record = constantBuffers_.Find(bound.handle);
    if (record == nullptr) {
        offset = 0;
        bytes = 0;
        return VK_NULL_HANDLE;
    }

    offset = record->block.offset;
    bytes = record->bytes;
    return constantPages_[record->page]->buffer;
}

} // namespace rocks::graphics::vulkan
