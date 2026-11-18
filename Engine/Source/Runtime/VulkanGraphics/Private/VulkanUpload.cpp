#include "VulkanDeviceImpl.h"
#include "VulkanFormats.h"

#include <Platform/Memory.h>

// 업로드 ring·staging·readback(GX8, Docs/Design/VulkanGraphics.ko.md 8·10절)이다. 업로드는 별도
// 명령 버퍼에 쌓이고 소유 스레드의 다음 제출이 그 제출보다 먼저 실행한다.
namespace rocks::graphics::vulkan {

namespace {

// 업로드 묶음이 앞 제출의 간접 인자·정점·index·셰이더(상수·버퍼 view 포함) 읽기와 전송을
// 기다리는 단계다(WAR·WAW).
constexpr VkPipelineStageFlags EarlierUseStages =
    VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
// 업로드 묶음 뒤 제출이 업로드 결과를 읽는 단계와 접근이다.
constexpr VkPipelineStageFlags LaterUseStages = EarlierUseStages;
constexpr VkAccessFlags LaterUseAccess =
    VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
    VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
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

// 버퍼→이미지 복사 원본 offset이 지켜야 할 정렬이다. 색은 texel(블록) 크기, 깊이·스텐실은 4의
// 배수여야 한다(Vulkan 사양 VkBufferImageCopy). 둘의 최소공배수(4와의)를 써서 모든 plane에 맞춘다.
uint64 CopyOffsetAlignment(PixelFormat format) noexcept {
    const PixelFormatInfo& info = GetPixelFormatInfo(format);
    uint64 alignment = 4;
    for (uint32 plane = 0; plane < info.planeCount; ++plane) {
        const PixelFormat planeFormat = info.planes[plane].format;
        uint64 element = planeFormat == PixelFormat::Unknown
                             ? info.blockBytes
                             : GetPixelFormatInfo(planeFormat).blockBytes;
        if (element == 0) {
            continue;
        }

        uint64 a = alignment;
        uint64 b = element;
        while (b != 0) {
            const uint64 rest = a % b;
            a = b;
            b = rest;
        }
        alignment = alignment / a * element;
    }

    return alignment;
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
    // 같은 묶음의 복사끼리는 순서가 없다. 앞 복사가 쓴 구간을 다시 쓰거나(WAW) 원본으로 읽으면(RAW,
    // 초기 데이터를 올린 직후의 ReadBuffer 등) 그 사이에 barrier를 둔다.
    const auto overlapping = [this](VkBuffer buffer, uint64 begin, uint64 end) noexcept {
        for (uint32 index = 0; index < bufferWriteCount_; ++index) {
            const BufferWrite& write = bufferWrites_[index];
            if (write.buffer == buffer && begin < write.end && write.begin < end) {
                return true;
            }
        }

        return false;
    };
    if (bufferWriteCount_ == MaxTrackedBufferWrites ||
        overlapping(target, targetOffset, targetOffset + size) ||
        overlapping(source, sourceOffset, sourceOffset + size)) {
        GlobalBarrier(functions_, upload_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
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
                                     const uint64* offsets, bool initial) noexcept {
    // 업로드 기록은 프레임 기록보다 먼저 실행되므로 상태 중립이다(GX5): 처음 생성이면 내용이
    // 없으므로 UNDEFINED에서 복사 대상으로 바꿨다가 셰이더 읽기 상태로, 아니면 제출한 상태
    // (committed)에서 바꿨다가 되돌린다. barrier는 이미지의 모든 aspect를 함께 옮긴다.
    // 복사는 mip 하나의 모든 layer(배열·cube 면)를 한 번에 옮긴다. layer마다 제출한 상태가 다를
    // 수 있어 같은 상태 구간씩 barrier를 낸다.
    if (initial) {
        (void)AccessBarrier(upload_, record.image, mipLevel, 1, ResourceAccess::Unknown,
                            ResourceAccess::CopyDestination, false, record.aspect, 0,
                            record.slices);
    } else {
        MipLayerBarriers(upload_, record, record.committed, mipLevel,
                         ResourceAccess::CopyDestination, false);
    }
    // plane마다 빽빽한 행이다(bufferRowLength = 0). 깊이·스텐실 plane은 aspect 하나씩 복사하고
    // 비디오 chroma plane은 축소 크기다.
    const PixelFormatInfo& info = GetPixelFormatInfo(record.format);
    VkBufferImageCopy regions[2] = {};
    for (uint32 plane = 0; plane < info.planeCount; ++plane) {
        const PixelFormatPlane& layout = info.planes[plane];
        VkBufferImageCopy& region = regions[plane];
        region.bufferOffset = offsets[plane];
        // 빽빽한 버퍼에서 layer·깊이 면은 이미지 하나씩 이어진다(TextureDesc 초기 픽셀 배치와
        // 같다. 다중 plane 형식은 2D만이다).
        region.imageSubresource = {PlaneAspect(record.format, plane), mipLevel, 0, record.slices};
        region.imageExtent = {MipExtent(record.width, mipLevel) >> layout.widthShift,
                              MipExtent(record.height, mipLevel) >> layout.heightShift,
                              MipDepth(record, mipLevel)};
    }

    functions_.vkCmdCopyBufferToImage(upload_, source, record.image,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, info.planeCount,
                                      regions);
    if (initial) {
        (void)AccessBarrier(upload_, record.image, mipLevel, 1, ResourceAccess::CopyDestination,
                            CanonicalAccess(ResourceAccess::SrvGraphics), false, record.aspect, 0,
                            record.slices);
    } else {
        MipLayerBarriers(upload_, record, record.committed, mipLevel,
                         ResourceAccess::CopyDestination, true);
    }
}

Result<void> VulkanDevice::RecordTextureUpload(const TextureRecord& record, uint32 firstMip,
                                               uint32 count, Span<const uint8> pixels,
                                               bool initial) noexcept {
    // 원본은 mip마다 plane을 잇는 빈틈 없는 배치다. 복사 원본 offset 규칙(texel 크기 배수, 깊이·
    // 스텐실은 4의 배수)을 지키도록 (mip, plane) 구역마다 정렬해 옮긴다.
    const uint32 planes = PixelFormatPlaneCount(record.format);
    const uint64 alignment = CopyOffsetAlignment(record.format);
    auto space = StageUpload(pixels.Size() + (count * planes + 1) * alignment);
    if (!space) {
        return Result<void>::Failure(space.Error());
    }

    auto opened = OpenUploadList();
    if (!opened) {
        return opened;
    }

    const VkBuffer source = space.Value().buffer;
    const uint64 base = space.Value().offset;
    uint64 target = base;
    usize read = 0;
    for (uint32 index = 0; index < count; ++index) {
        const uint32 level = firstMip + index;
        uint64 offsets[2] = {};
        for (uint32 plane = 0; plane < planes; ++plane) {
            // 단일 plane이면 그 mip의 모든 layer·깊이 면이 한 구역이다(다중 plane은 2D만이다).
            const usize bytes = static_cast<usize>(
                PixelFormatPlaneImageBytes(record.format, plane, MipExtent(record.width, level),
                                           MipExtent(record.height, level)) *
                MipDepth(record, level) * record.slices);
            target = (target + alignment - 1) / alignment * alignment;
            platform::MemoryCopy(space.Value().data + (target - base), pixels.Data() + read, bytes);
            offsets[plane] = target;
            target += bytes;
            read += bytes;
        }

        RecordTextureCopy(record, level, source, offsets, initial);
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

    if (record->isView || mipLevel >= record->mipLevels ||
        pixels.Size() != MipBytes(*record, mipLevel)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return RecordTextureUpload(*record, mipLevel, 1, pixels, false);
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

    const uint64 bytes = mipLevel < record->mipLevels ? MipBytes(*record, mipLevel) : 0u;
    if (record->isView || mipLevel >= record->mipLevels || bytes == 0 ||
        offset > memory.Value().size || bytes > memory.Value().size - offset) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const TextureRecord& copy = *record;
    // 원본 offset이 복사 정렬을 지키지 않거나 plane이 둘이면(plane 사이 offset도 정렬해야 한다)
    // ring으로 옮겨 올린다(vkCmdCopyBufferToImage 규칙).
    if (offset % CopyOffsetAlignment(copy.format) != 0 || PixelFormatPlaneCount(copy.format) != 1) {
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
    const uint64 offsets[2] = {offset, 0};
    RecordTextureCopy(copy, mipLevel, block->buffer, offsets, false);
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

    const TextureRecord& texture = *found;
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
    (void)AccessBarrier(upload_, texture.image, 0, 1, resting, ResourceAccess::CopySource, false,
                        texture.aspect);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {texture.width, texture.height, 1};
    functions_.vkCmdCopyImageToBuffer(upload_, texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      readback.buffer, 1, &region);
    (void)AccessBarrier(upload_, texture.image, 0, 1, ResourceAccess::CopySource, resting, false,
                        texture.aspect);
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

// mip 하나의 plane 하나를 host 버퍼로 복사하고 완료를 기다린다(시험·캡처용 동기 경로). 업로드
// 기록 위에 쌓으므로 앞선 초기 데이터·갱신 뒤에 실행되고, 상태 중립이다(제출한 상태로 되돌린다).
Result<void> VulkanDevice::ReadTexture(TextureHandle handle, uint32 mipLevel, uint32 plane,
                                       Span<uint8> out) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return owner;
    }

    platform::LockGuard lock(gate_);
    const TextureRecord* found = textures_.Find(handle.slot);
    if (found == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (recording_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    const TextureRecord& texture = *found;
    const uint32 width = MipExtent(texture.width, mipLevel);
    const uint32 height = MipExtent(texture.height, mipLevel);
    const uint32 depth = mipLevel < texture.mipLevels ? MipDepth(texture, mipLevel) : 1u;
    // 그 mip의 모든 layer·깊이 면을 빽빽한 이미지 순서로 읽는다(초기 픽셀 배치와 같다).
    const uint64 bytes =
        PixelFormatPlaneImageBytes(texture.format, plane, width, height) * depth * texture.slices;
    if (texture.isView || mipLevel >= texture.mipLevels ||
        plane >= PixelFormatPlaneCount(texture.format) || bytes == 0 || out.Size() != bytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return reserved;
    }

    Retired buffer;
    auto created = CreateBufferWithMemory(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          MemoryUsage::Readback, buffer.buffer, buffer.memory);
    if (!created) {
        return created;
    }

    auto opened = OpenUploadList();
    if (!opened) {
        DestroyRetired(buffer);
        return opened;
    }

    MipLayerBarriers(upload_, texture, texture.committed, mipLevel, ResourceAccess::CopySource,
                     false);
    const PixelFormatPlane& layout = GetPixelFormatInfo(texture.format).planes[plane];
    VkBufferImageCopy region = {};
    region.imageSubresource = {PlaneAspect(texture.format, plane), mipLevel, 0, texture.slices};
    region.imageExtent = {width >> layout.widthShift, height >> layout.heightShift, depth};
    functions_.vkCmdCopyImageToBuffer(upload_, texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      buffer.buffer, 1, &region);
    MipLayerBarriers(upload_, texture, texture.committed, mipLevel, ResourceAccess::CopySource,
                     true);
    VkBufferMemoryBarrier host = {};
    host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = buffer.buffer;
    host.size = VK_WHOLE_SIZE;
    functions_.vkCmdPipelineBarrier(upload_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0,
                                    nullptr);
    auto flushed = FlushUploadsLocked();
    if (!flushed) {
        KeepUntilDestruction(Move(buffer));
        return Result<void>::Failure(flushed.Error());
    }

    Result<void> outcome = WaitFenceOwner(flushed.Value());
    if (outcome) {
        {
            platform::LockGuard memoryLock(memoryGate_);
            memory_.Invalidate(buffer.memory);
        }

        platform::MemoryCopy(out.Data(), buffer.memory.mapped, static_cast<usize>(bytes));
    }

    RetireEntry(Move(buffer));
    return outcome;
}

} // namespace rocks::graphics::vulkan
