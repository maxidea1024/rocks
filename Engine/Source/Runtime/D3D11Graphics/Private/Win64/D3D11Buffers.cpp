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

// b0 상수, PS t0 텍스처·s0 샘플러, t1–t4 버퍼 SRV만 받는다(PipelineDesc 고정 배치). 두 단계가 같은
// slot을 다른 종류로 읽거나 배치 밖 자원(UAV 포함)이면 InvalidArgument다.
Result<void> D3D11Device::ReflectBindings(ID3DBlob* code, ShaderStage stage, DrawBufferSlots& slots,
                                          DrawBufferSlots* writeSlots) noexcept {
    ComRef<ID3D11ShaderReflection> reflection;
    HRESULT result =
        D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(reflection.Out()));
    if (FAILED(result)) {
        return Fail("D3DReflect (shader resources)", result);
    }

    D3D11_SHADER_DESC shader = {};
    result = reflection->GetDesc(&shader);
    if (FAILED(result)) {
        return Fail("ID3D11ShaderReflection::GetDesc", result);
    }

    for (UINT index = 0; index < shader.BoundResources; ++index) {
        D3D11_SHADER_INPUT_BIND_DESC resource = {};
        result = reflection->GetResourceBindingDesc(index, &resource);
        if (FAILED(result)) {
            return Fail("ID3D11ShaderReflection::GetResourceBindingDesc", result);
        }

        const bool pixel = stage == ShaderStage::Pixel;
        const bool single = resource.BindCount == 1;
        const uint32 slot = resource.BindPoint - 1;
        const bool viewSlot = resource.BindPoint >= 1 && slot < MaxDrawBufferViews;
        const bool writeSlot = writeSlots != nullptr && resource.BindPoint < MaxDrawBufferViews;
        const PixelShaderType type =
            resource.ReturnType == D3D_RETURN_TYPE_SINT   ? PixelShaderType::Int
            : resource.ReturnType == D3D_RETURN_TYPE_UINT ? PixelShaderType::Uint
                                                          : PixelShaderType::Float;
        bool fits = false;
        switch (resource.Type) {
        case D3D_SIT_CBUFFER:
            fits = single && resource.BindPoint == 0;
            break;
        case D3D_SIT_SAMPLER:
            fits = single && pixel && resource.BindPoint == 0;
            break;
        case D3D_SIT_TEXTURE:
            if (resource.Dimension != D3D_SRV_DIMENSION_BUFFER) {
                fits = single && pixel && resource.BindPoint == 0;
            } else {
                fits = single && viewSlot &&
                       AddDrawBufferSlot(slots, slot, BufferViewKind::Typed, type);
            }

            break;
        case D3D_SIT_STRUCTURED:
        case D3D_SIT_BYTEADDRESS:
            // 구조화 버퍼의 NumSamples는 원소 stride다(D3D11_SHADER_INPUT_BIND_DESC).
            fits =
                single && viewSlot &&
                AddDrawBufferSlot(slots, slot,
                                  resource.Type == D3D_SIT_STRUCTURED ? BufferViewKind::Structured
                                                                      : BufferViewKind::Raw,
                                  PixelShaderType::None,
                                  resource.Type == D3D_SIT_STRUCTURED ? resource.NumSamples : 0u);
            break;
        case D3D_SIT_UAV_RWTYPED:
            fits = single && writeSlot && resource.Dimension == D3D_SRV_DIMENSION_BUFFER &&
                   AddDrawBufferSlot(*writeSlots, resource.BindPoint, BufferViewKind::Typed, type);
            break;
        case D3D_SIT_UAV_RWSTRUCTURED:
        case D3D_SIT_UAV_RWBYTEADDRESS:
            fits = single && writeSlot &&
                   AddDrawBufferSlot(
                       *writeSlots, resource.BindPoint,
                       resource.Type == D3D_SIT_UAV_RWSTRUCTURED ? BufferViewKind::Structured
                                                                 : BufferViewKind::Raw,
                       PixelShaderType::None,
                       resource.Type == D3D_SIT_UAV_RWSTRUCTURED ? resource.NumSamples : 0u);
            break;
        default:
            break;
        }

        if (!fits) {
            Diagnostic message;
            message.Text(pixel ? "pixel" : (stage == ShaderStage::Vertex ? "vertex" : "compute"))
                .Text(" shader resource ")
                .Text(resource.Name != nullptr ? resource.Name : "?")
                .Text(" is outside the fixed layout (b0, pixel t0 and s0, buffer views t1-t4, "
                      "compute buffer UAVs u0-u3; no UAVs in graphics stages)");
            return Reject(message);
        }
    }

    return Result<void>::Success();
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
