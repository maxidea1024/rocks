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
    // 이 시점부터 버퍼가 없으므로 성공할 때까지 그리기를 막는다. 그리는 중인 프레임도 버린다.
    record->broken = true;
    record->frameOpen = false;
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
    return CreateBufferIn(desc, initial, nullptr);
}

D3D12_RESOURCE_DESC D3D12Device::BufferResourceFor(const BufferDesc& desc) noexcept {
    D3D12_RESOURCE_DESC resource = BufferResource(desc.size);
    if (HasAnyBufferUsage(desc.usage, BufferUsage::ShaderWrite)) {
        resource.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    return resource;
}

Result<BufferHandle> D3D12Device::CreateBufferIn(const BufferDesc& desc, Span<const uint8> initial,
                                                 const Placement* placement) noexcept {
    // placed 버퍼는 초기 데이터가 없다(내용은 획득 뒤 쓴다).
    auto valid = ValidateBufferDesc(desc);
    if (!valid || initial.Size() != (placement != nullptr ? 0u : desc.size)) {
        return Result<BufferHandle>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(gate_);
    if (desc.size > caps_.limits.bufferBytes) {
        return FailCode<BufferHandle>(ErrorCode::Unsupported,
                                      "buffer size exceeds the device limit (bufferBytes)");
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<BufferHandle>::Failure(reserved.Error());
    }

    BufferRecord record;
    const D3D12_RESOURCE_DESC resource = BufferResourceFor(desc);
    // 버퍼는 COMMON에서 복사·정점·index·셰이더 읽기로 암묵 승격되고 제출이 끝나면 COMMON으로
    // 돌아온다(placed 버퍼도 같다).
    uint64 placedBytes = 0;
    if (placement != nullptr) {
        auto heap = CheckPlacement(*placement, TransientHeapKind::Buffers, resource, placedBytes);
        if (!heap) {
            return Result<BufferHandle>::Failure(heap.Error());
        }

        const HRESULT created = device_->CreatePlacedResource(
            heap.Value()->heap.Get(), placement->offset, &resource, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(record.resource.Out()));
        if (FAILED(created)) {
            return FailWith<BufferHandle>("CreatePlacedResource (buffer)", created);
        }

        record.heap = placement->heap;
    } else {
        const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource,
                                                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(record.resource.Out())))) {
            return FailWith<BufferHandle>("buffer creation failed");
        }
    }

    record.address = record.resource->GetGPUVirtualAddress();
    record.size = desc.size;
    record.desc = desc;
    // placed 버퍼는 업로드하지 않는다(heap 구간을 다른 자원이 쓰는 중일 수 있다).
    if (placement == nullptr) {
        auto copied = RecordBufferUpload(record.resource.Get(), 0, initial);
        if (!copied) {
            return Result<BufferHandle>::Failure(copied.Error());
        }
    }

    auto added = buffers_.TryAdd(Move(record));
    if (!added) {
        return Result<BufferHandle>::Failure(added.Error());
    }

    if (placement != nullptr) {
        TrackPlaced(placement->heap, added.Value(), placement->offset, placedBytes);
    }

    return Result<BufferHandle>::Success(BufferHandle{added.Value()});
}

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation). descriptor는 CPU 전용 SRV
// heap 칸 하나다(SRV·UAV 모두, GX2 — 그리기 때 shader-visible ring으로 복사한다).
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
    record.heap = buffer->heap;
    auto added = bufferViews_.TryAdd(Move(record));
    if (!added) {
        srvs_.Free(descriptor.Value());
        if (cpuDescriptor >= 0) {
            cpuUavs_.Free(static_cast<uint32>(cpuDescriptor));
        }
        return Result<BufferViewHandle>::Failure(added.Error());
    }

    // placed 버퍼의 view는 버퍼 자원을 붙들므로 heap의 placed 수에 든다(view 회수 때 줄인다).
    TransientHeapRecord* heap = heaps_.Find(buffer->heap);
    if (heap != nullptr) {
        ++heap->placedCount;
    }

    return Result<BufferViewHandle>::Success(BufferViewHandle{added.Value()});
}

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation). 초기 픽셀은 업로드 기록으로 간다.
// 형식·용도 지원은 장치 능력(GetPixelFormatCaps)으로 판정한다(Docs/Design/PixelFormat 8.3절).
Result<TextureHandle> D3D12Device::CreateTexture(const TextureDesc& desc,
                                                 Span<const uint8> pixels) noexcept {
    return CreateTextureIn(desc, pixels, nullptr);
}

D3D12Device::TextureDescCheck D3D12Device::CheckTextureDesc(const TextureDesc& desc,
                                                            usize pixelBytes) const noexcept {
    TextureDescCheck check;
    const bool renderTarget = HasTextureUsage(desc.usage, TextureUsage::RenderTarget);
    const bool depthTarget = HasTextureUsage(desc.usage, TextureUsage::DepthStencil);
    const bool storage = HasTextureUsage(desc.usage, TextureUsage::Storage);
    const PixelFormatInfo& info = GetPixelFormatInfo(desc.format);
    // 형식·용도·차원 능력을 먼저 본다(능력 밖은 다른 인자 검사보다 앞서 Unsupported다).
    if (IsValidPixelFormat(desc.format) && IsValidTextureDimension(desc.dimension) &&
        !HasPixelFormatCaps(FormatCaps(desc.format), RequiredPixelFormatCaps(desc))) {
        check.code = ErrorCode::Unsupported;
        check.unsupportedFormat = true;
        return check;
    }

    auto valid = ValidateTextureDesc(desc);
    if (!valid) {
        check.code = valid.Error();
        return check;
    }

    if (desc.sampleCount > 1 &&
        (GetPixelFormatSampleCounts(desc.format) & SampleCountBit(desc.sampleCount)) == 0) {
        check.code = ErrorCode::Unsupported;
        check.detail = "MSAA sample count is not supported for this format by the D3D12 device";
        return check;
    }

    if (!CheckTextureLimits(caps_, desc)) {
        check.code = ErrorCode::Unsupported;
        check.detail = "texture size, slice count or dimension exceeds the D3D12 device limits";
        return check;
    }

    const bool depth =
        info.kind == PixelFormatKind::Depth || info.kind == PixelFormatKind::DepthStencil;
    const uint64 size = TextureDataBytes(desc);
    // 렌더 타깃·깊이·Storage는 초기 픽셀이 없다(업로드하지 않는다).
    const bool empty = renderTarget || depthTarget || storage;
    // BC는 최상위 크기가 블록 배수여야 한다(D3D12 요구, PixelFormatInfo::topLevelBlockAligned).
    const bool aligned = !info.topLevelBlockAligned ||
                         (desc.width % info.blockWidth == 0 && desc.height % info.blockHeight == 0);
    if (size == 0 || !aligned || desc.mipLevels > MaxMipLevels ||
        (empty ? pixelBytes != 0 : pixelBytes != size) || (depthTarget && !depth)) {
        check.code = ErrorCode::InvalidArgument;
    }

    return check;
}

D3D12_RESOURCE_DESC D3D12Device::TextureResourceFor(const TextureDesc& desc) noexcept {
    const PixelFormatInfo& info = GetPixelFormatInfo(desc.format);
    const bool depth =
        info.kind == PixelFormatKind::Depth || info.kind == PixelFormatKind::DepthStencil;
    const bool volume = desc.dimension == TextureDimension::Texture3D;
    D3D12_RESOURCE_DESC resource = {};
    // cube·배열은 2D 배열 자원이다(cube 면은 UE·D3D 순서대로 slice 0–5).
    resource.Dimension =
        volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width = desc.width;
    resource.Height = desc.height;
    resource.DepthOrArraySize = static_cast<UINT16>(volume ? desc.depth : TextureSliceCount(desc));
    resource.MipLevels = static_cast<UINT16>(desc.mipLevels);
    resource.Format = d3d12::MappingOf(desc.format).resource;
    resource.SampleDesc.Count = desc.sampleCount;
    // 깊이 형식은 늘 DEPTH_STENCIL 플래그로 만든다(typeless 깊이 자원의 DSV·복사 조건).
    // 렌더 타깃 + Storage는 두 flag를 함께 둔다(용도 비트 묶음, 텍스처 GX2 나머지 A부).
    resource.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (HasTextureUsage(desc.usage, TextureUsage::RenderTarget)) {
        resource.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    } else if (depth) {
        resource.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }

    if (HasTextureUsage(desc.usage, TextureUsage::Storage)) {
        resource.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    return resource;
}

Result<TextureHandle> D3D12Device::CreateTextureIn(const TextureDesc& desc,
                                                   Span<const uint8> pixels,
                                                   const Placement* placement) noexcept {
    const TextureDescCheck check = CheckTextureDesc(desc, pixels.Size());
    if (check.code != ErrorCode::None) {
        if (check.unsupportedFormat) {
            platform::LockGuard lock(gate_);
            return UnsupportedFormat<TextureHandle>(desc.format);
        }

        if (check.detail != nullptr) {
            platform::LockGuard lock(gate_);
            return FailCode<TextureHandle>(check.code, check.detail);
        }

        return Result<TextureHandle>::Failure(check.code);
    }

    const bool renderTarget = HasTextureUsage(desc.usage, TextureUsage::RenderTarget);
    const bool depthTarget = HasTextureUsage(desc.usage, TextureUsage::DepthStencil);
    const bool storage = HasTextureUsage(desc.usage, TextureUsage::Storage);
    // 렌더 타깃·깊이·Storage는 초기 픽셀이 없다(업로드하지 않는다).
    const bool empty = renderTarget || depthTarget || storage;
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
    record.usage = desc.usage;
    record.sampleCount = desc.sampleCount;
    record.viewRange = {0, record.mipLevels, 0, record.slices};
    record.sampler = sampler.Value();
    record.comparison = desc.sampler.comparison;
    const D3D12_RESOURCE_DESC resource = TextureResourceFor(desc);
    // 최적화 clear 값은 두지 않는다. 패스마다 clear 색이 다르므로 값을 두면 매번 불일치 경고가
    // 쌓인다.
    // 렌더 타깃·깊이 타깃은 셰이더 읽기 상태(ResourceAccess::SrvGraphics와 같은 PSR|NPSR)로
    // 만든다. 업로드로 채우는 텍스처가 업로드 뒤 놓이는 상태와 같아 import 선언이 생성 경로와
    // 무관하게 맞는다. placed 텍스처는 첫 사용이 획득 전이(before Discard)이므로 획득의 초기화
    // 상태(렌더 타깃 RENDER_TARGET, 깊이 DEPTH_WRITE, 그 밖 COMMON)로 만든다(GX12).
    D3D12_RESOURCE_STATES initialState = empty ? ShaderReadStates : D3D12_RESOURCE_STATE_COPY_DEST;
    uint64 placedBytes = 0;
    if (placement != nullptr) {
        initialState = (resource.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0
                           ? D3D12_RESOURCE_STATE_RENDER_TARGET
                       : (resource.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0
                           ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                           : D3D12_RESOURCE_STATE_COMMON;
        auto heap = CheckPlacement(*placement, TransientHeapKindOf(desc), resource, placedBytes);
        if (!heap) {
            return Result<TextureHandle>::Failure(heap.Error());
        }

        const HRESULT created = device_->CreatePlacedResource(
            heap.Value()->heap.Get(), placement->offset, &resource, initialState, nullptr,
            IID_PPV_ARGS(record.resource.Out()));
        if (FAILED(created)) {
            return FailWith<TextureHandle>("CreatePlacedResource (texture)", created);
        }

        record.heap = placement->heap;
    } else {
        const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource,
                                                    initialState, nullptr,
                                                    IID_PPV_ARGS(record.resource.Out())))) {
            return FailWith<TextureHandle>("texture creation failed");
        }
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
    if (!empty) {
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

    auto added = AddTextureRecord(Move(record));
    if (added && placement != nullptr) {
        TrackPlaced(placement->heap, added.Value().slot, placement->offset, placedBytes);
    }

    return added;
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
        WriteRenderTargetView(record, 0, 0, rtvs_.Cpu(record.rtv));
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
        WriteDepthStencilView(record, 0, 0, false, dsvs_.Cpu(record.dsv));
    }

    return Result<void>::Success();
}

// attachment (mip, slice)의 RTV다. 배열·cube·cube 배열은 2D 배열 RTV의 층 하나, 3D는 깊이 면
// 하나다(텍스처 GX2 C부).
void D3D12Device::WriteRenderTargetView(const TextureRecord& record, uint32 mipLevel, uint32 slice,
                                        D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept {
    D3D12_RENDER_TARGET_VIEW_DESC view = {};
    view.Format = d3d12::MappingOf(record.format).rtv;
    if (record.sampleCount > 1 && record.dimension == TextureDimension::Texture2DArray) {
        // MSAA 배열의 층 하나다(텍스처 GX2 나머지 C부).
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
        view.Texture2DMSArray.FirstArraySlice = slice;
        view.Texture2DMSArray.ArraySize = 1;
    } else if (record.sampleCount > 1) {
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    } else if (record.dimension == TextureDimension::Texture3D) {
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        view.Texture3D.MipSlice = mipLevel;
        view.Texture3D.FirstWSlice = slice;
        view.Texture3D.WSize = 1;
    } else if (record.dimension == TextureDimension::Texture2D) {
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = mipLevel;
    } else {
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipSlice = mipLevel;
        view.Texture2DArray.FirstArraySlice = slice;
        view.Texture2DArray.ArraySize = 1;
    }

    device_->CreateRenderTargetView(record.resource.Get(), &view, handle);
}

void D3D12Device::WriteDepthStencilView(const TextureRecord& record, uint32 mipLevel, uint32 slice,
                                        bool readOnly,
                                        D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept {
    D3D12_DEPTH_STENCIL_VIEW_DESC view = {};
    view.Format = d3d12::MappingOf(record.format).dsv;
    if (record.sampleCount > 1 && record.dimension == TextureDimension::Texture2DArray) {
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
        view.Texture2DMSArray.FirstArraySlice = slice;
        view.Texture2DMSArray.ArraySize = 1;
    } else if (record.sampleCount > 1) {
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    } else if (record.dimension == TextureDimension::Texture2D) {
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = mipLevel;
    } else {
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipSlice = mipLevel;
        view.Texture2DArray.FirstArraySlice = slice;
        view.Texture2DArray.ArraySize = 1;
    }

    if (readOnly) {
        view.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        if (GetPixelFormatInfo(record.format).stencilBits != 0) {
            view.Flags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
        }
    }

    device_->CreateDepthStencilView(record.resource.Get(), &view, handle);
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

// 텍스처 차원의 SRV(record.viewRange의 mip·slice)를 쓴다. plane은 깊이·스텐실·비디오 plane
// 선택이다(2D만 다중 plane이다). swizzle은 Shader4ComponentMapping이다: SwizzleSource의
// 값(Red–Alpha 0–3, Zero 4, One 5)이 D3D12_SHADER_COMPONENT_MAPPING의
// FROM_MEMORY_COMPONENT_0–3·FORCE_VALUE_0·1과 같다. 스텐실 plane
// 형식(X24_TYPELESS_G8_UINT·X32_TYPELESS_G8X24_UINT)은 스텐실이 G 성분이라 그대로 Rocks 규칙(값은
// G)이고 swizzle은 그 위에 놓인다.
void D3D12Device::WriteShaderResourceView(const TextureRecord& record, DXGI_FORMAT format,
                                          uint32 plane, const ChannelSwizzle& swizzle,
                                          uint32 slot) noexcept {
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = format;
    view.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        static_cast<UINT>(swizzle.red), static_cast<UINT>(swizzle.green),
        static_cast<UINT>(swizzle.blue), static_cast<UINT>(swizzle.alpha));
    const ResolvedSubresourceRange& range = record.viewRange;
    switch (record.dimension) {
    case TextureDimension::Texture2D:
        if (record.sampleCount > 1) {
            // MSAA는 표본 번호 읽기(Texture2DMS)다. mip·plane 지정이 없다.
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
            break;
        }

        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MostDetailedMip = range.firstMip;
        view.Texture2D.MipLevels = range.mipCount;
        view.Texture2D.PlaneSlice = plane;
        break;
    case TextureDimension::Texture2DArray:
        if (record.sampleCount > 1) {
            // MSAA 배열은 표본 번호 읽기(Texture2DMSArray)다.
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
            view.Texture2DMSArray.FirstArraySlice = range.firstSlice;
            view.Texture2DMSArray.ArraySize = range.sliceCount;
            break;
        }

        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MostDetailedMip = range.firstMip;
        view.Texture2DArray.MipLevels = range.mipCount;
        view.Texture2DArray.FirstArraySlice = range.firstSlice;
        view.Texture2DArray.ArraySize = range.sliceCount;
        view.Texture2DArray.PlaneSlice = plane;
        break;
    case TextureDimension::Texture3D:
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        view.Texture3D.MostDetailedMip = range.firstMip;
        view.Texture3D.MipLevels = range.mipCount;
        break;
    case TextureDimension::TextureCube:
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        view.TextureCube.MostDetailedMip = range.firstMip;
        view.TextureCube.MipLevels = range.mipCount;
        break;
    case TextureDimension::TextureCubeArray:
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        view.TextureCubeArray.MostDetailedMip = range.firstMip;
        view.TextureCubeArray.MipLevels = range.mipCount;
        view.TextureCubeArray.First2DArrayFace = range.firstSlice;
        view.TextureCubeArray.NumCubes = range.sliceCount / CubeFaceCount;
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

// 원 텍스처의 자원을 공유하는 SRV view다(Docs/Design/PixelFormat 9–11절). 다른 스레드에서도 부를 수
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
    auto resolved = ResolveTextureViewFormat(source->format, desc, source->comparison);
    if (!resolved) {
        return Result<TextureHandle>::Failure(resolved.Error());
    }
    auto range = ResolveTextureViewRange(source->dimension, source->usage,
                                         {source->mipLevels, source->slices}, desc);
    if (!range) {
        return Result<TextureHandle>::Failure(range.Error());
    }
    if (desc.access == ViewAccess::ReadWrite) {
        return CreateWritableTextureView(texture, *source, resolved.Value(), range.Value(),
                                         TextureViewDimensionOf(source->dimension, desc));
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
    // view는 원 자원에 참조를 하나 더 잡는다(원 텍스처를 먼저 Retire해도 자원이 산다). 차원은 view
    // 차원이다(SRV ViewDimension, 텍스처 GX2 나머지 B부).
    record.resource = source->resource.Share();
    record.dimension = TextureViewDimensionOf(source->dimension, desc);
    record.width = MipExtent(source->width, layout.widthShift);
    record.height = MipExtent(source->height, layout.heightShift);
    record.depth = source->depth;
    record.mipLevels = source->mipLevels;
    record.slices = source->slices;
    record.format = resolved.Value();
    record.usage = source->usage;
    record.sampleCount = source->sampleCount;
    record.view = true;
    record.source = texture.slot;
    record.viewRange = range.Value();
    record.devicePlanes = source->devicePlanes;
    record.sampler = source->sampler;
    record.comparison = source->comparison;
    record.srv = srv.Value();
    record.hasSrv = true;
    WriteShaderResourceView(record, srvFormat, desc.plane, desc.swizzle, record.srv);
    auto added = AddTextureRecord(Move(record));
    if (added) {
        ++source->viewCount;
    }
    return added;
}

// Storage 텍스처 mip 하나(slice 범위)의 UAV view다(잠금 안, SetWritableTexture로 묶는다). 형식이
// StorageWrite가 아니면 Unsupported다.
Result<TextureHandle> D3D12Device::CreateWritableTextureView(TextureHandle texture,
                                                             TextureRecord& source,
                                                             PixelFormat format,
                                                             const ResolvedSubresourceRange& range,
                                                             TextureDimension dimension) noexcept {
    const DXGI_FORMAT uavFormat = d3d12::MappingOf(format).uav;
    if (!HasPixelFormatCaps(FormatCaps(format), PixelFormatCaps::StorageWrite) ||
        uavFormat == DXGI_FORMAT_UNKNOWN) {
        return UnsupportedFormat<TextureHandle>(format);
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<TextureHandle>::Failure(reserved.Error());
    }

    auto uav = srvs_.Allocate();
    if (!uav) {
        return FailWith<TextureHandle>("shader resource descriptor heap is full");
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
    view.Format = uavFormat;
    switch (dimension) {
    case TextureDimension::Texture3D:
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        view.Texture3D.MipSlice = range.firstMip;
        view.Texture3D.FirstWSlice = 0;
        view.Texture3D.WSize = MipExtent(source.depth, range.firstMip);
        break;
    // cube 계열의 쓰기 view는 면 단위 2D 배열이다(cube UAV가 없다, 텍스처 GX2 나머지 A부).
    case TextureDimension::Texture2DArray:
    case TextureDimension::TextureCube:
    case TextureDimension::TextureCubeArray:
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipSlice = range.firstMip;
        view.Texture2DArray.FirstArraySlice = range.firstSlice;
        view.Texture2DArray.ArraySize = range.sliceCount;
        break;
    default:
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = range.firstMip;
        break;
    }

    device_->CreateUnorderedAccessView(source.resource.Get(), nullptr, &view,
                                       srvs_.Cpu(uav.Value()));
    TextureRecord record;
    record.resource = source.resource.Share();
    record.dimension = dimension;
    record.width = source.width;
    record.height = source.height;
    record.depth = source.depth;
    record.mipLevels = source.mipLevels;
    record.slices = source.slices;
    record.format = format;
    record.usage = source.usage;
    record.view = true;
    record.writable = true;
    record.source = texture.slot;
    record.viewRange = range;
    record.devicePlanes = source.devicePlanes;
    record.sampler = source.sampler;
    // UAV 칸을 srv에 두고 hasSrv로 놓는다(Retire·AddTextureRecord가 같은 칸을 회수한다).
    record.srv = uav.Value();
    record.hasSrv = true;
    auto added = AddTextureRecord(Move(record));
    if (added) {
        ++source.viewCount;
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
    // placed 버퍼는 heap 구간 목록에서 빼고, 자원을 놓은 뒤 heap의 placed 수를 줄인다(GX12).
    ForgetPlaced(record->heap, handle.slot);
    Defer(Move(record->resource), -1, -1, -1, record->heap);
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
    Defer(Move(record->resource), record->descriptor, -1, -1, record->heap);
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
    // placed 텍스처는 heap 구간 목록에서 빼고, 자원을 놓은 뒤 heap의 placed 수를 줄인다(GX12).
    ForgetPlaced(record.heap, slot);
    Defer(Move(record.resource), record.hasSrv ? static_cast<int64>(record.srv) : int64{-1},
          record.renderTarget ? static_cast<int64>(record.rtv) : int64{-1},
          record.depthTarget ? static_cast<int64>(record.dsv) : int64{-1}, record.heap);
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
    // AnisotropicPoint 하드웨어 필터가 없으면 UE처럼 AnisotropicLinear로 만든다(사용자 결정,
    // DeviceFeatures::anisotropicPointFilter가 거짓, 처음 한 번 Debug 로그).
    Filter filter = desc.filter;
    if (filter == Filter::AnisotropicPoint && !caps_.features.anisotropicPointFilter) {
        filter = Filter::AnisotropicLinear;
        if (!anisotropicPointReported_) {
            anisotropicPointReported_ = true;
            Log::Debug(LogGraphicsD3D12,
                       "AnisotropicPoint samplers are created as AnisotropicLinear because the "
                       "device has no AnisoFilterWithPointMipSupported");
        }
    }
    D3D12_SAMPLER_DESC sampler = {};
    sampler.Filter = SamplerFilter(filter, desc.comparison);
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

Result<TextureSubresources>
D3D12Device::GetTextureSubresources(TextureHandle texture) const noexcept {
    platform::LockGuard lock(gate_);
    const TextureRecord* record = textures_.Find(texture.slot);
    if (record == nullptr) {
        return Result<TextureSubresources>::Failure(ErrorCode::StaleHandle);
    }

    return Result<TextureSubresources>::Success(
        TextureSubresources{record->mipLevels, record->slices});
}

Result<TextureViewInfo> D3D12Device::GetTextureViewInfo(TextureHandle texture) noexcept {
    platform::LockGuard lock(gate_);
    const TextureRecord* record = textures_.Find(texture.slot);
    if (record == nullptr) {
        return Result<TextureViewInfo>::Failure(ErrorCode::StaleHandle);
    }

    TextureViewInfo info;
    info.dimension = record->dimension;
    info.format = record->format;
    info.sampleCount = record->sampleCount;
    info.access = record->writable ? ViewAccess::ReadWrite : ViewAccess::Read;
    info.sampled = record->hasSrv;
    info.comparison = record->comparison;
    return Result<TextureViewInfo>::Success(info);
}

Result<BufferViewInfo> D3D12Device::GetBufferViewInfo(BufferViewHandle view) noexcept {
    platform::LockGuard lock(gate_);
    const BufferViewRecord* record = bufferViews_.Find(view.slot);
    if (record == nullptr) {
        return Result<BufferViewInfo>::Failure(ErrorCode::StaleHandle);
    }

    return Result<BufferViewInfo>::Success(
        BufferViewInfo{record->kind, record->access, record->shaderType, record->stride});
}

} // namespace rocks::graphics::d3d12
