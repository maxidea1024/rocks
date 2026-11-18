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

    bufferStates_.Clear();
    constants_ = DrawConstants{};
    stencilReference_ = 0;
    return Result<void>::Success();
}

void D3D12Device::StartPass(const PassDesc& pass) noexcept {
    // attachment 전이다(GX5): offscreen 대상은 추적한 상태에서, 스왑체인 백버퍼는 present에서
    // 렌더 타깃으로 바꾼다. 깊이는 DepthWrite(읽기 전용이면 DepthRead)다.
    for (uint32 index = 0; index < target_.colorCount; ++index) {
        TextureRecord* color = textures_.Find(target_.colors[index].slot);
        if (color != nullptr) {
            TrackTransition(*color, target_.colors[index].slot, 0, 1,
                            D3D12_RESOURCE_STATE_RENDER_TARGET);
        } else if (index == 0 && target_.resource != nullptr) {
            const D3D12_RESOURCE_BARRIER toTarget = TransitionBarrier(
                target_.resource, target_.restState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list_->ResourceBarrier(1, &toTarget);
        }
    }

    TextureRecord* depth = textures_.Find(target_.depth.slot);
    if (depth != nullptr) {
        TrackTransition(*depth, target_.depth.slot, 0, 1,
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

    list_->SetGraphicsRoot32BitConstants(RootConstants, ConstantCount, &constants_, 0);
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
    if (record == nullptr || !record->hasSrv) {
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
        // 그래프를 거치지 않는 그리기용이다).
        bool readable = true;
        for (uint32 mip = 0; owner != nullptr && mip < owner->mipLevels; ++mip) {
            readable =
                readable && (owner->current[mip] & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0;
        }

        if (owner != nullptr && !readable && recording_) {
            TrackTransition(*owner, ownerSlot, 0, owner->mipLevels, ShaderReadStates);
        }

        list_->SetGraphicsRootDescriptorTable(RootTexture, srvs_.Gpu(record->srv));
        list_->SetGraphicsRootDescriptorTable(RootSampler, samplers_.Gpu(record->sampler));
    }
}

void D3D12Device::SetConstants(const DrawConstants& constants) noexcept {
    // compute는 dispatch 때 같은 값을 compute root 상수로 올린다.
    constants_ = constants;
    list_->SetGraphicsRoot32BitConstants(RootConstants, ConstantCount, &constants, 0);
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

    if (target_.resource == nullptr || !record->renderTarget || destination == target_.texture ||
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

        if (record == nullptr || transition.range.firstMip >= record->mipLevels) {
            ++ignoredDraws_;
            continue;
        }

        // 내용 버림은 지금 낮출 것이 없다(aliasing은 RG5·GX12).
        if (transition.after == ResourceAccess::Discard) {
            continue;
        }

        const uint32 first = transition.range.firstMip;
        const uint32 available = record->mipLevels - first;
        const uint32 count =
            transition.range.mipCount < available ? transition.range.mipCount : available;
        if (validation_ && transition.before != ResourceAccess::Unknown &&
            transition.before != ResourceAccess::Discard) {
            const D3D12_RESOURCE_STATES declared = StatesFor(transition.before);
            for (uint32 mip = first; mip < first + count; ++mip) {
                if (record->current[mip] != declared) {
                    ++validationErrors_;
                    auto failed = Fail("transition before-state differs from the tracked state");
                    (void)failed;
                    break;
                }
            }
        }

        const D3D12_RESOURCE_STATES after = StatesFor(transition.after);
        const bool uav = HasAnyAccess(transition.after, UavAccessMask);
        TrackTransition(*record, slot, first, count, after, uav);
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

// 기록 중 프레임에서 텍스처 mip [first, first + count)를 after로 바꾼다(잠금 안). 바뀌는 mip만
// barrier를 내고, 모든 mip이 같은 상태에서 바뀌면 한 barrier로 낸다. uavOrder면 이미 같은 UAV
// 상태인 mip에 UAV barrier를 낸다.
void D3D12Device::TrackTransition(TextureRecord& record, SlotHandle slot, uint32 first,
                                  uint32 count, D3D12_RESOURCE_STATES after,
                                  bool uavOrder) noexcept {
    bool uniform = first == 0 && count == record.mipLevels;
    for (uint32 mip = first; mip < first + count && uniform; ++mip) {
        uniform = record.current[mip] == record.current[first];
    }

    constexpr uint32 MaxPlanes = 2;
    D3D12_RESOURCE_BARRIER barriers[MaxMipLevels * MaxPlanes] = {};
    uint32 used = 0;
    if (uniform && record.current[first] != after) {
        barriers[used++] = TransitionBarrier(record.resource.Get(), record.current[first], after);
    } else {
        for (uint32 mip = first; mip < first + count; ++mip) {
            for (uint32 plane = 0; plane < record.devicePlanes && plane < MaxPlanes; ++plane) {
                if (record.current[mip] != after) {
                    barriers[used++] =
                        TransitionSubresource(record.resource.Get(),
                                              d3d12::SubresourceIndex(mip, plane, record.mipLevels),
                                              record.current[mip], after);
                }
            }
        }
    }

    if (used == 0 && uavOrder) {
        barriers[used++] = UavBarrier(record.resource.Get());
    }

    if (used != 0) {
        list_->ResourceBarrier(used, barriers);
    }

    for (uint32 mip = first; mip < first + count; ++mip) {
        record.current[mip] = after;
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
        for (uint32 mip = 0; mip < record.mipLevels; ++mip) {
            if (executed) {
                record.committed[mip] = record.current[mip];
            } else {
                record.current[mip] = record.committed[mip];
            }
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
