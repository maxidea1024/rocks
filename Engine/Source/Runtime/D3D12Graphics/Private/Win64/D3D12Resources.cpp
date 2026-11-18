#include "D3D12DeviceImpl.h"

namespace rocks::graphics::d3d12 {

Result<SwapChainHandle> D3D12Device::CreateSwapChain(const SwapChainDesc& desc) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<SwapChainHandle>::Failure(owner.Error());
    }
    platform::LockGuard lock(gate_);

    if (desc.surface.window == nullptr || desc.width == 0 || desc.height == 0) {
        return Result<SwapChainHandle>::Failure(ErrorCode::InvalidArgument);
    }

    DXGI_SWAP_CHAIN_DESC1 swapDesc = {};
    swapDesc.Width = desc.width;
    swapDesc.Height = desc.height;
    // flip-model 버퍼는 _SRGB 형식을 쓸 수 없다. RTV를 sRGB로 만들어 쓰기 시 인코딩한다.
    swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = SwapChainBufferCount;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const HWND window = static_cast<HWND>(desc.surface.window);
    ComRef<IDXGISwapChain1> swapChain1;
    const HRESULT swapCreated = factory_->CreateSwapChainForHwnd(
        queue_.Get(), window, &swapDesc, nullptr, nullptr, swapChain1.Out());
    if (FAILED(swapCreated)) {
        return FailWith<SwapChainHandle>("CreateSwapChainForHwnd", swapCreated);
    }
    factory_->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

    SwapChainRecord record;
    if (FAILED(swapChain1.TryQueryAs(record.swapChain))) {
        return FailWith<SwapChainHandle>("IDXGISwapChain3 is not available");
    }
    record.width = desc.width;
    record.height = desc.height;
    record.vsync = desc.vsync;
    auto reserved = ReserveRetireCapacity(SwapChainRetireEntries);
    if (!reserved) {
        return Result<SwapChainHandle>::Failure(reserved.Error());
    }
    uint32 allocatedViews = 0;
    for (uint32& rtv : record.rtv) {
        auto allocated = rtvs_.Allocate();
        if (!allocated) {
            for (uint32 index = 0; index < allocatedViews; ++index) {
                rtvs_.Free(record.rtv[index]);
            }
            return FailWith<SwapChainHandle>("render target descriptor heap is full");
        }
        rtv = allocated.Value();
        ++allocatedViews;
    }
    auto views = CreateSwapChainViews(record);
    if (!views) {
        for (uint32 rtv : record.rtv) {
            rtvs_.Free(rtv);
        }
        return Result<SwapChainHandle>::Failure(views.Error());
    }

    uint32 rtvIndices[SwapChainBufferCount];
    for (uint32 index = 0; index < SwapChainBufferCount; ++index) {
        rtvIndices[index] = record.rtv[index];
    }
    auto added = swapChains_.TryAdd(Move(record));
    if (!added) {
        for (uint32 rtv : rtvIndices) {
            rtvs_.Free(rtv);
        }
        return Result<SwapChainHandle>::Failure(added.Error());
    }
    return Result<SwapChainHandle>::Success(SwapChainHandle{added.Value()});
}

Result<void> D3D12Device::ResizeSwapChain(SwapChainHandle handle, uint32 width,
                                          uint32 height) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);
    SwapChainRecord* record = swapChains_.Find(handle.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    if (width == 0 || height == 0 || recording_) {
        return Result<void>::Failure(recording_ ? ErrorCode::Busy : ErrorCode::InvalidArgument);
    }

    // 버퍼를 참조하는 모든 GPU 작업이 끝난 뒤 버퍼를 놓고 크기를 바꾼다.
    auto idle = WaitIdleLocked();
    record = swapChains_.Find(handle.slot);
    if (!idle) {
        return idle;
    }
    for (ComRef<ID3D12Resource>& buffer : record->buffers) {
        buffer.Reset();
    }
    // 이 시점부터 버퍼가 없으므로 성공할 때까지 그리기를 막는다.
    record->broken = true;
    const HRESULT resized = record->swapChain->ResizeBuffers(SwapChainBufferCount, width, height,
                                                             DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(resized)) {
        return Fail("IDXGISwapChain::ResizeBuffers", resized);
    }
    auto views = CreateSwapChainViews(*record);
    if (!views) {
        for (ComRef<ID3D12Resource>& buffer : record->buffers) {
            buffer.Reset();
        }
        return views;
    }
    record->width = width;
    record->height = height;
    record->broken = false;
    return Result<void>::Success();
}

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation). 초기 데이터는 업로드 ring에 쓰고
// 복사를 기록한다. 소유 스레드의 다음 제출이 실행한다.
Result<BufferHandle> D3D12Device::CreateBuffer(const BufferDesc& desc,
                                               Span<const uint8> initial) noexcept {
    auto valid = ValidateBufferDesc(desc);
    if (!valid || initial.Size() != desc.size) {
        return Result<BufferHandle>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(gate_);
    if (desc.size > caps_.limits.bufferBytes) {
        return FailCode<BufferHandle>(ErrorCode::Unsupported,
                                      "buffer size exceeds the device limit (bufferBytes)");
    }

    BufferRecord record;
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC resource = BufferResource(desc.size);
    if (HasAnyBufferUsage(desc.usage, BufferUsage::ShaderWrite)) {
        resource.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    // 버퍼는 COMMON에서 복사·정점·index·셰이더 읽기로 암묵 승격되고 제출이 끝나면 COMMON으로
    // 돌아온다.
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource,
                                                D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(record.resource.Out())))) {
        return FailWith<BufferHandle>("buffer creation failed");
    }
    record.address = record.resource->GetGPUVirtualAddress();
    record.size = desc.size;
    record.desc = desc;

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<BufferHandle>::Failure(reserved.Error());
    }
    auto copied = RecordBufferUpload(record.resource.Get(), 0, initial);
    if (!copied) {
        return Result<BufferHandle>::Failure(copied.Error());
    }
    auto added = buffers_.TryAdd(Move(record));
    if (!added) {
        return Result<BufferHandle>::Failure(added.Error());
    }
    return Result<BufferHandle>::Success(BufferHandle{added.Value()});
}

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation). descriptor는 shader-visible SRV
// heap 칸 하나다(SRV·UAV 모두, GX2).
Result<BufferViewHandle> D3D12Device::CreateBufferView(BufferHandle handle,
                                                       const BufferViewDesc& desc) noexcept {
    platform::LockGuard lock(gate_);
    const BufferRecord* buffer = buffers_.Find(handle.slot);
    if (buffer == nullptr) {
        return Result<BufferViewHandle>::Failure(ErrorCode::StaleHandle);
    }

    auto resolved = ResolveBufferView(buffer->desc, desc);
    if (!resolved) {
        return FailCode<BufferViewHandle>(resolved.Error(),
                                          "buffer view breaks the view rules (kind, access, "
                                          "format or range)");
    }

    const ResolvedBufferView range = resolved.Value();
    auto limited = CheckBufferViewLimits(caps_.limits, FormatCaps(desc.format), desc, range);
    if (!limited) {
        return FailCode<BufferViewHandle>(limited.Error(),
                                          "buffer view needs a format capability, alignment "
                                          "or range the D3D12 device does not provide");
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<BufferViewHandle>::Failure(reserved.Error());
    }

    auto descriptor = srvs_.Allocate();
    if (!descriptor) {
        return FailWith<BufferViewHandle>("SRV descriptor heap is full");
    }

    const d3d12::FormatMapping mapping = d3d12::MappingOf(desc.format);
    int64 cpuDescriptor = -1;
    const UINT64 first = range.offset / range.elementBytes;
    const UINT count = static_cast<UINT>(range.elementCount);
    if (desc.access == ViewAccess::Read) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Buffer.FirstElement = first;
        view.Buffer.NumElements = count;
        if (desc.kind == BufferViewKind::Typed) {
            view.Format = mapping.srv;
        } else if (desc.kind == BufferViewKind::Structured) {
            view.Buffer.StructureByteStride = range.elementBytes;
        } else {
            view.Format = DXGI_FORMAT_R32_TYPELESS;
            view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        }

        device_->CreateShaderResourceView(buffer->resource.Get(), &view,
                                          srvs_.Cpu(descriptor.Value()));
    } else {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer.FirstElement = first;
        view.Buffer.NumElements = count;
        if (desc.kind == BufferViewKind::Typed) {
            view.Format = mapping.uav;
        } else if (desc.kind == BufferViewKind::Structured) {
            view.Buffer.StructureByteStride = range.elementBytes;
        } else {
            view.Format = DXGI_FORMAT_R32_TYPELESS;
            view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        }

        device_->CreateUnorderedAccessView(buffer->resource.Get(), nullptr, &view,
                                           srvs_.Cpu(descriptor.Value()));
        // ClearWritableBufferView가 받는 view(raw, R32Uint·R32Sint typed)는 CPU 전용 칸에도
        // 같은 UAV를 둔다(ClearUnorderedAccessViewUint가 shader-visible이 아닌 칸을 요구한다).
        const bool clearable =
            desc.kind == BufferViewKind::Raw ||
            (desc.kind == BufferViewKind::Typed &&
             (desc.format == PixelFormat::R32Uint || desc.format == PixelFormat::R32Sint));
        if (clearable) {
            auto cpu = cpuUavs_.Allocate();
            if (!cpu) {
                srvs_.Free(descriptor.Value());
                return FailWith<BufferViewHandle>("CPU UAV descriptor heap is full");
            }

            cpuDescriptor = cpu.Value();
            device_->CreateUnorderedAccessView(buffer->resource.Get(), nullptr, &view,
                                               cpuUavs_.Cpu(cpu.Value()));
        }
    }

    BufferViewRecord record;
    record.resource = buffer->resource.Share();
    record.descriptor = descriptor.Value();
    record.cpuDescriptor = cpuDescriptor;
    record.format = desc.format;
    record.kind = desc.kind;
    record.access = desc.access;
    record.stride = desc.kind == BufferViewKind::Structured ? range.elementBytes : 0u;
    record.shaderType = desc.kind == BufferViewKind::Typed
                            ? GetPixelFormatInfo(desc.format).shaderType
                            : PixelShaderType::None;
    auto added = bufferViews_.TryAdd(Move(record));
    if (!added) {
        srvs_.Free(descriptor.Value());
        if (cpuDescriptor >= 0) {
            cpuUavs_.Free(static_cast<uint32>(cpuDescriptor));
        }
        return Result<BufferViewHandle>::Failure(added.Error());
    }

    return Result<BufferViewHandle>::Success(BufferViewHandle{added.Value()});
}

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation). 초기 픽셀은 업로드 기록으로 간다.
// 형식·용도 지원은 장치 능력(GetPixelFormatCaps)으로 판정한다(PixelFormat.ko.md 8.3절).
Result<TextureHandle> D3D12Device::CreateTexture(const TextureDesc& desc,
                                                 Span<const uint8> pixels) noexcept {
    const bool renderTarget = desc.usage == TextureUsage::RenderTarget;
    const bool depthTarget = desc.usage == TextureUsage::DepthStencil;
    const PixelFormatInfo& info = GetPixelFormatInfo(desc.format);
    // 형식·용도·차원 능력을 먼저 본다(능력 밖은 다른 인자 검사보다 앞서 Unsupported다).
    if (IsValidPixelFormat(desc.format) && IsValidTextureDimension(desc.dimension) &&
        !HasPixelFormatCaps(FormatCaps(desc.format), RequiredPixelFormatCaps(desc))) {
        platform::LockGuard lock(gate_);
        return UnsupportedFormat<TextureHandle>(desc.format);
    }
    auto valid = ValidateTextureDesc(desc);
    if (!valid) {
        return Result<TextureHandle>::Failure(valid.Error());
    }
    if (!CheckTextureLimits(caps_, desc)) {
        platform::LockGuard lock(gate_);
        return FailCode<TextureHandle>(ErrorCode::Unsupported,
                                       "texture size, slice count or dimension exceeds the D3D12 "
                                       "device limits");
    }
    if (desc.sampler.filter == Filter::AnisotropicPoint && !caps_.features.anisotropicPointFilter) {
        platform::LockGuard lock(gate_);
        return FailCode<TextureHandle>(ErrorCode::Unsupported,
                                       "the D3D12 device has no anisotropic filter with point mip "
                                       "selection (AnisoFilterWithPointMipSupported)");
    }
    const bool depth =
        info.kind == PixelFormatKind::Depth || info.kind == PixelFormatKind::DepthStencil;
    const uint64 size = TextureDataBytes(desc);
    const bool attachment = renderTarget || depthTarget;
    // BC는 최상위 크기가 블록 배수여야 한다(D3D12 요구, PixelFormatInfo::topLevelBlockAligned).
    const bool aligned = !info.topLevelBlockAligned ||
                         (desc.width % info.blockWidth == 0 && desc.height % info.blockHeight == 0);
    if (size == 0 || !aligned || desc.mipLevels > MaxMipLevels ||
        (attachment ? pixels.Size() != 0 : pixels.Size() != size) || (depthTarget && !depth)) {
        return Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
    }
    platform::LockGuard lock(gate_);
    auto sampler = SamplerFor(desc.sampler);
    if (!sampler) {
        return FailWith<TextureHandle>("sampler descriptor heap is full");
    }

    const d3d12::FormatMapping mapping = d3d12::MappingOf(desc.format);
    const bool sampled = HasPixelFormatCaps(FormatCaps(desc.format), PixelFormatCaps::Sampled);
    TextureRecord record;
    record.dimension = desc.dimension;
    record.width = desc.width;
    record.height = desc.height;
    record.depth = desc.depth;
    record.mipLevels = desc.mipLevels;
    record.slices = TextureSliceCount(desc);
    record.format = desc.format;
    record.renderTarget = renderTarget;
    record.depthTarget = depthTarget;
    record.sampler = sampler.Value();
    const bool volume = desc.dimension == TextureDimension::Texture3D;
    D3D12_RESOURCE_DESC resource = {};
    // cube·배열은 2D 배열 자원이다(cube 면은 UE·D3D 순서대로 slice 0–5).
    resource.Dimension =
        volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width = desc.width;
    resource.Height = desc.height;
    resource.DepthOrArraySize = static_cast<UINT16>(volume ? desc.depth : record.slices);
    resource.MipLevels = static_cast<UINT16>(desc.mipLevels);
    resource.Format = mapping.resource;
    resource.SampleDesc.Count = 1;
    // 깊이 형식은 늘 DEPTH_STENCIL 플래그로 만든다(typeless 깊이 자원의 DSV·복사 조건).
    resource.Flags =
        renderTarget ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                     : (depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE);
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    // 최적화 clear 값은 두지 않는다. 패스마다 clear 색이 다르므로 값을 두면 매번 불일치 경고가
    // 쌓인다.
    // 렌더 타깃·깊이 타깃은 셰이더 읽기 상태(ResourceAccess::SrvGraphics와 같은 PSR|NPSR)로
    // 만든다. 업로드로 채우는 텍스처가 업로드 뒤 놓이는 상태와 같아 import 선언이 생성 경로와
    // 무관하게 맞는다.
    const D3D12_RESOURCE_STATES initialState =
        attachment ? ShaderReadStates : D3D12_RESOURCE_STATE_COPY_DEST;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource,
                                                initialState, nullptr,
                                                IID_PPV_ARGS(record.resource.Out())))) {
        return FailWith<TextureHandle>("texture creation failed");
    }
    auto committed =
        record.committed.Reset(allocator_, record.mipLevels, record.slices, initialState);
    auto current = record.current.Reset(allocator_, record.mipLevels, record.slices, initialState);
    if (!committed || !current) {
        return Result<TextureHandle>::Failure(!committed ? committed.Error() : current.Error());
    }
    record.devicePlanes = d3d12::FormatPlaneCount(device_.Get(), mapping.resource);

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<TextureHandle>::Failure(reserved.Error());
    }
    if (!attachment) {
        auto uploaded = RecordTextureUpload(record, 0, record.mipLevels, pixels, false);
        if (!uploaded) {
            return Result<TextureHandle>::Failure(uploaded.Error());
        }
        // 장치 자원이 Rocks보다 plane이 많으면(D24Unorm은 D24S8 자원의 깊이 plane만 쓴다)
        // 올리지 않은 plane도 쉬는 상태로 옮긴다.
        for (uint32 plane = PixelFormatPlaneCount(desc.format); plane < record.devicePlanes;
             ++plane) {
            for (uint32 slice = 0; slice < record.slices; ++slice) {
                for (uint32 mip = 0; mip < record.mipLevels; ++mip) {
                    const D3D12_RESOURCE_BARRIER toShader = TransitionSubresource(
                        record.resource.Get(),
                        d3d12::SubresourceIndex(mip, slice, plane, record.mipLevels, record.slices),
                        D3D12_RESOURCE_STATE_COPY_DEST, ShaderReadStates);
                    uploadList_->ResourceBarrier(1, &toShader);
                }
            }
        }
    }

    auto views = CreateTextureViews(record, mapping, sampled, desc.swizzle);
    if (!views) {
        return Result<TextureHandle>::Failure(views.Error());
    }
    return AddTextureRecord(Move(record));
}

// 기본 SRV(형식이 Sampled일 때, plane 0)·RTV·DSV를 만든다(잠금 안). 실패하면 이미 잡은 칸을
// 놓는다.
Result<void> D3D12Device::CreateTextureViews(TextureRecord& record,
                                             const d3d12::FormatMapping& mapping, bool sampled,
                                             const ChannelSwizzle& swizzle) noexcept {
    if (sampled) {
        auto srv = srvs_.Allocate();
        if (!srv) {
            return Fail("shader resource descriptor heap is full");
        }
        record.srv = srv.Value();
        record.hasSrv = true;
        WriteShaderResourceView(record, mapping.srv, 0, swizzle, record.srv);
    }
    if (record.renderTarget) {
        auto rtv = rtvs_.Allocate();
        if (!rtv) {
            if (record.hasSrv) {
                srvs_.Free(record.srv);
                record.hasSrv = false;
            }
            return Fail("render target descriptor heap is full");
        }
        record.rtv = rtv.Value();
        D3D12_RENDER_TARGET_VIEW_DESC view = {};
        view.Format = mapping.rtv;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device_->CreateRenderTargetView(record.resource.Get(), &view, rtvs_.Cpu(record.rtv));
    }
    if (record.depthTarget) {
        auto dsv = dsvs_.Allocate();
        if (!dsv) {
            if (record.hasSrv) {
                srvs_.Free(record.srv);
                record.hasSrv = false;
            }
            return Fail("depth stencil descriptor heap is full");
        }
        record.dsv = dsv.Value();
        D3D12_DEPTH_STENCIL_VIEW_DESC view = {};
        view.Format = mapping.dsv;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(record.resource.Get(), &view, dsvs_.Cpu(record.dsv));
    }

    return Result<void>::Success();
}

// 다 만든 텍스처 기록의 descriptor 칸을 바로 놓는다(잠금 안, 아직 어떤 명령도 참조하지 않을
// 때).
void D3D12Device::ReleaseTextureViews(const TextureRecord& record) noexcept {
    if (record.hasSrv) {
        srvs_.Free(record.srv);
    }
    if (record.renderTarget) {
        rtvs_.Free(record.rtv);
    }
    if (record.depthTarget) {
        dsvs_.Free(record.dsv);
    }
}

// 텍스처 차원의 SRV(모든 mip·slice)를 쓴다. plane은 깊이·스텐실·비디오 plane 선택이다(2D만
// 다중 plane이다). swizzle은 Shader4ComponentMapping이다: SwizzleSource의 값(Red–Alpha 0–3,
// Zero 4, One 5)이 D3D12_SHADER_COMPONENT_MAPPING의 FROM_MEMORY_COMPONENT_0–3·FORCE_VALUE_0·1과
// 같다. 스텐실 plane 형식(X24_TYPELESS_G8_UINT·X32_TYPELESS_G8X24_UINT)은 스텐실이 G 성분이라
// 그대로 Rocks 규칙(값은 G)이고 swizzle은 그 위에 놓인다.
void D3D12Device::WriteShaderResourceView(const TextureRecord& record, DXGI_FORMAT format,
                                          uint32 plane, const ChannelSwizzle& swizzle,
                                          uint32 slot) noexcept {
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = format;
    view.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        static_cast<UINT>(swizzle.red), static_cast<UINT>(swizzle.green),
        static_cast<UINT>(swizzle.blue), static_cast<UINT>(swizzle.alpha));
    switch (record.dimension) {
    case TextureDimension::Texture2D:
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = record.mipLevels;
        view.Texture2D.PlaneSlice = plane;
        break;
    case TextureDimension::Texture2DArray:
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipLevels = record.mipLevels;
        view.Texture2DArray.ArraySize = record.slices;
        view.Texture2DArray.PlaneSlice = plane;
        break;
    case TextureDimension::Texture3D:
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        view.Texture3D.MipLevels = record.mipLevels;
        break;
    case TextureDimension::TextureCube:
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        view.TextureCube.MipLevels = record.mipLevels;
        break;
    case TextureDimension::TextureCubeArray:
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        view.TextureCubeArray.MipLevels = record.mipLevels;
        view.TextureCubeArray.NumCubes = record.slices / CubeFaceCount;
        break;
    }
    device_->CreateShaderResourceView(record.resource.Get(), &view, srvs_.Cpu(slot));
}

// 기록을 표에 넣고 handle을 돌려준다(잠금 안). 실패하면 descriptor 칸을 놓는다.
Result<TextureHandle> D3D12Device::AddTextureRecord(TextureRecord&& record) noexcept {
    TextureRecord kept;
    kept.hasSrv = record.hasSrv;
    kept.srv = record.srv;
    kept.renderTarget = record.renderTarget;
    kept.rtv = record.rtv;
    kept.depthTarget = record.depthTarget;
    kept.dsv = record.dsv;
    auto added = textures_.TryAdd(Move(record));
    if (!added) {
        // 아직 어떤 명령도 이 텍스처를 참조하지 않으므로 바로 놓는다.
        ReleaseTextureViews(kept);
        return Result<TextureHandle>::Failure(added.Error());
    }
    return Result<TextureHandle>::Success(TextureHandle{added.Value()});
}

PixelFormatCaps D3D12Device::FormatCaps(PixelFormat format) const noexcept {
    const uint32 number = PixelFormatNumber(format);
    return number <= MaxPixelFormatNumber ? formatCaps_[number] : PixelFormatCaps::None;
}

PixelFormatCaps D3D12Device::GetPixelFormatCaps(PixelFormat format) const noexcept {
    return FormatCaps(format);
}

// 원 텍스처의 자원을 공유하는 SRV view다(PixelFormat.ko.md 9–11절). 다른 스레드에서도 부를 수
// 있다.
Result<TextureHandle> D3D12Device::CreateTextureView(TextureHandle texture,
                                                     const TextureViewDesc& desc) noexcept {
    platform::LockGuard lock(gate_);
    TextureRecord* source = textures_.Find(texture.slot);
    if (source == nullptr || source->retirePending) {
        return Result<TextureHandle>::Failure(ErrorCode::StaleHandle);
    }
    if (source->view) {
        return Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
    }
    auto resolved = ResolveTextureViewFormat(source->format, desc);
    if (!resolved) {
        return Result<TextureHandle>::Failure(resolved.Error());
    }
    // sRGB 짝 view는 짝 형식이, plane view는 원 형식이 샘플 가능해야 한다.
    const PixelFormat sampledFormat =
        PixelFormatPlaneCount(source->format) == 2 ? source->format : resolved.Value();
    const DXGI_FORMAT srvFormat =
        d3d12::ViewSrvFormat(source->format, desc.plane, resolved.Value());
    if (!HasPixelFormatCaps(FormatCaps(sampledFormat), PixelFormatCaps::Sampled) ||
        srvFormat == DXGI_FORMAT_UNKNOWN) {
        return UnsupportedFormat<TextureHandle>(resolved.Value());
    }
    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<TextureHandle>::Failure(reserved.Error());
    }
    auto srv = srvs_.Allocate();
    if (!srv) {
        return FailWith<TextureHandle>("shader resource descriptor heap is full");
    }
    const PixelFormatPlane& layout = GetPixelFormatInfo(source->format).planes[desc.plane];
    TextureRecord record;
    // view는 원 자원에 참조를 하나 더 잡는다(원 텍스처를 먼저 Retire해도 자원이 산다).
    record.resource = source->resource.Share();
    record.dimension = source->dimension;
    record.width = MipExtent(source->width, layout.widthShift);
    record.height = MipExtent(source->height, layout.heightShift);
    record.depth = source->depth;
    record.mipLevels = source->mipLevels;
    record.slices = source->slices;
    record.format = resolved.Value();
    record.view = true;
    record.source = texture.slot;
    record.devicePlanes = source->devicePlanes;
    record.sampler = source->sampler;
    record.srv = srv.Value();
    record.hasSrv = true;
    WriteShaderResourceView(record, srvFormat, desc.plane, desc.swizzle, record.srv);
    auto added = AddTextureRecord(Move(record));
    if (added) {
        ++source->viewCount;
    }
    return added;
}

void D3D12Device::Retire(BufferHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }
    platform::LockGuard lock(gate_);

    BufferRecord* record = buffers_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }
    Defer(Move(record->resource), -1, -1);
    auto removed = buffers_.TryRemove(handle.slot);
    (void)removed;
}

void D3D12Device::Retire(BufferViewHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    BufferViewRecord* record = bufferViews_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }

    // CPU 전용 UAV 칸은 ClearUnorderedAccessViewUint 기록 때만 읽히므로 바로 놓는다.
    if (record->cpuDescriptor >= 0) {
        cpuUavs_.Free(static_cast<uint32>(record->cpuDescriptor));
    }
    Defer(Move(record->resource), record->descriptor, -1);
    auto removed = bufferViews_.TryRemove(handle.slot);
    (void)removed;
}

void D3D12Device::Retire(TextureHandle handle) noexcept {
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
    const bool view = record->view;
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

// 텍스처 기록 하나를 회수 대기로 넘기고 표에서 뺀다(잠금 안).
void D3D12Device::RetireTextureRecord(SlotHandle slot, TextureRecord& record) noexcept {
    Defer(Move(record.resource), record.hasSrv ? static_cast<int64>(record.srv) : int64{-1},
          record.renderTarget ? static_cast<int64>(record.rtv) : int64{-1},
          record.depthTarget ? static_cast<int64>(record.dsv) : int64{-1});
    auto removed = textures_.TryRemove(slot);
    (void)removed;
}

void D3D12Device::Retire(PipelineHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }
    platform::LockGuard lock(gate_);

    PipelineRecord* record = pipelines_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }
    Defer(Move(record->state), -1, -1);
    auto removed = pipelines_.TryRemove(handle.slot);
    (void)removed;
}

void D3D12Device::Retire(SwapChainHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }
    platform::LockGuard lock(gate_);

    SwapChainRecord* record = swapChains_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }
    // 백버퍼와 RTV는 마지막으로 쓴 프레임이 끝난 뒤 놓는다. 기록 중인 프레임이 이 스왑체인에
    // 그리고 있어도 retire 항목이 참조를 쥐므로 EndFrame의 barrier까지 유효하다(present는
    // 생략된다).
    for (uint32 index = 0; index < SwapChainBufferCount; ++index) {
        Defer(Move(record->buffers[index]), -1, record->rtv[index]);
    }
    Defer(Move(record->swapChain), -1, -1);
    auto removed = swapChains_.TryRemove(handle.slot);
    (void)removed;
}

// desc와 같은 샘플러 칸을 돌려주고 없으면 만든다. 칸은 장치 수명 동안 유지한다(조합 수가
// 작다).
Result<uint32> D3D12Device::SamplerFor(const SamplerDesc& desc) noexcept {
    for (uint32 index = 0; index < samplerCount_; ++index) {
        if (samplerDescs_[index].desc == desc) {
            return Result<uint32>::Success(samplerDescs_[index].slot);
        }
    }
    if (samplerCount_ >= SamplerCapacity) {
        return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
    }
    auto slot = samplers_.Allocate();
    if (!slot) {
        return Result<uint32>::Failure(slot.Error());
    }
    D3D12_SAMPLER_DESC sampler = {};
    sampler.Filter = SamplerFilter(desc.filter, desc.comparison);
    sampler.AddressU = SamplerAddress(desc.addressU);
    sampler.AddressV = SamplerAddress(desc.addressV);
    sampler.AddressW = SamplerAddress(desc.addressW);
    sampler.MaxAnisotropy = IsAnisotropicFilter(desc.filter) ? desc.maxAnisotropy : 1u;
    // D3D12_COMPARISON_FUNC는 NEVER = 1부터 CompareFunction과 같은 순서다.
    sampler.ComparisonFunc =
        desc.comparison ? static_cast<D3D12_COMPARISON_FUNC>(static_cast<uint32>(desc.compare) + 1)
                        : D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    BorderColorValue(desc.borderColor, sampler.BorderColor);
    device_->CreateSampler(&sampler, samplers_.Cpu(slot.Value()));
    samplerDescs_[samplerCount_++] = {desc, slot.Value()};
    return Result<uint32>::Success(slot.Value());
}

Result<void> D3D12Device::CreateSwapChainViews(SwapChainRecord& record) noexcept {
    D3D12_RENDER_TARGET_VIEW_DESC view = {};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    for (uint32 index = 0; index < SwapChainBufferCount; ++index) {
        if (FAILED(record.swapChain->GetBuffer(index, IID_PPV_ARGS(record.buffers[index].Out())))) {
            return Fail("swap chain GetBuffer failed");
        }
        device_->CreateRenderTargetView(record.buffers[index].Get(), &view,
                                        rtvs_.Cpu(record.rtv[index]));
    }
    return Result<void>::Success();
}

} // namespace rocks::graphics::d3d12
