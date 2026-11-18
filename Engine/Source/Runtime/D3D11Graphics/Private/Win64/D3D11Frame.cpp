#include "D3D11DeviceImpl.h"

#include <Platform/Memory.h>

namespace rocks::graphics::d3d11 {

Result<CommandContext*> D3D11Device::BeginFrame(SwapChainHandle handle,
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

    Target target;
    target.rtv = record->rtv.Get();
    target.resource = record->buffer.Get();
    target.width = record->width;
    target.height = record->height;
    target.format = PixelFormat::RGBA8UnormSrgb;
    target.colorCount = 1;
    target.rtvs[0] = target.rtv;
    PassDesc pass;
    pass.colorCount = 1;
    pass.colors[0].clear = clear;
    BeginRecording(target, pass);
    presentTarget_ = handle;
    return Result<CommandContext*>::Success(this);
}

// deferred context는 FinishCommandList(FALSE) 뒤 기본 상태다. 패스 상태를 처음부터 묶는다.
void D3D11Device::BeginRecording(const Target& target, const PassDesc& pass) noexcept {
    target_ = target;
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

    stencilReference_ = 0;
    inputsDirty_ = false;
    ID3D11DeviceContext* context = deferred_.Get();
    context->OMSetRenderTargets(target_.colorCount, target_.rtvs, target_.dsv);
    for (uint32 index = 0; index < target_.colorCount; ++index) {
        if (pass.colors[index].load == LoadAction::Clear) {
            const ClearColor& clear = pass.colors[index].clear;
            const float color[4] = {clear.r, clear.g, clear.b, clear.a};
            context->ClearRenderTargetView(target_.rtvs[index], color);
        }
    }

    const TextureRecord* depth = textures_.Find(target_.depth.slot);
    if (depth != nullptr && !target_.depthReadOnly) {
        UINT flags = 0;
        if (pass.depth.depthLoad == LoadAction::Clear) {
            flags |= D3D11_CLEAR_DEPTH;
        }

        if (pass.depth.stencilLoad == LoadAction::Clear &&
            GetPixelFormatInfo(depth->format).stencilBits != 0) {
            flags |= D3D11_CLEAR_STENCIL;
        }

        if (flags != 0) {
            context->ClearDepthStencilView(target_.dsv, flags, pass.depth.clearDepth,
                                           pass.depth.clearStencil);
        }
    }

    // viewport·scissor는 대상 전체다(래스터 상태는 늘 scissor를 켠다). attachment 없는 패스는
    // 그리지 않으므로 두지 않는다.
    if (target_.width != 0) {
        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(target_.width);
        viewport.Height = static_cast<float>(target_.height);
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);
        const D3D11_RECT scissor = {0, 0, static_cast<LONG>(target_.width),
                                    static_cast<LONG>(target_.height)};
        context->RSSetScissorRects(1, &scissor);
    }

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* constants = constants_.Get();
    context->VSSetConstantBuffers(0, 1, &constants);
    context->PSSetConstantBuffers(0, 1, &constants);
    context->CSSetConstantBuffers(0, 1, &constants);
    const TextureRecord* white = textures_.Find(white_.slot);
    if (white != nullptr) {
        ID3D11ShaderResourceView* view = white->srv.Get();
        context->PSSetShaderResources(0, 1, &view);
        context->PSSetSamplers(0, 1, &white->sampler);
    }

    // deferred context의 동적 버퍼 내용은 그 기록 안에서 DISCARD로 쓰기 전까지 정해지지 않는다.
    const DrawConstants identity;
    WriteConstants(identity);
}

Result<GpuFencePoint> D3D11Device::EndFrame() noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<GpuFencePoint>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);
    if (!recording_) {
        return Result<GpuFencePoint>::Failure(ErrorCode::InvalidArgument);
    }

    // 끝나지 않은 분리 전이는 계약 위반이다(검증 장치는 오류로 센다).
    const usize unended = splits_.Close();
    if (validation_) {
        validationErrors_ += unended;
    }

    CloseMarkers();
    const TimestampRange stamps = timestamps_.OpenRange();
    recording_ = false;
    ComRef<ID3D11CommandList> commands;
    // 성공·실패와 무관하게 deferred context를 기본 상태로 되돌린다.
    const HRESULT finished = deferred_->FinishCommandList(FALSE, commands.Out());
    // 이 프레임 전에 쌓인 업로드(내용 갱신·staging 업로드)를 먼저 실행한다.
    auto uploads = FlushUploadsLocked();
    if (!uploads) {
        AssignPendingRetires(fenceValue_);
        timestamps_.Close(fenceValue_, false);
        return Result<GpuFencePoint>::Failure(uploads.Error());
    }

    if (FAILED(finished)) {
        // 아무것도 제출하지 않았다. 이 기록만 참조하던 자원은 지금까지의 제출 뒤에 놓아도 된다.
        AssignPendingRetires(fenceValue_);
        timestamps_.Close(fenceValue_, false);
        return FailWith<GpuFencePoint>("ID3D11DeviceContext::FinishCommandList", finished);
    }

    // 이 프레임의 timestamp를 slot의 disjoint query로 감싼다(D3D11 timestamp는 disjoint 구간
    // 안에서만 의미가 있다).
    ID3D11Query* disjoint = nullptr;
    if (stamps.count != 0) {
        disjoint = disjointQueries_[stamps.first / MaxTimestampsPerFrame].Get();
        immediate_->Begin(disjoint);
    }

    immediate_->ExecuteCommandList(commands.Get(), FALSE);
    if (disjoint != nullptr) {
        immediate_->End(disjoint);
    }

    commands.Reset();
    Result<void> outcome = Result<void>::Success();
    if (!presentTarget_.IsNull()) {
        SwapChainRecord* record = swapChains_.Find(presentTarget_.slot);
        if (record != nullptr) {
            const HRESULT presented = record->swapChain->Present(record->vsync ? 1 : 0, 0);
            if (FAILED(presented)) {
                outcome = Fail("IDXGISwapChain::Present", presented);
            }
        }
    }

    auto signaled = SignalQueue();
    if (!signaled) {
        // 장치 제거 등으로 완료를 알 수 없다. 이 프레임이 쓴 자원은 소멸 때까지 놓지 않는다.
        AssignPendingRetires(RetainUntilDestructionFence);
        timestamps_.Close(RetainUntilDestructionFence, false);
        DrainValidationMessages();
        return Result<GpuFencePoint>::Failure(signaled.Error());
    }

    const uint64 value = signaled.Value();
    AssignPendingRetires(value);
    timestamps_.Close(value, true);
    ProcessRetired();
    DrainValidationMessages();
    if (!outcome) {
        return Result<GpuFencePoint>::Failure(outcome.Error());
    }

    return Result<GpuFencePoint>::Success(timeline_.Point(value));
}

// --- CommandContext ---

void D3D11Device::SetPipeline(PipelineHandle handle) noexcept {
    platform::LockGuard lock(gate_);
    const PipelineRecord* record = pipelines_.Find(handle.slot);
    if (record == nullptr) {
        boundPipeline_ = {};
        return;
    }

    ID3D11DeviceContext* context = deferred_.Get();
    boundPipeline_ = handle.slot;
    if (record->compute) {
        // 그래픽 상태는 건드리지 않는다(같은 패스의 다음 그리기가 그대로 쓴다).
        context->CSSetShader(record->computeShader.Get(), nullptr, 0);
        return;
    }

    context->VSSetShader(record->vertexShader.Get(), nullptr, 0);
    context->PSSetShader(record->pixelShader.Get(), nullptr, 0);
    context->IASetInputLayout(record->inputLayout.Get());
    context->IASetPrimitiveTopology(record->topology);
    context->RSSetState(record->raster.Get());
    context->OMSetBlendState(record->blend.Get(), nullptr, D3D11_DEFAULT_SAMPLE_MASK);
    context->OMSetDepthStencilState(record->depthStencil.Get(), stencilReference_);
    // 이미 묶인 정점 버퍼를 새 파이프라인의 stride로 다시 묶는다.
    for (uint32 stream = 0; stream < MaxVertexStreams; ++stream) {
        if (!boundVertex_[stream].buffer.IsNull()) {
            ApplyVertexBuffer(stream);
        }
    }
}

void D3D11Device::SetVertexBuffers(uint32 firstStream,
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

void D3D11Device::SetIndexBuffer(BufferHandle buffer, IndexFormat format, uint64 offset) noexcept {
    platform::LockGuard lock(gate_);
    boundIndex_ = {buffer, format, offset};
    const BufferRecord* record = buffers_.Find(buffer.slot);
    if (record == nullptr || offset >= record->size) {
        return;
    }

    deferred_->IASetIndexBuffer(record->buffer.Get(),
                                format == IndexFormat::UInt16 ? DXGI_FORMAT_R16_UINT
                                                              : DXGI_FORMAT_R32_UINT,
                                static_cast<UINT>(offset));
}

void D3D11Device::SetTexture(TextureHandle handle) noexcept {
    platform::LockGuard lock(gate_);
    const TextureRecord* record = textures_.Find(handle.slot);
    if (record == nullptr || !record->srv) {
        record = textures_.Find(white_.slot);
    }

    if (record != nullptr) {
        ID3D11ShaderResourceView* view = record->srv.Get();
        deferred_->PSSetShaderResources(0, 1, &view);
        deferred_->PSSetSamplers(0, 1, &record->sampler);
    }
}

void D3D11Device::SetConstants(const DrawConstants& constants) noexcept {
    // 기록 context는 소유 스레드만 쓰므로 장치 잠금이 필요 없다(D3D12 루트 상수와 같다).
    WriteConstants(constants);
}

// 그리기 상수 버퍼를 갈아 끼운다(UE uniform buffer와 같은 WRITE_DISCARD). 앞 그리기는 앞 내용을
// 그대로 본다.
void D3D11Device::WriteConstants(const DrawConstants& constants) noexcept {
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT result = deferred_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(result)) {
        ++ignoredDraws_;
        return;
    }

    platform::MemoryCopy(mapped.pData, &constants, sizeof(constants));
    deferred_->Unmap(constants_.Get(), 0);
}

void D3D11Device::Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept {
    platform::LockGuard lock(gate_);
    if (!PrepareDraw(false)) {
        ++ignoredDraws_;
        return;
    }

    // base instance는 0으로 고정한다(RenderBase 10.4절).
    deferred_->DrawInstanced(vertexCount, instanceCount, firstVertex, 0);
}

void D3D11Device::DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex) noexcept {
    platform::LockGuard lock(gate_);
    if (!PrepareDraw(true)) {
        ++ignoredDraws_;
        return;
    }

    // base vertex·base instance는 0으로 고정한다.
    deferred_->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, 0, 0);
}

// D3D11은 드라이버가 자원 상태와 UAV hazard를 암묵으로 관리하므로 명시 전이를 낮출 곳이
// 없다(RenderBase GX5, UE D3D11 RHI도 전이가 빈 함수다).
void D3D11Device::Transition(Span<const ResourceTransition> transitions) noexcept {
    (void)transitions;
}

SplitTransition D3D11Device::BeginTransitions(Span<const ResourceTransition> transitions) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return {};
    }

    return splits_.Begin(transitions);
}

void D3D11Device::EndTransitions(SplitTransition split,
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
    }
}

Result<void> D3D11Device::CopyPassTarget(TextureHandle destination) noexcept {
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
    // CopyResource가 byte를 그대로 옮긴다. 기록 순서대로 실행되므로 앞 그리기가 모두 들어간다.
    deferred_->CopyResource(record->texture.Get(), target_.resource);
    return Result<void>::Success();
}

// 묶인 정점 버퍼 하나를 지금 파이프라인의 stride로 IA에 건다.
void D3D11Device::ApplyVertexBuffer(uint32 stream) noexcept {
    const BufferRecord* buffer = buffers_.Find(boundVertex_[stream].buffer.slot);
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_);
    if (buffer == nullptr || boundVertex_[stream].offset >= buffer->size) {
        return;
    }

    ID3D11Buffer* view = buffer->buffer.Get();
    const UINT stride = pipeline != nullptr ? pipeline->strides[stream] : 0u;
    const UINT offset = static_cast<UINT>(boundVertex_[stream].offset);
    deferred_->IASetVertexBuffers(stream, 1, &view, &stride, &offset);
}

// 묶인 파이프라인이 선언한 stream에 모두 유효한 버퍼가 있는지다.
bool D3D11Device::StreamsBound() const noexcept {
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

} // namespace rocks::graphics::d3d11
