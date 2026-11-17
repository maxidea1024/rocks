#include "VulkanDeviceImpl.h"

#include <Platform/Memory.h>

// 업로드 ring·staging·readback(GX8, Docs/Design/VulkanGraphics.ko.md 8·10절)이다. 업로드는 별도
// 명령 버퍼에 쌓이고 소유 스레드의 다음 제출이 그 제출보다 먼저 실행한다.
namespace rocks::graphics::vulkan {

namespace {

// 업로드 묶음이 앞 제출의 정점·index·셰이더 읽기와 전송을 기다리는 단계다(WAR·WAW).
constexpr VkPipelineStageFlags EarlierUseStages =
    VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
// 업로드 묶음 뒤 제출이 업로드 결과를 읽는 단계와 접근이다.
constexpr VkPipelineStageFlags LaterUseStages = EarlierUseStages;
constexpr VkAccessFlags LaterUseAccess = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                                         VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                                         VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;

void GlobalBarrier(const Functions& functions, VkCommandBuffer buffer, VkPipelineStageFlags from,
                   VkAccessFlags fromAccess, VkPipelineStageFlags to,
                   VkAccessFlags toAccess) noexcept {
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = fromAccess;
    barrier.dstAccessMask = toAccess;
    functions.vkCmdPipelineBarrier(buffer, from, to, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

} // namespace

Result<StagingMemory> StagingSourceImpl::CreateStaging(uint64 bytes) noexcept {
    return owner->CreateStagingMemory(bytes);
}

void StagingSourceImpl::DestroyStaging(const StagingMemory& memory) noexcept {
    owner->DestroyStagingMemory(memory);
}

// StagingPool이 자기 잠금 안에서 부른다(다른 스레드일 수 있다). 장치 잠금을 잡지 않고 메모리
// 잠금(leaf)만 잡으며 LastError를 쓰지 않는다.
Result<StagingMemory> VulkanDevice::CreateStagingMemory(uint64 bytes) noexcept {
    VkBufferCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult result = functions_.vkCreateBuffer(device_, &info, nullptr, &buffer);
    if (result != VK_SUCCESS) {
        return Result<StagingMemory>::Failure(ErrorCode::OutOfMemory);
    }

    VkMemoryRequirements2 requirements = {};
    requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    VkBufferMemoryRequirementsInfo2 query = {};
    query.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
    query.buffer = buffer;
    functions_.vkGetBufferMemoryRequirements2(device_, &query, &requirements);
    MemoryRequest request;
    request.size = requirements.memoryRequirements.size;
    request.alignment = requirements.memoryRequirements.alignment;
    request.typeBits = requirements.memoryRequirements.memoryTypeBits;
    platform::LockGuard lock(memoryGate_);
    auto allocated = memory_.Allocate(request, MemoryUsage::Upload, ResourceShape::Linear, result);
    if (!allocated) {
        functions_.vkDestroyBuffer(device_, buffer, nullptr);
        return Result<StagingMemory>::Failure(allocated.Error());
    }

    result = functions_.vkBindBufferMemory(device_, buffer, allocated.Value().memory,
                                           allocated.Value().offset);
    auto block = UniquePtr<StagingBlock>::TryMake(allocator_);
    auto reserved = stagingBlocks_.TryReserve(stagingBlocks_.Size() + 1);
    if (result != VK_SUCCESS || !block || !reserved) {
        functions_.vkDestroyBuffer(device_, buffer, nullptr);
        memory_.Free(allocated.Value());
        return Result<StagingMemory>::Failure(result != VK_SUCCESS ? ErrorCode::DependencyFailed
                                                                   : ErrorCode::OutOfMemory);
    }

    block.Value()->buffer = buffer;
    block.Value()->memory = allocated.Value();
    StagingMemory staging;
    staging.resource = block.Value().Get();
    staging.data = allocated.Value().mapped;
    staging.size = bytes;
    auto pushed = stagingBlocks_.TryPushBack(Move(block.Value()));
    (void)pushed;
    return Result<StagingMemory>::Success(staging);
}

void VulkanDevice::DestroyStagingMemory(const StagingMemory& memory) noexcept {
    platform::LockGuard lock(memoryGate_);
    for (usize index = 0; index < stagingBlocks_.Size(); ++index) {
        StagingBlock* block = stagingBlocks_[index].Get();
        if (block == memory.resource) {
            functions_.vkDestroyBuffer(device_, block->buffer, nullptr);
            memory_.Free(block->memory);
            auto removed = stagingBlocks_.TryRemoveAtSwap(index);
            (void)removed;
            return;
        }
    }
}

Result<void> VulkanDevice::CreateUploadRing(const DeviceDesc& desc) noexcept {
    // 업로드 ring(GX8): HOST_VISIBLE 버퍼 하나를 계속 map해 두고 구간을 나눠 쓴다.
    const uint64 ringBytes =
        desc.uploadRingBytes != 0 ? desc.uploadRingBytes : DefaultUploadRingBytes;
    auto reset = ring_.Reset(ringBytes);
    if (!reset) {
        return reset;
    }

    return CreateBufferWithMemory(ringBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::Upload,
                                  ringBuffer_, ringMemory_);
}

Result<void> VulkanDevice::OpenUploadList() noexcept {
    if (uploadOpen_) {
        return Result<void>::Success();
    }

    auto picked = PickCommandBuffer();
    if (!picked) {
        return Result<void>::Failure(picked.Error());
    }

    CommandRecord& record = commands_[picked.Value()];
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const VkResult result = functions_.vkBeginCommandBuffer(record.buffer, &begin);
    if (result != VK_SUCCESS) {
        return Fail("vkBeginCommandBuffer", result);
    }

    record.inUse = true;
    upload_ = record.buffer;
    uploadRecord_ = picked.Value();
    uploadOpen_ = true;
    bufferWriteCount_ = 0;
    // 앞 제출이 아직 읽거나 쓰는 버퍼를 덮어쓰지 않게 한다(WAR·WAW). 이미지는 복사마다 layout
    // 전이 barrier가 같은 일을 한다.
    GlobalBarrier(functions_, upload_, EarlierUseStages, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    return Result<void>::Success();
}

// size byte 업로드 공간을 잡는다(잠금 안). ring에 자리가 없으면 소유 스레드는 쌓인 업로드를
// 제출하고 가장 오래된 묶음의 fence를 기다려 비운다. 다른 스레드이거나 그래도 없으면 전용
// staging 버퍼를 만들고 그 업로드 묶음의 fence 뒤에 놓는다.
Result<VulkanDevice::UploadSpace> VulkanDevice::StageUpload(uint64 size) noexcept {
    for (;;) {
        PollCompleted();
        ring_.Reclaim(CompletedValue());
        auto offset = ring_.Allocate(size, UploadAlignment);
        if (offset) {
            return Result<UploadSpace>::Success(
                UploadSpace{ringBuffer_, offset.Value(), ringMemory_.mapped + offset.Value()});
        }

        if (offset.Error() != ErrorCode::CapacityExceeded || !ownership_.IsOwner()) {
            break;
        }

        if (ring_.HasOpenAllocations()) {
            auto flushed = FlushUploadsLocked();
            if (!flushed) {
                return Result<UploadSpace>::Failure(flushed.Error());
            }

            continue;
        }

        const uint64 oldest = ring_.OldestFence();
        if (oldest == 0 || oldest == RetainUntilDestructionFence) {
            break;
        }

        auto waited = WaitFenceOwner(oldest);
        if (!waited) {
            return Result<UploadSpace>::Failure(waited.Error());
        }
    }

    auto reserved = retired_.TryEnsureCapacity(retired_.Size() + 1);
    if (!reserved) {
        return Result<UploadSpace>::Failure(reserved.Error());
    }

    Retired entry;
    auto created = CreateBufferWithMemory(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          MemoryUsage::Upload, entry.buffer, entry.memory);
    if (!created) {
        return Result<UploadSpace>::Failure(created.Error());
    }

    const UploadSpace space{entry.buffer, 0, entry.memory.mapped};
    entry.uploadPending = true;
    auto pushed = retired_.TryPushBack(Move(entry));
    (void)pushed;
    ++dedicatedUploads_;
    return Result<UploadSpace>::Success(space);
}

void VulkanDevice::RecordBufferCopy(VkBuffer source, uint64 sourceOffset, VkBuffer target,
                                    uint64 targetOffset, uint64 size) noexcept {
    // 같은 묶음에서 겹치는 구간을 두 번 쓰면 두 복사 사이에 순서가 없으므로 barrier를 둔다.
    bool overlaps = bufferWriteCount_ == MaxTrackedBufferWrites;
    for (uint32 index = 0; index < bufferWriteCount_ && !overlaps; ++index) {
        const BufferWrite& write = bufferWrites_[index];
        overlaps =
            write.buffer == target && targetOffset < write.end && write.begin < targetOffset + size;
    }

    if (overlaps) {
        GlobalBarrier(functions_, upload_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT);
        bufferWriteCount_ = 0;
    }

    bufferWrites_[bufferWriteCount_++] = {target, targetOffset, targetOffset + size};
    VkBufferCopy region = {};
    region.srcOffset = sourceOffset;
    region.dstOffset = targetOffset;
    region.size = size;
    functions_.vkCmdCopyBuffer(upload_, source, target, 1, &region);
}

Result<void> VulkanDevice::RecordBufferUpload(VkBuffer target, uint64 offset,
                                              Span<const uint8> data) noexcept {
    auto space = StageUpload(data.Size());
    if (!space) {
        return Result<void>::Failure(space.Error());
    }

    platform::MemoryCopy(space.Value().data, data.Data(), data.Size());
    auto opened = OpenUploadList();
    if (!opened) {
        return opened;
    }

    RecordBufferCopy(space.Value().buffer, space.Value().offset, target, offset, data.Size());
    return Result<void>::Success();
}

void VulkanDevice::RecordTextureCopy(const TextureRecord& record, uint32 mipLevel, VkBuffer source,
                                     uint64 offset, bool initial) noexcept {
    // 업로드 기록은 프레임 기록보다 먼저 실행되므로 상태 중립이다(GX5): 처음 생성이면 내용이
    // 없으므로 UNDEFINED에서 복사 대상으로 바꿨다가 셰이더 읽기 상태로, 아니면 제출한 상태
    // (committed)에서 바꿨다가 되돌린다.
    const ResourceAccess resting =
        initial ? CanonicalAccess(ResourceAccess::SrvGraphics) : record.committed[mipLevel];
    (void)AccessBarrier(upload_, record.image, mipLevel, 1,
                        initial ? ResourceAccess::Unknown : resting,
                        ResourceAccess::CopyDestination, false);
    // 빽빽한 행이다(bufferRowLength = 0). Vulkan은 원본 offset이 texel 크기 배수이기만 하면 된다.
    VkBufferImageCopy region = {};
    region.bufferOffset = offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, 0, 1};
    region.imageExtent = {MipExtent(record.width, mipLevel), MipExtent(record.height, mipLevel), 1};
    functions_.vkCmdCopyBufferToImage(upload_, source, record.image,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    (void)AccessBarrier(upload_, record.image, mipLevel, 1, ResourceAccess::CopyDestination,
                        resting, false);
}

Result<void> VulkanDevice::RecordTextureUpload(const TextureRecord& record, uint32 firstMip,
                                               uint32 count, Span<const uint8> pixels,
                                               bool initial) noexcept {
    auto space = StageUpload(pixels.Size());
    if (!space) {
        return Result<void>::Failure(space.Error());
    }

    // 원본(빈틈 없는 행, mip을 잇는 배치)을 그대로 옮긴다. 장치 행 정렬로 다시 배치할 필요가 없다.
    platform::MemoryCopy(space.Value().data, pixels.Data(), pixels.Size());
    auto opened = OpenUploadList();
    if (!opened) {
        return opened;
    }

    const VkBuffer source = space.Value().buffer;
    uint64 offset = space.Value().offset;
    for (uint32 index = 0; index < count; ++index) {
        const uint32 level = firstMip + index;
        RecordTextureCopy(record, level, source, offset, initial);
        offset += PixelFormatImageBytes(record.format, MipExtent(record.width, level),
                                        MipExtent(record.height, level));
    }

    return Result<void>::Success();
}

// 쌓인 업로드 기록을 제출하고 그 fence 값을 돌려준다(잠금 안, 소유 스레드). 없으면 0이다.
Result<uint64> VulkanDevice::FlushUploadsLocked() noexcept {
    if (!uploadOpen_) {
        return Result<uint64>::Success(0);
    }

    uploadOpen_ = false;
    CommandRecord& record = commands_[uploadRecord_];
    record.inUse = false;
    // 뒤 제출의 정점·index·셰이더·전송이 업로드 결과를 본다(같은 queue 제출 순서가 잇는다).
    GlobalBarrier(functions_, upload_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  LaterUseStages, LaterUseAccess);
    VkResult result = functions_.vkEndCommandBuffer(upload_);
    if (result != VK_SUCCESS) {
        // 아무것도 제출하지 않았다. 기록이 쓰던 공간은 지금까지의 제출 뒤에 다시 쓴다.
        record.fenceValue = fenceValue_;
        ring_.CloseBatch(fenceValue_);
        staging_->AssignPending(fenceValue_);
        AssignPendingUploads(fenceValue_);
        return FailWith<uint64>("vkEndCommandBuffer", result);
    }

    auto signaled = Submit(upload_, VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    const uint64 value = signaled ? signaled.Value() : RetainUntilDestructionFence;
    record.fenceValue = value;
    ring_.CloseBatch(value);
    staging_->AssignPending(value);
    AssignPendingUploads(value);
    if (!signaled) {
        return Result<uint64>::Failure(signaled.Error());
    }

    return Result<uint64>::Success(value);
}

Result<void> VulkanDevice::UpdateBuffer(BufferHandle handle, uint64 offset,
                                        Span<const uint8> data) noexcept {
    platform::LockGuard lock(gate_);
    const BufferRecord* record = buffers_.Find(handle.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (data.Size() == 0 || offset > record->size || data.Size() > record->size - offset) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return RecordBufferUpload(record->buffer, offset, data);
}

Result<void> VulkanDevice::UpdateTexture(TextureHandle handle, uint32 mipLevel,
                                         Span<const uint8> pixels) noexcept {
    platform::LockGuard lock(gate_);
    const TextureRecord* record = textures_.Find(handle.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (mipLevel >= record->mipLevels ||
        pixels.Size() != PixelFormatImageBytes(record->format, MipExtent(record->width, mipLevel),
                                               MipExtent(record->height, mipLevel))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const TextureRecord copy = *record;
    return RecordTextureUpload(copy, mipLevel, 1, pixels, false);
}

Result<GpuFencePoint> VulkanDevice::SubmitUploads() noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<GpuFencePoint>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);
    if (recording_) {
        return Result<GpuFencePoint>::Failure(ErrorCode::Busy);
    }

    auto flushed = FlushUploadsLocked();
    if (!flushed) {
        return Result<GpuFencePoint>::Failure(flushed.Error());
    }

    return Result<GpuFencePoint>::Success(timeline_.Point(flushed.Value()));
}

UploadStats VulkanDevice::UploadStatistics() const noexcept {
    platform::LockGuard lock(gate_);
    UploadStats stats;
    stats.ringCapacity = ring_.Capacity();
    stats.ringUsed = ring_.UsedBytes();
    stats.ringWraps = ring_.Wraps();
    stats.dedicatedUploads = dedicatedUploads_;
    stats.pending = uploadOpen_;
    return stats;
}

Result<StagingBuffer> VulkanDevice::AcquireStaging(uint64 bytes) noexcept {
    return staging_->Acquire(bytes);
}

Result<void> VulkanDevice::UploadTextureFromStaging(TextureHandle handle, uint32 mipLevel,
                                                    StagingHandle staging, uint64 offset) noexcept {
    platform::LockGuard lock(gate_);
    const TextureRecord* record = textures_.Find(handle.slot);
    auto memory = staging_->Find(staging);
    if (record == nullptr || !memory) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    const uint64 bytes = PixelFormatImageBytes(record->format, MipExtent(record->width, mipLevel),
                                               MipExtent(record->height, mipLevel));
    if (mipLevel >= record->mipLevels || offset > memory.Value().size ||
        bytes > memory.Value().size - offset) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const TextureRecord copy = *record;
    // 원본 offset이 texel 크기 배수가 아니면 ring으로 옮겨 올린다(vkCmdCopyBufferToImage 규칙).
    if (offset % GetPixelFormatInfo(copy.format).blockBytes != 0) {
        return RecordTextureUpload(
            copy, mipLevel, 1,
            Span<const uint8>(memory.Value().data + offset, static_cast<usize>(bytes)), false);
    }

    auto opened = OpenUploadList();
    if (!opened) {
        return opened;
    }

    auto marked = staging_->MarkPending(staging);
    if (!marked) {
        return marked;
    }

    const auto* block = static_cast<const StagingBlock*>(memory.Value().resource);
    RecordTextureCopy(copy, mipLevel, block->buffer, offset, false);
    return Result<void>::Success();
}

Result<void> VulkanDevice::UploadBufferFromStaging(BufferHandle handle, uint64 destinationOffset,
                                                   StagingHandle staging, uint64 sourceOffset,
                                                   uint64 size) noexcept {
    platform::LockGuard lock(gate_);
    const BufferRecord* record = buffers_.Find(handle.slot);
    auto memory = staging_->Find(staging);
    if (record == nullptr || !memory) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (size == 0 || destinationOffset > record->size || size > record->size - destinationOffset ||
        sourceOffset > memory.Value().size || size > memory.Value().size - sourceOffset) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const VkBuffer target = record->buffer;
    auto opened = OpenUploadList();
    if (!opened) {
        return opened;
    }

    auto marked = staging_->MarkPending(staging);
    if (!marked) {
        return marked;
    }

    const auto* block = static_cast<const StagingBlock*>(memory.Value().resource);
    RecordBufferCopy(block->buffer, sourceOffset, target, destinationOffset, size);
    return Result<void>::Success();
}

void VulkanDevice::ReleaseStaging(StagingHandle staging) noexcept {
    staging_->Release(staging);
}

StagingStats VulkanDevice::StagingStatistics() const noexcept {
    return staging_->Stats();
}

// RenderTarget 텍스처를 readback 버퍼로 복사하는 기록을 쌓고 바로 제출한다(잠금 안).
Result<ReadbackRequest> VulkanDevice::RequestReadbackLocked(TextureHandle target) noexcept {
    const TextureRecord* found = textures_.Find(target.slot);
    if (found == nullptr) {
        return Result<ReadbackRequest>::Failure(ErrorCode::StaleHandle);
    }

    if (!found->renderTarget) {
        return Result<ReadbackRequest>::Failure(ErrorCode::InvalidArgument);
    }

    const TextureRecord texture = *found;
    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<ReadbackRequest>::Failure(reserved.Error());
    }

    ReadbackRecord readback;
    readback.width = texture.width;
    readback.height = texture.height;
    readback.bytesPerPixel = GetPixelFormatInfo(texture.format).blockBytes;
    const uint64 bytes =
        static_cast<uint64>(readback.width) * readback.height * readback.bytesPerPixel;
    auto created = CreateBufferWithMemory(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          MemoryUsage::Readback, readback.buffer, readback.memory);
    if (!created) {
        return Result<ReadbackRequest>::Failure(created.Error());
    }

    auto opened = OpenUploadList();
    if (!opened) {
        functions_.vkDestroyBuffer(device_, readback.buffer, nullptr);
        FreeMemoryLocked(readback.memory);
        return Result<ReadbackRequest>::Failure(opened.Error());
    }

    // 업로드 기록은 상태 중립이다: 제출한 상태에서 복사 원본으로 바꿨다가 되돌린다. 이미 복사 원본
    // 상태면(그래프가 CopySource로 끝냈다) barrier를 내지 않는다.
    const ResourceAccess resting = texture.committed[0];
    (void)AccessBarrier(upload_, texture.image, 0, 1, resting, ResourceAccess::CopySource, false);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {texture.width, texture.height, 1};
    functions_.vkCmdCopyImageToBuffer(upload_, texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      readback.buffer, 1, &region);
    (void)AccessBarrier(upload_, texture.image, 0, 1, ResourceAccess::CopySource, resting, false);
    // fence 뒤 CPU가 읽도록 전송 쓰기를 host 읽기에 보인다.
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
        Retired entry;
        entry.buffer = readback.buffer;
        entry.memory = readback.memory;
        KeepUntilDestruction(Move(entry));
        return Result<ReadbackRequest>::Failure(flushed.Error());
    }

    readback.fenceValue = flushed.Value();
    const VkBuffer buffer = readback.buffer;
    const MemoryAllocation memory = readback.memory;
    auto added = readbacks_.TryAdd(Move(readback));
    if (!added) {
        Retired entry;
        entry.buffer = buffer;
        entry.memory = memory;
        RetireEntry(Move(entry));
        return Result<ReadbackRequest>::Failure(added.Error());
    }

    ReadbackRequest request;
    request.handle = ReadbackHandle{added.Value()};
    request.completion = timeline_.Point(flushed.Value());
    request.bytes = bytes;
    return Result<ReadbackRequest>::Success(request);
}

Result<bool> VulkanDevice::TryReadbackLocked(ReadbackHandle handle, Span<uint8> out) noexcept {
    const ReadbackRecord* record = readbacks_.Find(handle.slot);
    if (record == nullptr) {
        return Result<bool>::Failure(ErrorCode::StaleHandle);
    }

    const usize bytes = static_cast<usize>(record->width) * record->height * record->bytesPerPixel;
    if (out.Size() != bytes) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    // 손실 뒤에는 fence가 끝난 것처럼 보여도 내용이 없다(사양 "Lost Device").
    PollCompleted();
    if (timeline_.IsLost()) {
        return Result<bool>::Failure(ErrorCode::Destroyed);
    }

    if (completedValue_ < record->fenceValue) {
        return Result<bool>::Success(false);
    }

    {
        platform::LockGuard lock(memoryGate_);
        memory_.Invalidate(record->memory);
    }

    platform::MemoryCopy(out.Data(), record->memory.mapped, bytes);
    return Result<bool>::Success(true);
}

void VulkanDevice::ReleaseReadbackLocked(ReadbackHandle handle) noexcept {
    ReadbackRecord* record = readbacks_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }

    Retired entry;
    entry.buffer = record->buffer;
    entry.memory = record->memory;
    RetireEntry(Move(entry));
    auto removed = readbacks_.TryRemove(handle.slot);
    (void)removed;
}

Result<ReadbackRequest> VulkanDevice::RequestReadback(TextureHandle target) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<ReadbackRequest>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);
    if (recording_) {
        return Result<ReadbackRequest>::Failure(ErrorCode::Busy);
    }

    return RequestReadbackLocked(target);
}

Result<bool> VulkanDevice::TryReadback(ReadbackHandle handle, Span<uint8> out) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<bool>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);
    return TryReadbackLocked(handle, out);
}

void VulkanDevice::ReleaseReadback(ReadbackHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    ReleaseReadbackLocked(handle);
}

Result<void> VulkanDevice::ReadPixels(TextureHandle handle, Span<uint8> out) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);
    const TextureRecord* record = textures_.Find(handle.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    const usize size =
        static_cast<usize>(PixelFormatImageBytes(record->format, record->width, record->height));
    if (!record->renderTarget || out.Size() != size || recording_) {
        return Result<void>::Failure(recording_ ? ErrorCode::Busy : ErrorCode::InvalidArgument);
    }

    // 비동기 readback과 같은 경로로 복사를 제출하고 그 fence를 기다린다(시험·캡처용 동기 경로).
    auto requested = RequestReadbackLocked(handle);
    if (!requested) {
        return Result<void>::Failure(requested.Error());
    }

    const ReadbackHandle readback = requested.Value().handle;
    auto waited = WaitFenceOwner(requested.Value().completion.value);
    Result<void> outcome = Result<void>::Success();
    if (!waited) {
        outcome = Move(waited);
    } else {
        auto copied = TryReadbackLocked(readback, out);
        if (!copied || !copied.Value()) {
            outcome =
                FailCode("ReadPixels", ErrorCode::DependencyFailed, "readback did not complete");
        }
    }

    ReleaseReadbackLocked(readback);
    return outcome;
}

} // namespace rocks::graphics::vulkan
