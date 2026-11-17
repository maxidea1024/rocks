#include "D3D11DeviceImpl.h"

#include <Platform/Memory.h>

namespace rocks::graphics::d3d11 {

Result<void> D3D11Device::UpdateBuffer(BufferHandle handle, uint64 offset,
                                       Span<const uint8> data) noexcept {
    platform::LockGuard lock(gate_);
    const BufferRecord* record = buffers_.Find(handle.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (data.Size() == 0 || offset > record->size || data.Size() > record->size - offset) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return RecordBufferUpload(record->buffer.Get(), offset, data);
}

Result<void> D3D11Device::UpdateTexture(TextureHandle handle, uint32 mipLevel,
                                        Span<const uint8> pixels) noexcept {
    platform::LockGuard lock(gate_);
    const TextureRecord* record = textures_.Find(handle.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (record->view || record->renderTarget || record->depthTarget ||
        mipLevel >= record->mipLevels ||
        pixels.Size() != PixelFormatImageBytes(record->format, MipExtent(record->width, mipLevel),
                                               MipExtent(record->height, mipLevel))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto space = StageUpload(pixels.Size());
    if (!space) {
        return Result<void>::Failure(space.Error());
    }

    platform::MemoryCopy(space.Value().data, pixels.Data(), pixels.Size());
    return RecordAnyUpload(*record, mipLevel, space.Value().data);
}

Result<GpuFencePoint> D3D11Device::SubmitUploads() noexcept {
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

UploadStats D3D11Device::UploadStatistics() const noexcept {
    platform::LockGuard lock(gate_);
    UploadStats stats;
    stats.ringCapacity = ring_.Capacity();
    stats.ringUsed = ring_.UsedBytes();
    stats.ringWraps = ring_.Wraps();
    stats.dedicatedUploads = dedicatedUploads_;
    stats.pending = uploadOps_.Size() != 0;
    return stats;
}

Result<StagingBuffer> D3D11Device::AcquireStaging(uint64 bytes) noexcept {
    return staging_->Acquire(bytes);
}

// staging은 시스템 메모리이고 UpdateSubresource는 원본 정렬을 요구하지 않으므로 언제나 그 자리에서
// 올린다(D3D12처럼 ring으로 다시 옮기는 경로가 없다).
Result<void> D3D11Device::UploadTextureFromStaging(TextureHandle handle, uint32 mipLevel,
                                                   StagingHandle staging, uint64 offset) noexcept {
    platform::LockGuard lock(gate_);
    const TextureRecord* record = textures_.Find(handle.slot);
    auto memory = staging_->Find(staging);
    if (record == nullptr || !memory) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    const uint64 bytes = PixelFormatImageBytes(record->format, MipExtent(record->width, mipLevel),
                                               MipExtent(record->height, mipLevel));
    if (record->view || record->renderTarget || record->depthTarget ||
        mipLevel >= record->mipLevels || bytes == 0 || offset > memory.Value().size ||
        bytes > memory.Value().size - offset) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto marked = staging_->MarkPending(staging);
    if (!marked) {
        return marked;
    }

    return RecordAnyUpload(*record, mipLevel, memory.Value().data + offset);
}

Result<void> D3D11Device::UploadBufferFromStaging(BufferHandle handle, uint64 destinationOffset,
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

    auto marked = staging_->MarkPending(staging);
    if (!marked) {
        return marked;
    }

    UploadOp op;
    op.destination = record->buffer.Get();
    op.hasBox = true;
    op.box = {static_cast<UINT>(destinationOffset),        0, 0,
              static_cast<UINT>(destinationOffset + size), 1, 1};
    op.source = memory.Value().data + sourceOffset;
    return PushUpload(op);
}

void D3D11Device::ReleaseStaging(StagingHandle staging) noexcept {
    staging_->Release(staging);
}

StagingStats D3D11Device::StagingStatistics() const noexcept {
    return staging_->Stats();
}

// size byte 업로드 원본 공간을 잡는다(잠금 안). ring에 자리가 없으면 소유 스레드는 쌓인 업로드를
// 제출하고 가장 오래된 묶음의 fence를 기다려 비운다. 다른 스레드이거나 그래도 없으면 전용 메모리를
// 잡고 그 업로드 묶음의 fence 뒤에 놓는다.
Result<D3D11Device::UploadSpace> D3D11Device::StageUpload(uint64 size) noexcept {
    for (;;) {
        ring_.Reclaim(fence_->GetCompletedValue());
        auto offset = ring_.Allocate(size, UploadAlignment);
        if (offset) {
            return Result<UploadSpace>::Success(UploadSpace{ringData_ + offset.Value()});
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

    auto reserved = dedicated_.TryEnsureCapacity(dedicated_.Size() + 1);
    if (!reserved) {
        return Result<UploadSpace>::Failure(reserved.Error());
    }

    auto memory = allocator_.Allocate(static_cast<usize>(size), UploadAlignment);
    if (!memory) {
        return Result<UploadSpace>::Failure(memory.Error());
    }

    DedicatedUpload upload;
    upload.memory = memory.Value();
    auto pushed = dedicated_.TryPushBack(upload);
    (void)pushed;
    ++dedicatedUploads_;
    return Result<UploadSpace>::Success(UploadSpace{static_cast<uint8*>(memory.Value())});
}

Result<void> D3D11Device::PushUpload(const UploadOp& op) noexcept {
    auto pushed = uploadOps_.TryPushBack(op);
    if (!pushed) {
        return Result<void>::Failure(pushed.Error());
    }

    return Result<void>::Success();
}

// 버퍼 구간 갱신을 업로드 기록에 쌓는다(잠금 안).
Result<void> D3D11Device::RecordBufferUpload(ID3D11Buffer* target, uint64 offset,
                                             Span<const uint8> data) noexcept {
    auto space = StageUpload(data.Size());
    if (!space) {
        return Result<void>::Failure(space.Error());
    }

    platform::MemoryCopy(space.Value().data, data.Data(), data.Size());
    UploadOp op;
    op.destination = target;
    op.hasBox = true;
    op.box = {static_cast<UINT>(offset), 0, 0, static_cast<UINT>(offset + data.Size()), 1, 1};
    op.source = space.Value().data;
    return PushUpload(op);
}

// 빈틈 없는 행의 mip 하나 갱신을 업로드 기록에 쌓는다(잠금 안). pixels는 제출 때까지 살아 있는
// ring·staging 메모리다.
Result<void> D3D11Device::RecordTextureUpload(const TextureRecord& record, uint32 mipLevel,
                                              const uint8* pixels) noexcept {
    const uint32 rowBytes =
        static_cast<uint32>(PixelFormatRowBytes(record.format, MipExtent(record.width, mipLevel)));
    UploadOp op;
    op.destination = record.texture.Get();
    op.subresource = mipLevel;
    op.source = pixels;
    op.rowPitch = rowBytes;
    op.depthPitch =
        rowBytes * PixelFormatBlockRows(record.format, MipExtent(record.height, mipLevel));
    return PushUpload(op);
}

Result<void> D3D11Device::RecordAnyUpload(const TextureRecord& record, uint32 mipLevel,
                                          const uint8* pixels) noexcept {
    const PixelFormatKind kind = GetPixelFormatInfo(record.format).kind;
    if (kind == PixelFormatKind::Color || kind == PixelFormatKind::Compressed) {
        return RecordTextureUpload(record, mipLevel, pixels);
    }

    return RecordStagedUpload(record, mipLevel, pixels);
}

Result<void> D3D11Device::RecordStagedUpload(const TextureRecord& record, uint32 mipLevel,
                                             const uint8* pixels) noexcept {
    const uint32 width = MipExtent(record.width, mipLevel);
    const uint32 height = MipExtent(record.height, mipLevel);
    const DeviceLayout layout = DeviceLayoutOf(record.format, width, height);
    Array<uint8> converted(allocator_);
    if (!converted.TryResize(static_cast<usize>(layout.rowBytes * layout.rows))) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    ToDeviceLayout(record.format, width, height, pixels, converted.Data());
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = width;
    stagingDesc.Height = height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = MappingOf(record.format).resource;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SUBRESOURCE_DATA initial = {};
    initial.pSysMem = converted.Data();
    initial.SysMemPitch = static_cast<UINT>(layout.rowBytes);
    ComRef<ID3D11Texture2D> staging;
    const HRESULT created = device_->CreateTexture2D(&stagingDesc, &initial, staging.Out());
    if (FAILED(created)) {
        return Fail("CreateTexture2D (upload staging)", created);
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return reserved;
    }

    UploadOp op;
    op.kind = UploadOp::Kind::CopyRegion;
    op.destination = record.texture.Get();
    op.subresource = mipLevel;
    op.copySource = staging.Get();
    op.sourceSubresource = 0;
    auto pushed = PushUpload(op);
    if (!pushed) {
        return pushed;
    }

    // 업로드 기록이 실행될 때까지 회수 대기 항목이 staging을 붙든다(uploadPending).
    Defer(Move(staging));
    return Result<void>::Success();
}

Result<void> D3D11Device::ReadTexture(TextureHandle handle, uint32 mipLevel, uint32 plane,
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

    const uint32 width = MipExtent(record->width, mipLevel);
    const uint32 height = MipExtent(record->height, mipLevel);
    if (record->view || mipLevel >= record->mipLevels ||
        out.Size() != PixelFormatPlaneImageBytes(record->format, plane, width, height)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = width;
    stagingDesc.Height = height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = MappingOf(record->format).resource;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComRef<ID3D11Texture2D> staging;
    const HRESULT created = device_->CreateTexture2D(&stagingDesc, nullptr, staging.Out());
    if (FAILED(created)) {
        return Fail("CreateTexture2D (readback staging)", created);
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return reserved;
    }

    // 쌓인 초기 데이터·갱신 업로드 뒤에 복사를 붙인다.
    UploadOp op;
    op.kind = UploadOp::Kind::CopyRegion;
    op.destination = staging.Get();
    op.subresource = 0;
    op.copySource = record->texture.Get();
    op.sourceSubresource = mipLevel;
    auto pushed = PushUpload(op);
    if (!pushed) {
        return pushed;
    }

    auto flushed = FlushUploadsLocked();
    if (!flushed) {
        ComRef<IUnknown> object;
        object.Attach(staging.Detach());
        KeepUntilDestruction(Move(object));
        return Result<void>::Failure(flushed.Error());
    }

    Result<void> outcome = WaitFenceOwner(flushed.Value());
    if (outcome) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT result = immediate_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(result)) {
            outcome = Fail("ID3D11DeviceContext::Map (texture readback)", result);
        } else {
            FromDeviceLayout(record->format, plane, width, height,
                             static_cast<const uint8*>(mapped.pData), mapped.RowPitch, out.Data());
            immediate_->Unmap(staging.Get(), 0);
        }
    }

    Defer(Move(staging));
    return outcome;
}

// 쌓인 업로드를 immediate context에서 실행하고 신호한 fence 값을 돌려준다(잠금 안, 소유 스레드).
// 없으면 0이다.
Result<uint64> D3D11Device::FlushUploadsLocked() noexcept {
    if (uploadOps_.Size() == 0) {
        return Result<uint64>::Success(0);
    }

    for (const UploadOp& op : uploadOps_) {
        if (op.kind == UploadOp::Kind::Copy) {
            immediate_->CopyResource(op.destination, op.copySource);
        } else if (op.kind == UploadOp::Kind::CopyRegion) {
            immediate_->CopySubresourceRegion(op.destination, op.subresource, 0, 0, 0,
                                              op.copySource, op.sourceSubresource, nullptr);
        } else {
            immediate_->UpdateSubresource(op.destination, op.subresource,
                                          op.hasBox ? &op.box : nullptr, op.source, op.rowPitch,
                                          op.depthPitch);
        }
    }

    uploadOps_.Clear();
    auto signaled = SignalQueue();
    const uint64 value = signaled ? signaled.Value() : RetainUntilDestructionFence;
    ring_.CloseBatch(value);
    staging_->AssignPending(value);
    AssignPendingUploads(value);
    if (!signaled) {
        return Result<uint64>::Failure(signaled.Error());
    }

    return Result<uint64>::Success(value);
}

Result<void> D3D11Device::WaitIdle() noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);
    return WaitIdleLocked();
}

Result<void> D3D11Device::WaitIdleLocked() noexcept {
    // 쌓인 업로드도 제출해 함께 기다린다.
    auto uploads = FlushUploadsLocked();
    if (!uploads) {
        ProcessRetired();
        return Result<void>::Failure(uploads.Error());
    }

    auto signaled = SignalQueue();
    if (!signaled) {
        // 장치가 제거되었으면 fence 완료 값이 최대라 회수 대기 항목을 모두 놓을 수 있다.
        ProcessRetired();
        return Result<void>::Failure(signaled.Error());
    }

    auto waited = WaitFenceOwner(signaled.Value());
    // 기록 중인 프레임의 보류 항목은 아직 제출되지 않았으므로 남긴다.
    ProcessRetired();
    if (!waited) {
        return waited;
    }

    return Result<void>::Success();
}

Result<void> D3D11Device::ReadPixels(TextureHandle handle, Span<uint8> out) noexcept {
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
        if (!copied) {
            outcome = Result<void>::Failure(copied.Error());
        } else if (!copied.Value()) {
            outcome = Fail("readback did not complete");
        }
    }

    ReleaseReadbackLocked(readback);
    return outcome;
}

Result<ReadbackRequest> D3D11Device::RequestReadback(TextureHandle target) noexcept {
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

Result<bool> D3D11Device::TryReadback(ReadbackHandle handle, Span<uint8> out) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<bool>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);
    return TryReadbackLocked(handle, out);
}

void D3D11Device::ReleaseReadback(ReadbackHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    ReleaseReadbackLocked(handle);
}

// RenderTarget 텍스처를 staging 텍스처로 복사하는 기록을 쌓고 바로 제출한다(잠금 안).
Result<ReadbackRequest> D3D11Device::RequestReadbackLocked(TextureHandle target) noexcept {
    const TextureRecord* record = textures_.Find(target.slot);
    if (record == nullptr) {
        return Result<ReadbackRequest>::Failure(ErrorCode::StaleHandle);
    }

    if (!record->renderTarget) {
        return Result<ReadbackRequest>::Failure(ErrorCode::InvalidArgument);
    }

    ReadbackRecord readback;
    readback.width = record->width;
    readback.height = record->height;
    readback.bytesPerPixel = GetPixelFormatInfo(record->format).blockBytes;
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = record->width;
    stagingDesc.Height = record->height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = MappingOf(record->format).resource;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    const HRESULT created = device_->CreateTexture2D(&stagingDesc, nullptr, readback.staging.Out());
    if (FAILED(created)) {
        return FailWith<ReadbackRequest>("CreateTexture2D (readback staging)", created);
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<ReadbackRequest>::Failure(reserved.Error());
    }

    UploadOp op;
    op.kind = UploadOp::Kind::Copy;
    op.destination = readback.staging.Get();
    op.copySource = record->texture.Get();
    auto pushed = PushUpload(op);
    if (!pushed) {
        return Result<ReadbackRequest>::Failure(pushed.Error());
    }

    auto flushed = FlushUploadsLocked();
    if (!flushed) {
        ComRef<IUnknown> object;
        object.Attach(readback.staging.Detach());
        KeepUntilDestruction(Move(object));
        return Result<ReadbackRequest>::Failure(flushed.Error());
    }

    readback.fenceValue = flushed.Value();
    const uint64 bytes =
        static_cast<uint64>(readback.width) * readback.height * readback.bytesPerPixel;
    auto added = readbacks_.TryAdd(Move(readback));
    if (!added) {
        return Result<ReadbackRequest>::Failure(added.Error());
    }

    ReadbackRequest request;
    request.handle = ReadbackHandle{added.Value()};
    request.completion = timeline_.Point(flushed.Value());
    request.bytes = bytes;
    return Result<ReadbackRequest>::Success(request);
}

// fence가 끝났으면 staging을 map해 빈틈 없는 행으로 옮긴다. 끝나지 않았으면 false다(기다리지
// 않는다).
Result<bool> D3D11Device::TryReadbackLocked(ReadbackHandle handle, Span<uint8> out) noexcept {
    ReadbackRecord* record = readbacks_.Find(handle.slot);
    if (record == nullptr) {
        return Result<bool>::Failure(ErrorCode::StaleHandle);
    }

    const usize rowBytes = static_cast<usize>(record->width) * record->bytesPerPixel;
    if (out.Size() != rowBytes * record->height) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 completed = CompletedFence();
    if (completed == RemovedFenceValue) {
        return Result<bool>::Failure(ErrorCode::Destroyed);
    }

    if (completed < record->fenceValue) {
        return Result<bool>::Success(false);
    }

    // fence가 끝났으므로 GPU 복사는 끝났다. DO_NOT_WAIT은 쓰지 않는다: runtime의 자원 사용
    // 추적이 fence보다 늦게 풀려(하드웨어 드라이버에서 확인) 끝난 복사에도 WAS_STILL_DRAWING을
    // 돌려줄 수 있기 때문이다. 이 map은 runtime 장부만 맞추고 GPU를 기다리지 않는다.
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT result = immediate_->Map(record->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        return FailWith<bool>("ID3D11DeviceContext::Map (readback)", result);
    }

    const uint8* rows = static_cast<const uint8*>(mapped.pData);
    for (uint32 row = 0; row < record->height; ++row) {
        platform::MemoryCopy(out.Data() + row * rowBytes,
                             rows + static_cast<usize>(row) * mapped.RowPitch, rowBytes);
    }

    immediate_->Unmap(record->staging.Get(), 0);
    return Result<bool>::Success(true);
}

void D3D11Device::ReleaseReadbackLocked(ReadbackHandle handle) noexcept {
    ReadbackRecord* record = readbacks_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }

    Defer(Move(record->staging));
    auto removed = readbacks_.TryRemove(handle.slot);
    (void)removed;
}

} // namespace rocks::graphics::d3d11
