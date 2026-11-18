#include "D3D12DeviceImpl.h"

namespace rocks::graphics::d3d12 {

// 버퍼 구간을 동기로 읽는다(시험·캡처용). 업로드 기록 뒤에 복사를 붙여 앞선 갱신 뒤의 내용을
// 본다. 버퍼는 COMMON에서 복사 원본으로 암묵 승격된다.
Result<void> D3D12Device::ReadBuffer(BufferHandle handle, uint64 offset, Span<uint8> out) noexcept {
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

    ReadbackRecord readback;
    readback.rowBytes = out.Size();
    readback.rows = 1;
    readback.total = out.Size();
    readback.footprint.Footprint.RowPitch = static_cast<UINT>(out.Size());
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC buffer = BufferResource(out.Size());
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(readback.resource.Out())))) {
        return Fail("readback buffer creation failed");
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return reserved;
    }

    // 버퍼는 암묵 승격으로 상태가 바뀐다. 같은 업로드 기록에서 이미 복사 대상으로 승격됐으면
    // 복사 원본으로 다시 승격할 수 없으므로 쌓인 업로드를 먼저 제출한다(제출 끝에 COMMON으로
    // 돌아온다).
    auto pending = FlushUploadsLocked();
    if (!pending) {
        return Result<void>::Failure(pending.Error());
    }

    auto opened = OpenUploadList();
    if (!opened) {
        return opened;
    }

    uploadList_->CopyBufferRegion(readback.resource.Get(), 0, record->resource.Get(), offset,
                                  out.Size());
    auto flushed = FlushUploadsLocked();
    if (!flushed) {
        KeepUntilDestruction(Move(readback.resource));
        return Result<void>::Failure(flushed.Error());
    }

    readback.fenceValue = flushed.Value();
    auto added = readbacks_.TryAdd(Move(readback));
    if (!added) {
        // 제출한 복사가 아직 쓰는 자원이다. 완료를 기다리지 않고 놓지 않도록 소멸 때까지 둔다.
        KeepUntilDestruction(Move(readback.resource));
        return Result<void>::Failure(added.Error());
    }

    const ReadbackHandle readbackHandle{added.Value()};
    auto waited = WaitFenceOwner(flushed.Value());
    Result<void> outcome = Result<void>::Success();
    if (!waited) {
        outcome = Move(waited);
    } else {
        auto copied = TryReadbackLocked(readbackHandle, out);
        if (!copied || !copied.Value()) {
            outcome = Fail("buffer readback did not complete");
        }
    }

    ReleaseReadbackLocked(readbackHandle);
    return outcome;
}

// 텍스처 mip·plane 하나를 동기로 읽는다(시험·캡처용). ReadPixels와 같은 경로다.
Result<void> D3D12Device::ReadTexture(TextureHandle handle, uint32 mipLevel, uint32 plane,
                                      Span<uint8> out) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return owner;
    }
    platform::LockGuard lock(gate_);
    const TextureRecord* record = textures_.Find(handle.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    if (recording_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (record->view || mipLevel >= record->mipLevels ||
        out.Size() != PixelFormatPlaneImageBytes(record->format, plane,
                                                 MipExtent(record->width, mipLevel),
                                                 MipExtent(record->height, mipLevel))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    // 초기 데이터·갱신 업로드가 먼저 실행되도록 같은 업로드 기록 뒤에 복사를 붙인다.
    auto requested = RequestReadbackLocked(handle, mipLevel, plane, false);
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
            outcome = Fail("readback did not complete");
        }
    }
    ReleaseReadbackLocked(readback);
    return outcome;
}

Result<void> D3D12Device::ReadPixels(TextureHandle handle, Span<uint8> out) noexcept {
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
            outcome = Fail("readback did not complete");
        }
    }
    ReleaseReadbackLocked(readback);
    return outcome;
}

Result<void> D3D12Device::UpdateBuffer(BufferHandle handle, uint64 offset,
                                       Span<const uint8> data) noexcept {
    platform::LockGuard lock(gate_);
    const BufferRecord* record = buffers_.Find(handle.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    if (data.Size() == 0 || offset > record->size || data.Size() > record->size - offset) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    return RecordBufferUpload(record->resource.Get(), offset, data);
}

Result<void> D3D12Device::UpdateTexture(TextureHandle handle, uint32 mipLevel,
                                        Span<const uint8> pixels) noexcept {
    platform::LockGuard lock(gate_);
    TextureRecord* record = textures_.Find(handle.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    if (record->view || record->renderTarget || record->depthTarget ||
        mipLevel >= record->mipLevels ||
        pixels.Size() != PixelFormatImageBytes(record->format, MipExtent(record->width, mipLevel),
                                               MipExtent(record->height, mipLevel))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    return RecordTextureUpload(*record, mipLevel, 1, pixels, true);
}

Result<GpuFencePoint> D3D12Device::SubmitUploads() noexcept {
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

UploadStats D3D12Device::UploadStatistics() const noexcept {
    platform::LockGuard lock(gate_);
    UploadStats stats;
    stats.ringCapacity = ring_.Capacity();
    stats.ringUsed = ring_.UsedBytes();
    stats.ringWraps = ring_.Wraps();
    stats.dedicatedUploads = dedicatedUploads_;
    stats.pending = uploadOpen_;
    return stats;
}

Result<StagingBuffer> D3D12Device::AcquireStaging(uint64 bytes) noexcept {
    return staging_->Acquire(bytes);
}

Result<void> D3D12Device::UploadTextureFromStaging(TextureHandle handle, uint32 mipLevel,
                                                   StagingHandle staging, uint64 offset) noexcept {
    platform::LockGuard lock(gate_);
    TextureRecord* record = textures_.Find(handle.slot);
    auto memory = staging_->Find(staging);
    if (record == nullptr || !memory) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    const uint32 width = MipExtent(record->width, mipLevel);
    const uint32 height = MipExtent(record->height, mipLevel);
    const uint64 rowBytes = PixelFormatRowBytes(record->format, width);
    const uint64 bytes = PixelFormatImageBytes(record->format, width, height);
    if (record->view || record->renderTarget || record->depthTarget ||
        mipLevel >= record->mipLevels || bytes == 0 || offset > memory.Value().size ||
        bytes > memory.Value().size - offset) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    // 다중 plane은 plane마다 footprint가 달라 ring에 옮겨 올린다.
    if (PixelFormatPlaneCount(record->format) != 1) {
        return RecordTextureUpload(
            *record, mipLevel, 1,
            Span<const uint8>(memory.Value().data + offset, static_cast<usize>(bytes)), true);
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 total = 0;
    const D3D12_RESOURCE_DESC resourceDesc = record->resource->GetDesc();
    device_->GetCopyableFootprints(&resourceDesc, mipLevel, 1, 0, &footprint, nullptr, nullptr,
                                   &total);
    // 행 간격이 장치 정렬과 같고 시작이 배치 정렬이면 staging에서 바로 복사한다. 아니면 ring에
    // 장치 정렬로 다시 옮긴다(RenderBase 6.5절: 장치 정렬은 렌더 업로드가 맞춘다).
    if (footprint.Footprint.RowPitch != rowBytes || offset % TexturePlacementAlignment != 0) {
        return RecordTextureUpload(
            *record, mipLevel, 1,
            Span<const uint8>(memory.Value().data + offset, static_cast<usize>(bytes)), true);
    }
    auto opened = OpenUploadList();
    if (!opened) {
        return opened;
    }
    auto marked = staging_->MarkPending(staging);
    if (!marked) {
        return marked;
    }
    footprint.Offset = offset;
    CopyTextureFrom(*record, mipLevel, mipLevel,
                    static_cast<ID3D12Resource*>(memory.Value().resource), footprint, true);
    return Result<void>::Success();
}

Result<void> D3D12Device::UploadBufferFromStaging(BufferHandle handle, uint64 destinationOffset,
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
    auto opened = OpenUploadList();
    if (!opened) {
        return opened;
    }
    auto marked = staging_->MarkPending(staging);
    if (!marked) {
        return marked;
    }
    uploadList_->CopyBufferRegion(record->resource.Get(), destinationOffset,
                                  static_cast<ID3D12Resource*>(memory.Value().resource),
                                  sourceOffset, size);
    return Result<void>::Success();
}

void D3D12Device::ReleaseStaging(StagingHandle staging) noexcept {
    staging_->Release(staging);
}

StagingStats D3D12Device::StagingStatistics() const noexcept {
    return staging_->Stats();
}

Result<ReadbackRequest> D3D12Device::RequestReadback(TextureHandle target) noexcept {
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

Result<bool> D3D12Device::TryReadback(ReadbackHandle handle, Span<uint8> out) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<bool>::Failure(owner.Error());
    }
    platform::LockGuard lock(gate_);
    return TryReadbackLocked(handle, out);
}

void D3D12Device::ReleaseReadback(ReadbackHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }
    platform::LockGuard lock(gate_);
    ReleaseReadbackLocked(handle);
}

// 업로드 기록 list를 연다(잠금 안). 이미 열려 있으면 그대로 쓴다.
Result<void> D3D12Device::OpenUploadList() noexcept {
    if (uploadOpen_) {
        return Result<void>::Success();
    }
    auto picked = PickCommandAllocator();
    if (!picked) {
        return Result<void>::Failure(picked.Error());
    }
    AllocatorRecord& record = allocators_[picked.Value()];
    if (FAILED(record.allocator->Reset()) ||
        FAILED(uploadList_->Reset(record.allocator.Get(), nullptr))) {
        return Fail("upload command list reset failed");
    }
    record.inUse = true;
    uploadAllocator_ = picked.Value();
    uploadOpen_ = true;
    return Result<void>::Success();
}

// size byte 업로드 공간을 잡는다(잠금 안). ring에 자리가 없으면 소유 스레드는 쌓인 업로드를
// 제출하고 가장 오래된 묶음의 fence를 기다려 비운다. 다른 스레드이거나 그래도 없으면 전용
// staging을 만들고 그 업로드 묶음의 fence 뒤에 놓는다.
Result<D3D12Device::UploadSpace> D3D12Device::StageUpload(uint64 size, uint64 alignment) noexcept {
    for (;;) {
        ring_.Reclaim(fence_->GetCompletedValue());
        auto offset = ring_.Allocate(size, alignment);
        if (offset) {
            return Result<UploadSpace>::Success(
                UploadSpace{ringBuffer_.Get(), offset.Value(), ringData_ + offset.Value()});
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

    ComRef<ID3D12Resource> dedicated;
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC buffer = BufferResource(size);
    void* mapped = nullptr;
    const D3D12_RANGE noRead = {0, 0};
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(dedicated.Out()))) ||
        FAILED(dedicated->Map(0, &noRead, &mapped))) {
        return FailWith<UploadSpace>("upload staging creation failed");
    }
    ID3D12Resource* resource = dedicated.Get();
    Retired entry;
    entry.object.Attach(dedicated.Detach());
    entry.uploadPending = true;
    auto pushed = retired_.TryPushBack(Move(entry));
    if (!pushed) {
        return Result<UploadSpace>::Failure(pushed.Error());
    }
    ++dedicatedUploads_;
    return Result<UploadSpace>::Success(UploadSpace{resource, 0, static_cast<uint8*>(mapped)});
}

// 버퍼 구간 복사를 업로드 기록에 쌓는다(잠금 안).
Result<void> D3D12Device::RecordBufferUpload(ID3D12Resource* target, uint64 offset,
                                             Span<const uint8> data) noexcept {
    auto space = StageUpload(data.Size(), BufferUploadAlignment);
    if (!space) {
        return Result<void>::Failure(space.Error());
    }
    platform::MemoryCopy(space.Value().data, data.Data(), data.Size());
    auto opened = OpenUploadList();
    if (!opened) {
        return opened;
    }
    uploadList_->CopyBufferRegion(target, offset, space.Value().resource, space.Value().offset,
                                  data.Size());
    return Result<void>::Success();
}

// footprint 배치의 원본에서 mip 하나의 plane 하나(subresource)로 복사를 기록한다. restore면
// 제출한 상태(committed)에서 복사 대상으로 바꿨다가 되돌린다(업로드 기록은 프레임 기록보다 먼저
// 실행되므로 상태 중립이어야 한다). 처음 생성(COPY_DEST)이면 복사 뒤 셰이더 읽기 상태로 바꾸고
// 그것을 추적 상태로 삼는다. 상태는 mip 단위로 추적하고 그 mip의 plane들이 함께 따른다.
void D3D12Device::CopyTextureFrom(TextureRecord& record, uint32 mipLevel, uint32 subresource,
                                  ID3D12Resource* source,
                                  const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                                  bool restore) noexcept {
    const D3D12_RESOURCE_STATES resting = restore ? record.committed[mipLevel] : ShaderReadStates;
    if (restore && resting != D3D12_RESOURCE_STATE_COPY_DEST) {
        const D3D12_RESOURCE_BARRIER toCopy = TransitionSubresource(
            record.resource.Get(), subresource, resting, D3D12_RESOURCE_STATE_COPY_DEST);
        uploadList_->ResourceBarrier(1, &toCopy);
    }
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = record.resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = subresource;
    D3D12_TEXTURE_COPY_LOCATION from = {};
    from.pResource = source;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = footprint;
    uploadList_->CopyTextureRegion(&destination, 0, 0, 0, &from, nullptr);
    if (resting != D3D12_RESOURCE_STATE_COPY_DEST) {
        const D3D12_RESOURCE_BARRIER back = TransitionSubresource(
            record.resource.Get(), subresource, D3D12_RESOURCE_STATE_COPY_DEST, resting);
        uploadList_->ResourceBarrier(1, &back);
    }
    if (!restore) {
        record.committed[mipLevel] = resting;
        record.current[mipLevel] = resting;
    }
}

// 빈틈 없는 행의 픽셀(mip firstMip부터 count개, mip 안은 plane 순서)을 장치 행 정렬로 ring에
// 옮기고 복사를 기록한다. 블록 압축 형식의 행은 블록 행이다.
Result<void> D3D12Device::RecordTextureUpload(TextureRecord& record, uint32 firstMip, uint32 count,
                                              Span<const uint8> pixels, bool restore) noexcept {
    constexpr uint32 MaxPlanes = 2;
    const uint32 planes = PixelFormatPlaneCount(record.format);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[MaxMipLevels * MaxPlanes] = {};
    uint32 subresources[MaxMipLevels * MaxPlanes] = {};
    uint32 levels[MaxMipLevels * MaxPlanes] = {};
    uint64 offsets[MaxMipLevels * MaxPlanes] = {};
    const D3D12_RESOURCE_DESC resourceDesc = record.resource->GetDesc();
    // subresource마다 배치 정렬된 위치에 이어 놓는다.
    uint64 total = 0;
    uint32 copies = 0;
    for (uint32 index = 0; index < count; ++index) {
        for (uint32 plane = 0; plane < planes; ++plane) {
            const uint32 subresource =
                d3d12::SubresourceIndex(firstMip + index, plane, record.mipLevels);
            UINT64 bytes = 0;
            device_->GetCopyableFootprints(&resourceDesc, subresource, 1, 0, &footprints[copies],
                                           nullptr, nullptr, &bytes);
            total = (total + TexturePlacementAlignment - 1) / TexturePlacementAlignment *
                    TexturePlacementAlignment;
            offsets[copies] = total;
            subresources[copies] = subresource;
            levels[copies] = firstMip + index;
            total += bytes;
            ++copies;
        }
    }
    auto space = StageUpload(total, TexturePlacementAlignment);
    if (!space) {
        return Result<void>::Failure(space.Error());
    }
    usize sourceOffset = 0;
    uint32 copy = 0;
    for (uint32 index = 0; index < count; ++index) {
        const uint32 level = firstMip + index;
        const uint32 width = MipExtent(record.width, level);
        const uint32 height = MipExtent(record.height, level);
        for (uint32 plane = 0; plane < planes; ++plane, ++copy) {
            const usize rowBytes =
                static_cast<usize>(PixelFormatPlaneRowBytes(record.format, plane, width));
            const uint32 rows = PixelFormatPlaneRows(record.format, plane, height);
            uint8* destination = space.Value().data + offsets[copy];
            for (uint32 row = 0; row < rows; ++row) {
                platform::MemoryCopy(destination + static_cast<usize>(row) *
                                                       footprints[copy].Footprint.RowPitch,
                                     pixels.Data() + sourceOffset + row * rowBytes, rowBytes);
            }
            sourceOffset += rowBytes * rows;
            footprints[copy].Offset = space.Value().offset + offsets[copy];
        }
    }
    auto opened = OpenUploadList();
    if (!opened) {
        return opened;
    }
    for (uint32 index = 0; index < copies; ++index) {
        CopyTextureFrom(record, levels[index], subresources[index], space.Value().resource,
                        footprints[index], restore);
    }
    return Result<void>::Success();
}

// 쌓인 업로드 기록을 제출하고 그 fence 값을 돌려준다(잠금 안, 소유 스레드). 없으면 0이다.
Result<uint64> D3D12Device::FlushUploadsLocked() noexcept {
    if (!uploadOpen_) {
        return Result<uint64>::Success(0);
    }
    uploadOpen_ = false;
    AllocatorRecord& record = allocators_[uploadAllocator_];
    record.inUse = false;
    if (FAILED(uploadList_->Close())) {
        // 아무것도 제출하지 않았다. 기록이 쓰던 공간은 지금까지의 제출 뒤에 다시 쓴다.
        record.fenceValue = fenceValue_;
        ring_.CloseBatch(fenceValue_);
        staging_->AssignPending(fenceValue_);
        AssignPendingUploads(fenceValue_);
        return FailWith<uint64>("upload command list close failed");
    }
    ID3D12CommandList* lists[] = {uploadList_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    auto signaled = SignalQueue();
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

void D3D12Device::AssignPendingUploads(uint64 value) noexcept {
    for (Retired& entry : retired_) {
        if (entry.uploadPending) {
            entry.uploadPending = false;
            entry.fenceValue = value;
        }
    }
}

// 텍스처 mip 하나의 plane 하나를 readback 버퍼로 복사하는 기록을 쌓고 바로 제출한다(잠금 안).
// requireRenderTarget이면 RenderTarget 텍스처만 받는다(ReadPixels·RequestReadback 계약).
Result<ReadbackRequest> D3D12Device::RequestReadbackLocked(TextureHandle target, uint32 mipLevel,
                                                           uint32 plane,
                                                           bool requireRenderTarget) noexcept {
    const TextureRecord* record = textures_.Find(target.slot);
    if (record == nullptr) {
        return Result<ReadbackRequest>::Failure(ErrorCode::StaleHandle);
    }
    if ((requireRenderTarget && !record->renderTarget) || record->view ||
        mipLevel >= record->mipLevels || plane >= PixelFormatPlaneCount(record->format)) {
        return Result<ReadbackRequest>::Failure(ErrorCode::InvalidArgument);
    }
    ReadbackRecord readback;
    const uint32 width = MipExtent(record->width, mipLevel);
    const uint32 height = MipExtent(record->height, mipLevel);
    readback.rowBytes = PixelFormatPlaneRowBytes(record->format, plane, width);
    readback.rows = PixelFormatPlaneRows(record->format, plane, height);
    const uint32 subresource = d3d12::SubresourceIndex(mipLevel, plane, record->mipLevels);
    const D3D12_RESOURCE_DESC resourceDesc = record->resource->GetDesc();
    UINT64 total = 0;
    device_->GetCopyableFootprints(&resourceDesc, subresource, 1, 0, &readback.footprint, nullptr,
                                   nullptr, &total);
    readback.total = total;
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC buffer = BufferResource(total);
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(readback.resource.Out())))) {
        return FailWith<ReadbackRequest>("readback buffer creation failed");
    }
    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<ReadbackRequest>::Failure(reserved.Error());
    }
    auto opened = OpenUploadList();
    if (!opened) {
        return Result<ReadbackRequest>::Failure(opened.Error());
    }
    // 업로드 기록은 상태 중립이다: 제출한 상태에서 복사 원본으로 바꿨다가 되돌린다.
    // 이미 복사 원본 상태면(그래프가 CopySource로 끝냈다) barrier를 내지 않는다.
    const D3D12_RESOURCE_STATES resting = record->committed[mipLevel];
    const bool move = resting != D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (move) {
        const D3D12_RESOURCE_BARRIER toCopy = TransitionSubresource(
            record->resource.Get(), subresource, resting, D3D12_RESOURCE_STATE_COPY_SOURCE);
        uploadList_->ResourceBarrier(1, &toCopy);
    }
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = readback.resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = readback.footprint;
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = record->resource.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = subresource;
    uploadList_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    if (move) {
        const D3D12_RESOURCE_BARRIER back = TransitionSubresource(
            record->resource.Get(), subresource, D3D12_RESOURCE_STATE_COPY_SOURCE, resting);
        uploadList_->ResourceBarrier(1, &back);
    }
    auto flushed = FlushUploadsLocked();
    if (!flushed) {
        KeepUntilDestruction(Move(readback.resource));
        return Result<ReadbackRequest>::Failure(flushed.Error());
    }
    readback.fenceValue = flushed.Value();
    const uint64 bytes = readback.rowBytes * readback.rows;
    auto added = readbacks_.TryAdd(Move(readback));
    if (!added) {
        // 제출한 복사가 아직 쓰는 자원이다. 완료를 기다리지 않고 놓지 않도록 소멸 때까지 둔다.
        KeepUntilDestruction(Move(readback.resource));
        return Result<ReadbackRequest>::Failure(added.Error());
    }
    ReadbackRequest request;
    request.handle = ReadbackHandle{added.Value()};
    request.completion = timeline_.Point(flushed.Value());
    request.bytes = bytes;
    return Result<ReadbackRequest>::Success(request);
}

Result<bool> D3D12Device::TryReadbackLocked(ReadbackHandle handle, Span<uint8> out) noexcept {
    ReadbackRecord* record = readbacks_.Find(handle.slot);
    if (record == nullptr) {
        return Result<bool>::Failure(ErrorCode::StaleHandle);
    }
    const usize rowBytes = static_cast<usize>(record->rowBytes);
    if (out.Size() != rowBytes * record->rows) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }
    const uint64 completed = fence_->GetCompletedValue();
    if (completed == D3D12FenceSource::RemovedFenceValue) {
        timeline_.MarkLost();
        return Result<bool>::Failure(ErrorCode::Destroyed);
    }
    if (completed < record->fenceValue) {
        return Result<bool>::Success(false);
    }
    void* mapped = nullptr;
    const D3D12_RANGE whole = {0, static_cast<SIZE_T>(record->total)};
    if (FAILED(record->resource->Map(0, &whole, &mapped))) {
        return FailWith<bool>("readback map failed");
    }
    const uint8* rows = static_cast<const uint8*>(mapped) + record->footprint.Offset;
    for (uint32 row = 0; row < record->rows; ++row) {
        platform::MemoryCopy(out.Data() + row * rowBytes,
                             rows + static_cast<usize>(row) * record->footprint.Footprint.RowPitch,
                             rowBytes);
    }
    const D3D12_RANGE noWrite = {0, 0};
    record->resource->Unmap(0, &noWrite);
    return Result<bool>::Success(true);
}

void D3D12Device::ReleaseReadbackLocked(ReadbackHandle handle) noexcept {
    ReadbackRecord* record = readbacks_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }
    Defer(Move(record->resource), -1, -1);
    auto removed = readbacks_.TryRemove(handle.slot);
    (void)removed;
}

} // namespace rocks::graphics::d3d12
