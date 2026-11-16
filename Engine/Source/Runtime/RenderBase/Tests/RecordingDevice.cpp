#include "RecordingDevice.h"

#include <Platform/Fatal.h>

namespace rocks::render::testing {

using namespace rocks::graphics;

RecordingDevice::RecordingDevice(Allocator& allocator) noexcept
    : timeline_(allocator, NewDeviceEpoch(), QueueKind::Graphics), buffers_(allocator),
      textures_(allocator), pipelines_(allocator), swapChains_(allocator), retired_(allocator) {
}

RecordingDevice::~RecordingDevice() {
    timeline_.Close();
}

void RecordingDevice::HoldCompletions(bool hold) noexcept {
    platform::LockGuard lock(gate_);
    hold_ = hold;
}

void RecordingDevice::Complete(uint64 value) noexcept {
    timeline_.Publish(value);
}

void RecordingDevice::CompleteAll() noexcept {
    timeline_.Publish(Submitted());
}

void RecordingDevice::InjectLost() noexcept {
    timeline_.MarkLost();
}

void RecordingDevice::GateAcquire() noexcept {
    acquireOpen_.Reset();
}

void RecordingDevice::OpenAcquire() noexcept {
    acquireOpen_.Signal();
}

uint64 RecordingDevice::Submitted() const noexcept {
    return timeline_.LastSubmitted().value;
}

void RecordingDevice::Record(Op op, uint64 value) noexcept {
    if (callCount_ == MaxCalls) {
        platform::Fatal("RecordingDevice call log overflow");
    }

    calls_[callCount_++] = {op, value};
}

DeviceInfo RecordingDevice::Info() const noexcept {
    DeviceInfo info;
    info.backend = Backend::D3D12;
    info.deviceEpoch = timeline_.DeviceEpoch();
    return info;
}

Result<SwapChainHandle> RecordingDevice::CreateSwapChain(const SwapChainDesc& desc) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<SwapChainHandle>::Failure(owner.Error());
    }

    auto added = swapChains_.TryAdd(uint8{0});
    if (!added) {
        return Result<SwapChainHandle>::Failure(added.Error());
    }

    Record(Op::CreateSwapChain, desc.width);
    return Result<SwapChainHandle>::Success(SwapChainHandle{added.Value()});
}

Result<void> RecordingDevice::ResizeSwapChain(SwapChainHandle swapChain, uint32 width,
                                              uint32 height) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        ++failedResizes_;
        return owner;
    }

    if (!swapChains_.IsValid(swapChain.slot)) {
        ++failedResizes_;
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    Record(Op::ResizeSwapChain, (static_cast<uint64>(width) << 32) | height);
    return Result<void>::Success();
}

Result<BufferHandle> RecordingDevice::CreateBuffer(const BufferDesc& desc,
                                                   Span<const uint8> initial) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<BufferHandle>::Failure(owner.Error());
    }

    if (desc.size == 0 || initial.Size() != desc.size) {
        return Result<BufferHandle>::Failure(ErrorCode::InvalidArgument);
    }

    auto added = buffers_.TryAdd(uint8{0});
    if (!added) {
        return Result<BufferHandle>::Failure(added.Error());
    }

    Record(Op::CreateBuffer, desc.size);
    return Result<BufferHandle>::Success(BufferHandle{added.Value()});
}

Result<TextureHandle> RecordingDevice::CreateTexture(const TextureDesc& desc,
                                                     Span<const uint8> pixels) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<TextureHandle>::Failure(owner.Error());
    }

    (void)pixels;
    auto added = textures_.TryAdd(uint8{0});
    if (!added) {
        return Result<TextureHandle>::Failure(added.Error());
    }

    Record(Op::CreateTexture, (static_cast<uint64>(desc.width) << 32) | desc.height);
    return Result<TextureHandle>::Success(TextureHandle{added.Value()});
}

Result<PipelineHandle> RecordingDevice::CreatePipeline(const PipelineDesc& desc) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<PipelineHandle>::Failure(owner.Error());
    }

    auto added = pipelines_.TryAdd(uint8{0});
    if (!added) {
        return Result<PipelineHandle>::Failure(added.Error());
    }

    Record(Op::CreatePipeline, desc.vertexStride);
    return Result<PipelineHandle>::Success(PipelineHandle{added.Value()});
}

Result<CommandContext*> RecordingDevice::BeginFrame(SwapChainHandle swapChain,
                                                    const ClearColor& clear) noexcept {
    (void)clear;
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<CommandContext*>::Failure(owner.Error());
    }

    if (timeline_.IsLost()) {
        return Result<CommandContext*>::Failure(ErrorCode::Destroyed);
    }

    if (recording_ || !swapChains_.IsValid(swapChain.slot)) {
        return Result<CommandContext*>::Failure(recording_ ? ErrorCode::Busy
                                                           : ErrorCode::StaleHandle);
    }

    recording_ = true;
    Record(Op::BeginFrame);
    return Result<CommandContext*>::Success(this);
}

Result<CommandContext*> RecordingDevice::BeginOffscreen(TextureHandle target,
                                                        const ClearColor& clear) noexcept {
    (void)clear;
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<CommandContext*>::Failure(owner.Error());
    }

    if (timeline_.IsLost()) {
        return Result<CommandContext*>::Failure(ErrorCode::Destroyed);
    }

    if (recording_ || !textures_.IsValid(target.slot)) {
        return Result<CommandContext*>::Failure(recording_ ? ErrorCode::Busy
                                                           : ErrorCode::StaleHandle);
    }

    recording_ = true;
    Record(Op::BeginOffscreen);
    return Result<CommandContext*>::Success(this);
}

Result<GpuFencePoint> RecordingDevice::EndFrame() noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<GpuFencePoint>::Failure(owner.Error());
    }

    if (!recording_) {
        return Result<GpuFencePoint>::Failure(ErrorCode::InvalidArgument);
    }

    recording_ = false;
    const uint64 value = ++fenceValue_;
    for (Retired& entry : retired_) {
        if (entry.pending) {
            entry.pending = false;
            entry.fenceValue = value;
        }
    }

    timeline_.Submitted(value, nullptr);
    Record(Op::EndFrame, value);
    bool hold = false;
    {
        platform::LockGuard lock(gate_);
        hold = hold_;
    }

    if (!hold) {
        timeline_.Publish(value);
    }

    if (timeline_.IsLost()) {
        return Result<GpuFencePoint>::Failure(ErrorCode::Destroyed);
    }

    return Result<GpuFencePoint>::Success(timeline_.Point(value));
}

Result<void> RecordingDevice::ReadPixels(TextureHandle target, Span<uint8> out) noexcept {
    (void)target;
    (void)out;
    return Result<void>::Failure(ErrorCode::Unsupported);
}

void RecordingDevice::Defer() noexcept {
    Retired entry;
    entry.fenceValue = fenceValue_;
    entry.pending = recording_;
    auto pushed = retired_.TryPushBack(entry);
    if (!pushed) {
        platform::Fatal("RecordingDevice retire list allocation failed");
    }
}

void RecordingDevice::Retire(BufferHandle buffer) noexcept {
    if (!ownership_.Check() || !buffers_.TryRemove(buffer.slot)) {
        return;
    }

    Defer();
    Record(Op::RetireBuffer);
}

void RecordingDevice::Retire(TextureHandle texture) noexcept {
    if (!ownership_.Check() || !textures_.TryRemove(texture.slot)) {
        return;
    }

    Defer();
    Record(Op::RetireTexture);
}

void RecordingDevice::Retire(PipelineHandle pipeline) noexcept {
    if (!ownership_.Check() || !pipelines_.TryRemove(pipeline.slot)) {
        return;
    }

    Defer();
    Record(Op::RetirePipeline);
}

void RecordingDevice::Retire(SwapChainHandle swapChain) noexcept {
    if (!ownership_.Check() || !swapChains_.TryRemove(swapChain.slot)) {
        return;
    }

    Defer();
    Record(Op::RetireSwapChain);
}

void RecordingDevice::ReclaimCompleted() noexcept {
    if (!ownership_.Check()) {
        return;
    }

    const uint64 completed = timeline_.Completed();
    for (usize index = retired_.Size(); index > 0; --index) {
        const Retired& entry = retired_[index - 1];
        if (!entry.pending && entry.fenceValue <= completed) {
            auto removed = retired_.TryRemoveAtSwap(index - 1);
            (void)removed;
            ++reclaimed_;
        }
    }
}

Result<void> RecordingDevice::WaitIdle() noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return owner;
    }

    Record(Op::WaitIdle);
    // 실제 장치처럼 제출한 모든 작업이 끝날 때까지 기다린다. 시험이 완료를 미루면 그 게시를
    // 기다린다.
    auto waited = timeline_.Wait(timeline_.LastSubmitted());
    ReclaimCompleted();
    if (!waited) {
        return Result<void>::Failure(waited.Error());
    }

    return Result<void>::Success();
}

GpuFencePoint RecordingDevice::LastSubmittedFence() const noexcept {
    return timeline_.LastSubmitted();
}

Result<bool> RecordingDevice::IsFenceComplete(GpuFencePoint point) const noexcept {
    return timeline_.IsComplete(point);
}

Result<platform::WaitResult> RecordingDevice::WaitFence(GpuFencePoint point,
                                                        uint32 timeoutMs) noexcept {
    return timeline_.Wait(point, timeoutMs);
}

Result<void> RecordingDevice::NotifyFence(GpuFencePoint point, FenceCallback callback,
                                          void* context) noexcept {
    return timeline_.Notify(point, callback, context);
}

bool RecordingDevice::IsLost() const noexcept {
    return timeline_.IsLost();
}

bool RecordingDevice::IsOwnerThread() const noexcept {
    return ownership_.IsOwner();
}

Result<void> RecordingDevice::ReleaseOwnership() noexcept {
    if (!ownership_.IsOwner()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (recording_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    return ownership_.Release();
}

Result<void> RecordingDevice::AcquireOwnership() noexcept {
    acquireOpen_.Wait();
    auto acquired = ownership_.Acquire();
    if (acquired) {
        ++ownershipTransfers_;
    }

    return acquired;
}

uint64 RecordingDevice::RejectedCalls() const noexcept {
    return ownership_.Rejected();
}

StringView RecordingDevice::LastError() const noexcept {
    return {};
}

uint64 RecordingDevice::IgnoredDraws() const noexcept {
    return 0;
}

uint64 RecordingDevice::ValidationErrorCount() const noexcept {
    return 0;
}

usize RecordingDevice::PendingRetireCount() const noexcept {
    return retired_.Size();
}

void RecordingDevice::SetPipeline(PipelineHandle pipeline) noexcept {
    Record(Op::SetPipeline, pipelines_.IsValid(pipeline.slot) ? 1u : 0u);
}

void RecordingDevice::SetVertexBuffer(BufferHandle buffer) noexcept {
    Record(Op::SetVertexBuffer, buffers_.IsValid(buffer.slot) ? 1u : 0u);
}

void RecordingDevice::SetTexture(TextureHandle texture) noexcept {
    Record(Op::SetTexture, textures_.IsValid(texture.slot) ? 1u : 0u);
}

void RecordingDevice::SetConstants(const DrawConstants& constants) noexcept {
    Record(Op::SetConstants, static_cast<uint64>(constants.color[0] * 255.0f));
}

void RecordingDevice::Draw(uint32 vertexCount, uint32 firstVertex) noexcept {
    Record(Op::Draw, (static_cast<uint64>(vertexCount) << 32) | firstVertex);
}

} // namespace rocks::render::testing
