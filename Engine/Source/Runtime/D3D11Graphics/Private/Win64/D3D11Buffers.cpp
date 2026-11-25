#include "D3D11DeviceImpl.h"

#include <Platform/Memory.h>

// 장치 능력 조회와 버퍼 view(RenderBase 4.4절 GX1·GX2)다.
namespace rocks::graphics::d3d11 {

const DeviceCaps& D3D11Device::Caps() const noexcept {
    return caps_;
}

uint32 D3D11Device::GetPixelFormatSampleCounts(PixelFormat format) const noexcept {
    const uint32 number = PixelFormatNumber(format);
    return number <= MaxPixelFormatNumber ? sampleCounts_[number] : 0;
}

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation, free-threaded 장치 호출).
Result<BufferViewHandle> D3D11Device::CreateBufferView(BufferHandle handle,
                                                       const BufferViewDesc& desc) noexcept {
    platform::LockGuard lock(gate_);
    const BufferRecord* buffer = buffers_.Find(handle.slot);
    if (buffer == nullptr) {
        return Result<BufferViewHandle>::Failure(ErrorCode::StaleHandle);
    }

    auto resolved = ResolveBufferView(buffer->desc, desc);
    if (!resolved) {
        auto assigned = lastError_.TryAssign(
            StringView("buffer view breaks the view rules (kind, access, format or range)"));
        (void)assigned;
        return Result<BufferViewHandle>::Failure(resolved.Error());
    }

    const ResolvedBufferView range = resolved.Value();
    auto limited = CheckBufferViewLimits(caps_.limits, FormatCaps(desc.format), desc, range);
    if (!limited) {
        auto assigned = lastError_.TryAssign(StringView(
            "buffer view needs a format capability, alignment or range the D3D11 device does "
            "not provide"));
        (void)assigned;
        return Result<BufferViewHandle>::Failure(limited.Error());
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<BufferViewHandle>::Failure(reserved.Error());
    }

    const FormatMapping mapping = MappingOf(desc.format);
    const UINT first = static_cast<UINT>(range.offset / range.elementBytes);
    const UINT count = static_cast<UINT>(range.elementCount);
    BufferViewRecord record;
    record.format = desc.format;
    record.kind = desc.kind;
    record.access = desc.access;
    record.stride = desc.kind == BufferViewKind::Structured ? range.elementBytes : 0u;
    record.shaderType = desc.kind == BufferViewKind::Typed
                            ? GetPixelFormatInfo(desc.format).shaderType
                            : PixelShaderType::None;
    HRESULT result = S_OK;
    if (desc.access == ViewAccess::Read) {
        // raw view는 BUFFEREX 차원만 RAW 플래그를 받는다.
        D3D11_SHADER_RESOURCE_VIEW_DESC view = {};
        view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        view.BufferEx.FirstElement = first;
        view.BufferEx.NumElements = count;
        if (desc.kind == BufferViewKind::Typed) {
            view.Format = mapping.srv;
        } else if (desc.kind == BufferViewKind::Raw) {
            view.Format = DXGI_FORMAT_R32_TYPELESS;
            view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        }

        result = device_->CreateShaderResourceView(buffer->buffer.Get(), &view, record.srv.Out());
    } else {
        D3D11_UNORDERED_ACCESS_VIEW_DESC view = {};
        view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        view.Buffer.FirstElement = first;
        view.Buffer.NumElements = count;
        if (desc.kind == BufferViewKind::Typed) {
            view.Format = mapping.uav;
        } else if (desc.kind == BufferViewKind::Raw) {
            view.Format = DXGI_FORMAT_R32_TYPELESS;
            view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        }

        result = device_->CreateUnorderedAccessView(buffer->buffer.Get(), &view, record.uav.Out());
    }

    if (FAILED(result)) {
        return FailWith<BufferViewHandle>("CreateShaderResourceView/UnorderedAccessView (buffer)",
                                          result);
    }

    auto added = bufferViews_.TryAdd(Move(record));
    if (!added) {
        return Result<BufferViewHandle>::Failure(added.Error());
    }

    return Result<BufferViewHandle>::Success(BufferViewHandle{added.Value()});
}

// 버퍼 구간을 staging 버퍼로 복사해 읽는다(시험·캡처용). 쌓인 업로드 뒤에 복사한다.
Result<void> D3D11Device::ReadBuffer(BufferHandle handle, uint64 offset, Span<uint8> out) noexcept {
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

    D3D11_BUFFER_DESC stagingDesc = {};
    stagingDesc.ByteWidth = static_cast<UINT>(out.Size());
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComRef<ID3D11Buffer> staging;
    const HRESULT created = device_->CreateBuffer(&stagingDesc, nullptr, staging.Out());
    if (FAILED(created)) {
        return Fail("CreateBuffer (readback staging)", created);
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return reserved;
    }

    UploadOp op;
    op.kind = UploadOp::Kind::CopyRegion;
    op.destination = staging.Get();
    op.copySource = record->buffer.Get();
    op.hasBox = true;
    op.box.left = static_cast<UINT>(offset);
    op.box.right = static_cast<UINT>(offset + out.Size());
    op.box.top = 0;
    op.box.bottom = 1;
    op.box.front = 0;
    op.box.back = 1;
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
            outcome = Fail("ID3D11DeviceContext::Map (buffer readback)", result);
        } else {
            platform::MemoryCopy(out.Data(), mapped.pData, out.Size());
            immediate_->Unmap(staging.Get(), 0);
        }
    }

    Defer(Move(staging));
    return outcome;
}

void D3D11Device::Retire(BufferViewHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    BufferViewRecord* record = bufferViews_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }

    Defer(Move(record->srv), Move(record->uav));
    auto removed = bufferViews_.TryRemove(handle.slot);
    (void)removed;
}

bool D3D11Device::BufferViewsBound() const noexcept {
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_);
    if (pipeline == nullptr) {
        return false;
    }

    BoundBufferViewInfo bound[MaxDrawBufferViews] = {};
    for (uint32 slot = 0; slot < MaxDrawBufferViews; ++slot) {
        const BufferViewRecord* view = bufferViews_.Find(boundViews_[slot].slot);
        if (view != nullptr) {
            bound[slot] = {true, view->kind, view->shaderType, view->stride};
        }
    }

    return DrawBufferSlotsSatisfied(pipeline->bufferSlots, bound);
}

void D3D11Device::SetBufferView(uint32 slot, BufferViewHandle handle) noexcept {
    platform::LockGuard lock(gate_);
    if (slot >= MaxDrawBufferViews) {
        return;
    }

    // 그래픽 단계에는 UAV가 없으므로 ReadWrite view와 stale handle은 slot을 비운다. 실제 묶기는
    // 그리기·dispatch 직전에 한다(PrepareDraw·PrepareDispatch, dispatch의 UAV와 겹치지 않게).
    const BufferViewRecord* view = bufferViews_.Find(handle.slot);
    const bool readable = view != nullptr && view->access == ViewAccess::Read;
    boundViews_[slot] = readable ? handle : BufferViewHandle{};
}

} // namespace rocks::graphics::d3d11
