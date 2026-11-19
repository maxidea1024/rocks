#include "D3D12DeviceImpl.h"

namespace rocks::graphics::d3d12 {

Result<CommandContext*> D3D12Device::BeginFrame(SwapChainHandle handle,
                                                const ClearColor& clear) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<CommandContext*>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);

    SwapChainRecord* record = swapChains_.Find(handle.slot);
    if (record == nullptr) {
        return Result<CommandContext*>::Failure(ErrorCode::StaleHandle);
    }

    if (recording_) {
        return Result<CommandContext*>::Failure(ErrorCode::Busy);
    }

    if (timeline_.IsLost()) {
        return Result<CommandContext*>::Failure(ErrorCode::Destroyed);
    }

    if (record->broken) {
        return FailWith<CommandContext*>("swap chain has no buffers after a failed resize");
    }

    const uint32 backBuffer = record->swapChain->GetCurrentBackBufferIndex();
    auto begun = BeginRecording();
    if (!begun) {
        return Result<CommandContext*>::Failure(begun.Error());
    }

    target_ = {};
    target_.resource = record->buffers[backBuffer].Get();
    target_.restState = D3D12_RESOURCE_STATE_PRESENT;
    target_.rtv = rtvs_.Cpu(record->rtv[backBuffer]);
    target_.width = record->width;
    target_.height = record->height;
    target_.format = PixelFormat::RGBA8UnormSrgb;
    target_.colorCount = 1;
    target_.rtvs[0] = target_.rtv;
    presentTarget_ = handle;
    PassDesc pass;
    pass.colorCount = 1;
    pass.colors[0].clear = clear;
    StartPass(pass);
    return Result<CommandContext*>::Success(this);
}

Result<GpuFencePoint> D3D12Device::EndFrame() noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<GpuFencePoint>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);

    if (!recording_) {
        return Result<GpuFencePoint>::Failure(ErrorCode::InvalidArgument);
    }

    // 끝나지 않은 분리 전이는 적용하지 않는다(계약 위반이므로 검증 장치는 오류로 센다).
    const usize unended = splits_.Close();
    if (validation_ && unended != 0) {
        validationErrors_ += unended;
        auto failed = Fail("split transitions were begun but not ended before EndFrame");
        (void)failed;
    }

    ResolveAttachments();

    // 스왑체인 백버퍼만 present 상태로 돌린다. offscreen 대상은 RenderTarget 그대로 둔다(GX5:
    // 다음 사용이 명시 전이나 암묵 전이로 바꾼다).
    if (!presentTarget_.IsNull()) {
        const D3D12_RESOURCE_BARRIER back = TransitionBarrier(
            target_.resource, D3D12_RESOURCE_STATE_RENDER_TARGET, target_.restState);
        list_->ResourceBarrier(1, &back);
    }

    CloseMarkers();
    const TimestampRange stamps = timestamps_.OpenRange();
    if (stamps.count != 0) {
        list_->ResolveQueryData(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stamps.first,
                                stamps.count, timestampReadback_.Get(),
                                uint64{stamps.first} * sizeof(uint64));
    }

    recording_ = false;
    allocators_[recordingAllocator_].inUse = false;
    // 이 프레임 전에 쌓인 업로드(생성 초기 데이터·내용 갱신)를 먼저 실행한다.
    auto uploads = FlushUploadsLocked();
    if (!uploads) {
        allocators_[recordingAllocator_].fenceValue = fenceValue_;
        AssignPendingRetires(fenceValue_);
        timestamps_.Close(fenceValue_, false);
        (void)list_->Close();
        SettleTracking(false);
        return Result<GpuFencePoint>::Failure(uploads.Error());
    }

    if (FAILED(list_->Close())) {
        // 아무것도 제출하지 않았다. 이 기록만 참조하던 자원은 지금까지의 제출 뒤에 놓아도 된다.
        allocators_[recordingAllocator_].fenceValue = fenceValue_;
        AssignPendingRetires(fenceValue_);
        timestamps_.Close(fenceValue_, false);
        SettleTracking(false);
        return FailWith<GpuFencePoint>("command list close failed");
    }

    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    SettleTracking(true);

    Result<void> outcome = Result<void>::Success();
    if (!presentTarget_.IsNull()) {
        SwapChainRecord* record = swapChains_.Find(presentTarget_.slot);
        if (record != nullptr) {
            const HRESULT presented = record->swapChain->Present(record->vsync ? 1 : 0, 0);
            if (FAILED(presented)) {
                // Fail이 장치 제거를 판정해 손실 전이 때 한 번 Error로 남긴다.
                outcome = Fail("IDXGISwapChain::Present", presented);
            }
        }
    }

    auto signaled = SignalQueue();
    if (!signaled) {
        // 장치 제거 등으로 완료를 알 수 없다. 이 프레임이 쓴 자원·기록 저장소는 소멸 때까지
        // 놓지 않는다.
        allocators_[recordingAllocator_].fenceValue = RetainUntilDestructionFence;
        AssignPendingRetires(RetainUntilDestructionFence);
        timestamps_.Close(RetainUntilDestructionFence, false);
        DrainValidationMessages();
        return Result<GpuFencePoint>::Failure(signaled.Error());
    }

    const uint64 value = signaled.Value();
    allocators_[recordingAllocator_].fenceValue = value;
    AssignPendingRetires(value);
    timestamps_.Close(value, true);
    ProcessRetired();
    DrainValidationMessages();
    if (!outcome) {
        return Result<GpuFencePoint>::Failure(outcome.Error());
    }

    return Result<GpuFencePoint>::Success(timeline_.Point(value));
}

// 묶인 정점 버퍼 하나를 지금 파이프라인의 stride로 IA에 건다.
void D3D12Device::ApplyVertexBuffer(uint32 stream) noexcept {
    const BufferRecord* buffer = buffers_.Find(boundVertex_[stream].buffer.slot);
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_);
    if (buffer == nullptr || boundVertex_[stream].offset >= buffer->size) {
        return;
    }

    D3D12_VERTEX_BUFFER_VIEW view = {};
    view.BufferLocation = buffer->address + boundVertex_[stream].offset;
    view.SizeInBytes = static_cast<UINT>(buffer->size - boundVertex_[stream].offset);
    view.StrideInBytes = pipeline != nullptr ? pipeline->strides[stream] : 0u;
    list_->IASetVertexBuffers(stream, 1, &view);
}

// 묶인 파이프라인이 선언한 stream에 모두 유효한 버퍼가 있는지다.
bool D3D12Device::StreamsBound() const noexcept {
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_);
    if (pipeline == nullptr) {
        return false;
    }

    for (uint32 stream = 0; stream < MaxVertexStreams; ++stream) {
        if ((pipeline->streamMask & (1u << stream)) == 0) {
            continue;
        }

        const BufferRecord* buffer = buffers_.Find(boundVertex_[stream].buffer.slot);
        if (buffer == nullptr || boundVertex_[stream].offset >= buffer->size) {
            return false;
        }
    }

    return true;
}

Result<void> D3D12Device::BeginRecording() noexcept {
    // 이전 제출이 끝난 저장소만 다시 쓸 수 있다.
    auto picked = PickCommandAllocator();
    if (!picked) {
        return Result<void>::Failure(picked.Error());
    }

    AllocatorRecord& record = allocators_[picked.Value()];
    if (FAILED(record.allocator->Reset()) ||
        FAILED(list_->Reset(record.allocator.Get(), nullptr))) {
        return Fail("command list reset failed");
    }

    recordingAllocator_ = picked.Value();
    record.inUse = true;
    recording_ = true;
    markerDepth_ = 0;
    boundPipeline_ = {};
    for (BoundVertexBuffer& bound : boundVertex_) {
        bound = {};
    }

    boundIndex_ = {};
    for (BufferViewHandle& view : boundViews_) {
        view = {};
    }

    for (BufferViewHandle& view : boundWriteViews_) {
        view = {};
    }

    for (TextureHandle& view : boundWriteTextures_) {
        view = {};
    }

    boundTexture_ = white_.slot;

    bufferStates_.Clear();
    platform::MemoryZero(constants_, sizeof(constants_));
    stencilReference_ = 0;
    return Result<void>::Success();
}

void D3D12Device::StartPass(const PassDesc& pass) noexcept {
    // attachment 전이다(GX5): offscreen 대상은 추적한 상태에서, 스왑체인 백버퍼는 present에서
    // 렌더 타깃으로 바꾼다. 깊이는 DepthWrite(읽기 전용이면 DepthRead)다.
    for (uint32 index = 0; index < target_.colorCount; ++index) {
        TextureRecord* color = textures_.Find(target_.colors[index].slot);
        if (color != nullptr) {
            // attachment의 subresource 하나만 바꾼다(텍스처 GX2 C부).
            TrackTransition(*color, target_.colors[index].slot,
                            AttachmentSubresource(color->dimension, target_.colorMips[index],
                                                  target_.colorSlices[index]),
                            D3D12_RESOURCE_STATE_RENDER_TARGET);
        } else if (index == 0 && target_.resource != nullptr) {
            const D3D12_RESOURCE_BARRIER toTarget = TransitionBarrier(
                target_.resource, target_.restState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list_->ResourceBarrier(1, &toTarget);
        }
    }

    TextureRecord* depth = textures_.Find(target_.depth.slot);
    if (depth != nullptr) {
        TrackTransition(
            *depth, target_.depth.slot,
            AttachmentSubresource(depth->dimension, target_.depthMip, target_.depthSlice),
            target_.depthReadOnly ? D3D12_RESOURCE_STATE_DEPTH_READ
                                  : D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

    list_->OMSetRenderTargets(target_.colorCount, target_.colorCount != 0 ? target_.rtvs : nullptr,
                              FALSE, depth != nullptr ? &target_.dsv : nullptr);
    for (uint32 index = 0; index < target_.colorCount; ++index) {
        if (pass.colors[index].load == LoadAction::Clear) {
            const ClearColor& clear = pass.colors[index].clear;
            const float color[4] = {clear.r, clear.g, clear.b, clear.a};
            list_->ClearRenderTargetView(target_.rtvs[index], color, 0, nullptr);
        }
    }

    if (depth != nullptr && !target_.depthReadOnly) {
        D3D12_CLEAR_FLAGS flags = static_cast<D3D12_CLEAR_FLAGS>(0);
        if (pass.depth.depthLoad == LoadAction::Clear) {
            flags |= D3D12_CLEAR_FLAG_DEPTH;
        }

        if (pass.depth.stencilLoad == LoadAction::Clear &&
            GetPixelFormatInfo(depth->format).stencilBits != 0) {
            flags |= D3D12_CLEAR_FLAG_STENCIL;
        }

        if (flags != 0) {
            list_->ClearDepthStencilView(target_.dsv, flags, pass.depth.clearDepth,
                                         pass.depth.clearStencil, 0, nullptr);
        }
    }

    if (target_.width != 0) {
        D3D12_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(target_.width);
        viewport.Height = static_cast<float>(target_.height);
        viewport.MaxDepth = 1.0f;
        const D3D12_RECT scissor = {0, 0, static_cast<LONG>(target_.width),
                                    static_cast<LONG>(target_.height)};
        list_->RSSetViewports(1, &viewport);
        list_->RSSetScissorRects(1, &scissor);
    }

    list_->SetGraphicsRootSignature(rootSignature_.Get());
    list_->SetComputeRootSignature(computeRootSignature_.Get());
    ID3D12DescriptorHeap* heaps[] = {srvs_.Heap(), samplers_.Heap()};
    list_->SetDescriptorHeaps(2, heaps);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list_->OMSetStencilRef(0);
    const TextureRecord* white = textures_.Find(white_.slot);
    if (white != nullptr) {
        list_->SetGraphicsRootDescriptorTable(RootTexture, srvs_.Gpu(white->srv));
        list_->SetGraphicsRootDescriptorTable(RootSampler, samplers_.Gpu(white->sampler));
    }

    for (uint32 slot = 0; slot < MaxDrawBufferViews; ++slot) {
        list_->SetGraphicsRootDescriptorTable(RootBufferViews + slot, srvs_.Gpu(nullBufferSrv_));
    }

    list_->SetGraphicsRoot32BitConstants(RootConstants, ConstantCount, constants_, 0);
}

void D3D12Device::SetPipeline(PipelineHandle handle) noexcept {
    platform::LockGuard lock(gate_);
    const PipelineRecord* record = pipelines_.Find(handle.slot);
    if (record == nullptr) {
        boundPipeline_ = {};
        return;
    }

    list_->SetPipelineState(record->state.Get());
    boundPipeline_ = handle.slot;
    if (record->compute) {
        return;
    }

    list_->IASetPrimitiveTopology(record->topology);
    // 이미 묶인 정점 버퍼를 새 파이프라인의 stride로 다시 묶는다.
    for (uint32 stream = 0; stream < MaxVertexStreams; ++stream) {
        if (!boundVertex_[stream].buffer.IsNull()) {
            ApplyVertexBuffer(stream);
        }
    }
}

void D3D12Device::SetVertexBuffers(uint32 firstStream,
                                   Span<const VertexBufferBinding> buffers) noexcept {
    platform::LockGuard lock(gate_);
    for (usize index = 0; index < buffers.Size(); ++index) {
        const usize stream = firstStream + index;
        if (stream >= MaxVertexStreams) {
            return;
        }

        boundVertex_[stream] = {buffers[index].buffer, buffers[index].offset};
        ApplyVertexBuffer(static_cast<uint32>(stream));
    }
}

void D3D12Device::SetIndexBuffer(BufferHandle buffer, IndexFormat format, uint64 offset) noexcept {
    platform::LockGuard lock(gate_);
    boundIndex_ = {buffer, format, offset};
    const BufferRecord* record = buffers_.Find(buffer.slot);
    if (record == nullptr || offset >= record->size) {
        return;
    }

    D3D12_INDEX_BUFFER_VIEW view = {};
    view.BufferLocation = record->address + offset;
    view.SizeInBytes = static_cast<UINT>(record->size - offset);
    view.Format = format == IndexFormat::UInt16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    list_->IASetIndexBuffer(&view);
}

void D3D12Device::SetTexture(TextureHandle handle) noexcept {
    platform::LockGuard lock(gate_);
    TextureRecord* record = textures_.Find(handle.slot);
    SlotHandle slot = handle.slot;
    if (record == nullptr || !record->hasSrv || record->writable) {
        record = textures_.Find(white_.slot);
        slot = white_.slot;
    }

    if (record != nullptr) {
        // view는 자원 상태를 원 텍스처 기록이 추적한다(원 텍스처는 view가 남아 있는 동안 표에
        // 남는다, Retire 참고).
        TextureRecord* owner = record;
        SlotHandle ownerSlot = slot;
        if (record->view) {
            ownerSlot = record->source;
            owner = textures_.Find(ownerSlot);
        }

        // 명시 전이 없이 셰이더 읽기 상태가 아닌 텍스처를 읽으면 암묵 전이로 메운다(GX5. 렌더
        // 그래프를 거치지 않는 그리기용이다). view는 자기 mip·slice 범위만 본다(텍스처 GX2 A부).
        // 읽기 상태는 묶인 파이프라인 종류를 따른다: compute는 NON_PIXEL(SrvCompute), 그 밖은
        // PIXEL(SrvGraphics)이다. 그래서 compute 파이프라인을 먼저 묶어야 그래프가 낸 SrvCompute를
        // 다시 바꾸지 않는다(RG1 남은 것 B부).
        const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_);
        const D3D12_RESOURCE_STATES readState = pipeline != nullptr && pipeline->compute
                                                    ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                                    : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        const ResolvedSubresourceRange& range = record->viewRange;
        bool readable = true;
        for (uint32 slice = range.firstSlice;
             owner != nullptr && slice < range.firstSlice + range.sliceCount; ++slice) {
            for (uint32 mip = range.firstMip; mip < range.firstMip + range.mipCount; ++mip) {
                readable = readable && (owner->current.At(mip, slice) & readState) != 0;
            }
        }

        if (owner != nullptr && !readable && recording_) {
            TrackTransition(*owner, ownerSlot, range, ShaderReadStates);
        }

        boundTexture_ = slot;
        list_->SetGraphicsRootDescriptorTable(RootTexture, srvs_.Gpu(record->srv));
        list_->SetGraphicsRootDescriptorTable(RootSampler, samplers_.Gpu(record->sampler));
    }
}

Result<void> D3D12Device::SetDrawConstants(Span<const uint8> bytes) noexcept {
    platform::LockGuard lock(gate_);
    const PipelineRecord* pipeline = recording_ ? pipelines_.Find(boundPipeline_) : nullptr;
    if (pipeline == nullptr || bytes.Size() != pipeline->drawConstantBytes) {
        return RejectDrawConstantSize(pipeline != nullptr, bytes.Size(),
                                      pipeline != nullptr ? pipeline->drawConstantBytes : 0u);
    }

    if (bytes.Size() == 0) {
        return Result<void>::Success();
    }

    // 그래픽 root에는 바로 올리고(compute 파이프라인이 묶여 있어도 다음 그리기가 같은 값을 본다),
    // compute는 dispatch 때 같은 값을 compute root 상수로 올린다.
    platform::MemoryCopy(constants_, bytes.Data(), bytes.Size());
    list_->SetGraphicsRoot32BitConstants(
        RootConstants, static_cast<UINT>(bytes.Size() / sizeof(uint32)), constants_, 0);
    return Result<void>::Success();
}

void D3D12Device::SetBufferView(uint32 slot, BufferViewHandle handle) noexcept {
    platform::LockGuard lock(gate_);
    if (slot >= MaxDrawBufferViews) {
        return;
    }

    // 그래픽 단계에는 UAV가 없으므로 ReadWrite view와 stale handle은 slot을 비운다.
    const BufferViewRecord* view = bufferViews_.Find(handle.slot);
    const bool readable = view != nullptr && view->access == ViewAccess::Read;
    boundViews_[slot] = readable ? handle : BufferViewHandle{};
    list_->SetGraphicsRootDescriptorTable(RootBufferViews + slot,
                                          srvs_.Gpu(readable ? view->descriptor : nullBufferSrv_));
}

// 파이프라인이 읽는 버퍼 view slot이 모두 맞는 view로 채워졌는지다(DrawBufferSlotsSatisfied).
bool D3D12Device::BufferViewsBound() const noexcept {
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

void D3D12Device::Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept {
    platform::LockGuard lock(gate_);
    if (!PrepareDraw(false)) {
        ++ignoredDraws_;
        return;
    }

    // base instance는 0으로 고정한다(RenderBase 10.4절).
    list_->DrawInstanced(vertexCount, instanceCount, firstVertex, 0);
}

void D3D12Device::DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex) noexcept {
    platform::LockGuard lock(gate_);
    if (!PrepareDraw(true)) {
        ++ignoredDraws_;
        return;
    }

    // base vertex·base instance는 0으로 고정한다.
    list_->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, 0, 0);
}

Result<void> D3D12Device::CopyPassTarget(TextureHandle destination) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const TextureRecord* record = textures_.Find(destination.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    // 패스 대상은 2D mip 0 attachment여야 한다(자원 전체 복사, 텍스처 GX2 C부).
    const TextureRecord* passTexture = textures_.Find(target_.texture.slot);
    const bool wholeTarget =
        passTexture == nullptr ||
        (passTexture->dimension == TextureDimension::Texture2D && passTexture->mipLevels == 1);
    if (target_.resource == nullptr || !record->renderTarget || record->sampleCount > 1 ||
        target_.sampleCount > 1 || !wholeTarget || record->mipLevels != 1 ||
        record->dimension != TextureDimension::Texture2D || destination == target_.texture ||
        record->width != target_.width || record->height != target_.height ||
        record->format != target_.format) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 백버퍼(R8G8B8A8_UNORM)와 RenderTarget(R8G8B8A8_UNORM_SRGB)은 같은 typeless 묶음이라
    // CopyResource가 byte를 그대로 옮긴다. 원본(패스 대상)과 복사 대상은 지금(기록 중) 추적
    // 상태에서 바꿨다가 되돌린다(상태 중립). 이미 복사 상태면 barrier를 내지 않는다.
    const TextureRecord* source = textures_.Find(target_.texture.slot);
    const D3D12_RESOURCE_STATES sourceState =
        source != nullptr ? source->current[0] : D3D12_RESOURCE_STATE_RENDER_TARGET;
    const D3D12_RESOURCE_STATES resting = record->current[0];
    D3D12_RESOURCE_BARRIER toCopy[2] = {};
    D3D12_RESOURCE_BARRIER back[2] = {};
    uint32 count = 0;
    if (sourceState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        toCopy[count] = TransitionSubresource(target_.resource, 0, sourceState,
                                              D3D12_RESOURCE_STATE_COPY_SOURCE);
        back[count] = TransitionSubresource(target_.resource, 0, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                            sourceState);
        ++count;
    }

    if (resting != D3D12_RESOURCE_STATE_COPY_DEST) {
        toCopy[count] = TransitionSubresource(record->resource.Get(), 0, resting,
                                              D3D12_RESOURCE_STATE_COPY_DEST);
        back[count] = TransitionSubresource(record->resource.Get(), 0,
                                            D3D12_RESOURCE_STATE_COPY_DEST, resting);
        ++count;
    }

    if (count != 0) {
        list_->ResourceBarrier(count, toCopy);
    }

    list_->CopyResource(record->resource.Get(), target_.resource);
    if (count != 0) {
        list_->ResourceBarrier(count, back);
    }

    return Result<void>::Success();
}

// 명시 전이를 resource barrier로 낮춘다(GX5, Device.h Transition 주석).
void D3D12Device::Transition(Span<const ResourceTransition> transitions) noexcept {
    platform::LockGuard lock(gate_);
    TransitionLocked(transitions);
}

SplitTransition D3D12Device::BeginTransitions(Span<const ResourceTransition> transitions) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return {};
    }

    return splits_.Begin(transitions);
}

void D3D12Device::EndTransitions(SplitTransition split,
                                 Span<const ResourceTransition> transitions) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return;
    }

    const SplitEndResult ended = splits_.End(split, transitions);
    if (ended == SplitEndResult::Unknown) {
        ++ignoredDraws_;
    } else if (ended == SplitEndResult::Mismatch && validation_) {
        ++validationErrors_;
        auto failed = Fail("split transition ended with a list different from its begin");
        (void)failed;
    }

    TransitionLocked(transitions);
}

void D3D12Device::TransitionLocked(Span<const ResourceTransition> transitions) noexcept {
    if (!recording_) {
        return;
    }

    for (const ResourceTransition& transition : transitions) {
        if (!CanCombineAccess(transition.after)) {
            ++ignoredDraws_;
            continue;
        }

        if (!transition.buffer.IsNull()) {
            LowerBufferTransition(transition);
            continue;
        }

        // view의 전이는 자원 상태를 추적하는 원 텍스처 기록에 낸다.
        SlotHandle slot = transition.texture.slot;
        TextureRecord* record = textures_.Find(slot);
        if (record != nullptr && record->view) {
            slot = record->source;
            record = textures_.Find(slot);
        }

        const ResolvedSubresourceRange range =
            record == nullptr
                ? ResolvedSubresourceRange{}
                : ResolveSubresourceRange(transition.range, record->mipLevels, record->slices);
        if (range.IsEmpty()) {
            ++ignoredDraws_;
            continue;
        }

        // 내용 버림은 지금 낮출 것이 없다(aliasing은 RG5·GX12).
        if (transition.after == ResourceAccess::Discard) {
            continue;
        }

        if (validation_ && transition.before != ResourceAccess::Unknown &&
            transition.before != ResourceAccess::Discard) {
            const D3D12_RESOURCE_STATES declared = StatesFor(transition.before);
            bool matched = true;
            for (uint32 slice = range.firstSlice;
                 matched && slice < range.firstSlice + range.sliceCount; ++slice) {
                for (uint32 mip = range.firstMip; mip < range.firstMip + range.mipCount; ++mip) {
                    if (record->current.At(mip, slice) != declared) {
                        matched = false;
                        break;
                    }
                }
            }
            if (!matched) {
                ++validationErrors_;
                auto failed = Fail("transition before-state differs from the tracked state");
                (void)failed;
            }
        }

        const D3D12_RESOURCE_STATES after = StatesFor(transition.after);
        const bool uav = HasAnyAccess(transition.after, UavAccessMask);
        TrackTransition(*record, slot, range, after, uav);
    }
}

// 버퍼 전이다. 기록 중 추적 상태(RequireBufferState)로 COMMON 암묵 승격·읽기 누적·barrier를 고르고,
// UAV → UAV는 UAV barrier다(GX5).
void D3D12Device::LowerBufferTransition(const ResourceTransition& transition) noexcept {
    const BufferRecord* record = buffers_.Find(transition.buffer.slot);
    if (record == nullptr) {
        ++ignoredDraws_;
        return;
    }

    RequireBufferState(record->resource.Get(), StatesFor(transition.after));
}

// 기록 중 프레임에서 텍스처 subresource 범위(mip × slice)를 after로 바꾼다(잠금 안). 바뀌는
// subresource만 barrier를 내고, 전체 범위가 같은 상태에서 바뀌면 한 barrier(ALL_SUBRESOURCES)로
// 낸다. uavOrder면 이미 같은 UAV 상태인 범위에 UAV barrier를 낸다. barrier는 묶음 칸이 차면 나눠
// 낸다(배열 slice 수에 상한이 없다).
// 패스 끝에 MSAA 색 attachment를 resolve 대상에 박스 평균으로 resolve한다(ColorAttachment::
// resolveTarget, 결정 R17). 원본은 RESOLVE_SOURCE, 대상은 RESOLVE_DEST로 바꿔 resolve하고 대상은
// 셰이더 읽기 상태로 둔다(잠금 안).
void D3D12Device::ResolveAttachments() noexcept {
    for (uint32 index = 0; index < target_.colorCount; ++index) {
        if (target_.resolves[index].IsNull()) {
            continue;
        }

        TextureRecord* source = textures_.Find(target_.colors[index].slot);
        TextureRecord* target = textures_.Find(target_.resolves[index].slot);
        if (source == nullptr || target == nullptr) {
            continue;
        }

        // MSAA 원본은 mip 1이라 그 attachment 층(2D는 0)이 subresource이고, 대상은 resolve
        // mip·slice 하나다.
        const uint32 sourceSlice = target_.colorSlices[index];
        const ResolvedSubresourceRange whole = {0, 1, sourceSlice, 1};
        const ResolvedSubresourceRange destination = AttachmentSubresource(
            target->dimension, target_.resolveMips[index], target_.resolveSlices[index]);
        TrackTransition(*source, target_.colors[index].slot, whole,
                        D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        TrackTransition(*target, target_.resolves[index].slot, destination,
                        D3D12_RESOURCE_STATE_RESOLVE_DEST);
        list_->ResolveSubresource(
            target->resource.Get(),
            d3d12::SubresourceIndex(destination.firstMip, destination.firstSlice, 0,
                                    target->mipLevels, target->slices),
            source->resource.Get(), d3d12::SubresourceIndex(0, sourceSlice, 0, 1, source->slices),
            d3d12::MappingOf(source->format).rtv);
        TrackTransition(*target, target_.resolves[index].slot, destination, ShaderReadStates);
    }
}

void D3D12Device::TrackTransition(TextureRecord& record, SlotHandle slot,
                                  const ResolvedSubresourceRange& range,
                                  D3D12_RESOURCE_STATES after, bool uavOrder) noexcept {
    const D3D12_RESOURCE_STATES firstState = record.current.At(range.firstMip, range.firstSlice);
    bool uniform = range.mipCount == record.mipLevels && range.sliceCount == record.slices;
    for (uint32 index = 0; index < record.current.Count() && uniform; ++index) {
        uniform = record.current[index] == firstState;
    }

    constexpr uint32 MaxPlanes = 2;
    constexpr uint32 BatchCapacity = 32;
    D3D12_RESOURCE_BARRIER barriers[BatchCapacity] = {};
    uint32 used = 0;
    bool emitted = false;
    auto flush = [&]() noexcept {
        if (used != 0) {
            list_->ResourceBarrier(used, barriers);
            used = 0;
            emitted = true;
        }
    };
    if (uniform) {
        if (firstState != after) {
            barriers[used++] = TransitionBarrier(record.resource.Get(), firstState, after);
        }
    } else {
        for (uint32 slice = range.firstSlice; slice < range.firstSlice + range.sliceCount;
             ++slice) {
            for (uint32 mip = range.firstMip; mip < range.firstMip + range.mipCount; ++mip) {
                const D3D12_RESOURCE_STATES before = record.current.At(mip, slice);
                for (uint32 plane = 0; plane < record.devicePlanes && plane < MaxPlanes; ++plane) {
                    if (before == after) {
                        continue;
                    }
                    if (used == BatchCapacity) {
                        flush();
                    }
                    barriers[used++] = TransitionSubresource(
                        record.resource.Get(),
                        d3d12::SubresourceIndex(mip, slice, plane, record.mipLevels, record.slices),
                        before, after);
                }
            }
        }
    }

    if (used == 0 && !emitted && uavOrder) {
        barriers[used++] = UavBarrier(record.resource.Get());
    }

    flush();
    for (uint32 slice = range.firstSlice; slice < range.firstSlice + range.sliceCount; ++slice) {
        for (uint32 mip = range.firstMip; mip < range.firstMip + range.mipCount; ++mip) {
            record.current.At(mip, slice) = after;
        }
    }

    if (!record.touched) {
        record.touched = true;
        if (!touchedTextures_.TryPushBack(slot)) {
            touchedOverflow_ = true;
        }
    }
}

// 프레임 기록을 끝낸다(잠금 안). 제출했으면 바뀐 상태를 committed로 삼고, 제출하지 못했으면
// current를 committed로 되돌린다.
void D3D12Device::SettleTracking(bool executed) noexcept {
    auto settle = [executed](TextureRecord& record) noexcept {
        if (executed) {
            record.committed.CopyFrom(record.current);
        } else {
            record.current.CopyFrom(record.committed);
        }

        record.touched = false;
    };
    if (touchedOverflow_) {
        for (TextureRecord& record : textures_) {
            if (record.touched) {
                settle(record);
            }
        }
    } else {
        for (const SlotHandle slot : touchedTextures_) {
            TextureRecord* record = textures_.Find(slot);
            if (record != nullptr) {
                settle(*record);
            }
        }
    }

    touchedTextures_.Clear();
    touchedOverflow_ = false;
}

} // namespace rocks::graphics::d3d12
