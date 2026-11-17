#include "RecordingDevice.h"

#include <Platform/Clock.h>
#include <Platform/Fatal.h>

namespace rocks::render::testing {

using namespace rocks::graphics;

RecordingDevice::RecordingDevice(Allocator& allocator) noexcept
    : timeline_(allocator, NewDeviceEpoch(), QueueKind::Graphics), buffers_(allocator),
      textures_(allocator), pipelines_(allocator), swapChains_(allocator), retired_(allocator),
      readbacks_(allocator),
      staging_(allocator, stagingSource_, graphics::DefaultStagingBudgetBytes),
      timestamps_(allocator), timestampPool_(allocator) {
    stagingSource_.allocator = &allocator;
    tickSource_.pool = &timestampPool_;
}

TimestampReadState RecordingDevice::TickSource::ReadTimestamps(uint32 first, uint32 count,
                                                               uint64* ticks) noexcept {
    for (uint32 index = 0; index < count; ++index) {
        ticks[index] = (*pool)[first + index];
    }

    return TimestampReadState::Ready;
}

Result<StagingMemory> RecordingDevice::CpuStaging::CreateStaging(uint64 bytes) noexcept {
    auto memory = allocator->Allocate(static_cast<usize>(bytes), 16);
    if (!memory) {
        return Result<StagingMemory>::Failure(memory.Error());
    }

    StagingMemory staging;
    staging.resource = memory.Value();
    staging.data = static_cast<uint8*>(memory.Value());
    staging.size = bytes;
    return Result<StagingMemory>::Success(staging);
}

void RecordingDevice::CpuStaging::DestroyStaging(const StagingMemory& memory) noexcept {
    (void)allocator->Deallocate(memory.resource);
}

// 제출 하나를 만든다(시험이 완료를 미루면 Complete까지 끝나지 않는다).
uint64 RecordingDevice::SubmitInternal() noexcept {
    const uint64 value = ++fenceValue_;
    timeline_.Submitted(value, nullptr);
    bool hold = false;
    {
        platform::LockGuard lock(gate_);
        hold = hold_;
    }

    if (!hold) {
        timeline_.Publish(value);
    }

    return value;
}

Result<void> RecordingDevice::UpdateBuffer(BufferHandle buffer, uint64 offset,
                                           Span<const uint8> data) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return owner;
    }

    if (!buffers_.IsValid(buffer.slot)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    Record(Op::UpdateBuffer, offset + data.Size());
    return Result<void>::Success();
}

Result<void> RecordingDevice::UpdateTexture(TextureHandle texture, uint32 mipLevel,
                                            Span<const uint8> pixels) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return owner;
    }

    if (!textures_.IsValid(texture.slot)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    Record(Op::UpdateTexture, (static_cast<uint64>(mipLevel) << 32) | pixels.Size());
    return Result<void>::Success();
}

Result<GpuFencePoint> RecordingDevice::SubmitUploads() noexcept {
    return Result<GpuFencePoint>::Success(GpuFencePoint{});
}

UploadStats RecordingDevice::UploadStatistics() const noexcept {
    return UploadStats{};
}

Result<StagingBuffer> RecordingDevice::AcquireStaging(uint64 bytes) noexcept {
    return staging_.Acquire(bytes);
}

Result<void> RecordingDevice::UploadTextureFromStaging(TextureHandle texture, uint32 mipLevel,
                                                       StagingHandle staging,
                                                       uint64 offset) noexcept {
    (void)offset;
    if (!textures_.IsValid(texture.slot) || !staging_.Find(staging)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    auto marked = staging_.MarkPending(staging);
    Record(Op::UploadFromStaging, mipLevel);
    return marked;
}

Result<void> RecordingDevice::UploadBufferFromStaging(BufferHandle buffer, uint64 destinationOffset,
                                                      StagingHandle staging, uint64 sourceOffset,
                                                      uint64 size) noexcept {
    (void)destinationOffset;
    (void)sourceOffset;
    if (!buffers_.IsValid(buffer.slot) || !staging_.Find(staging)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    auto marked = staging_.MarkPending(staging);
    Record(Op::UploadFromStaging, size);
    return marked;
}

void RecordingDevice::ReleaseStaging(StagingHandle staging) noexcept {
    staging_.Release(staging);
}

StagingStats RecordingDevice::StagingStatistics() const noexcept {
    return staging_.Stats();
}

Result<ReadbackRequest> RecordingDevice::RequestReadback(TextureHandle target) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<ReadbackRequest>::Failure(owner.Error());
    }

    if (!textures_.IsValid(target.slot) || recording_) {
        return Result<ReadbackRequest>::Failure(recording_ ? ErrorCode::Busy
                                                           : ErrorCode::StaleHandle);
    }

    // 앞서 기록한 staging 업로드도 이 제출의 fence 뒤에 돌아간다.
    Record(Op::RequestReadback);
    const uint64 value = SubmitInternal();
    staging_.AssignPending(value);
    Readback readback;
    readback.fenceValue = value;
    readback.bytes = 16;
    auto added = readbacks_.TryAdd(readback);
    if (!added) {
        return Result<ReadbackRequest>::Failure(added.Error());
    }

    ReadbackRequest request;
    request.handle = ReadbackHandle{added.Value()};
    request.completion = timeline_.Point(value);
    request.bytes = readback.bytes;
    return Result<ReadbackRequest>::Success(request);
}

Result<bool> RecordingDevice::TryReadback(ReadbackHandle readback, Span<uint8> out) noexcept {
    const Readback* record = readbacks_.Find(readback.slot);
    if (record == nullptr) {
        return Result<bool>::Failure(ErrorCode::StaleHandle);
    }

    if (out.Size() != record->bytes) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    auto complete = timeline_.IsComplete(timeline_.Point(record->fenceValue));
    if (!complete) {
        return Result<bool>::Failure(complete.Error());
    }

    if (!complete.Value()) {
        return Result<bool>::Success(false);
    }

    for (uint8& byte : out) {
        byte = readbackFill_;
    }

    return Result<bool>::Success(true);
}

void RecordingDevice::ReleaseReadback(ReadbackHandle readback) noexcept {
    auto removed = readbacks_.TryRemove(readback.slot);
    (void)removed;
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
    info.timestamps = timestampsSupported_;
    info.timestampFrequency = timestampsSupported_ ? TimestampFrequency : 0;
    info.clockCalibration = timestampsSupported_;
    info.debugMarkers = true;
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

    Record(Op::CreatePipeline,
           desc.layout.streams.Size() != 0 ? desc.layout.streams[0].stride : 0u);
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

    while (markerDepth_ != 0) {
        PopMarker();
    }

    timestamps_.Close(value, true);
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
    staging_.Reclaim(completed);
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

void RecordingDevice::SetVertexBuffers(uint32 firstStream,
                                       Span<const VertexBufferBinding> buffers) noexcept {
    uint64 valid = 0;
    for (const VertexBufferBinding& binding : buffers) {
        valid += buffers_.IsValid(binding.buffer.slot) ? 1u : 0u;
    }

    Record(Op::SetVertexBuffers, (static_cast<uint64>(firstStream) << 32) | valid);
}

void RecordingDevice::SetIndexBuffer(BufferHandle buffer, IndexFormat format,
                                     uint64 offset) noexcept {
    (void)offset;
    Record(Op::SetIndexBuffer,
           (buffers_.IsValid(buffer.slot) ? 2u : 0u) | (format == IndexFormat::UInt32 ? 1u : 0u));
}

void RecordingDevice::SetTexture(TextureHandle texture) noexcept {
    Record(Op::SetTexture, textures_.IsValid(texture.slot) ? 1u : 0u);
}

void RecordingDevice::SetConstants(const DrawConstants& constants) noexcept {
    Record(Op::SetConstants, static_cast<uint64>(constants.color[0] * 255.0f));
}

void RecordingDevice::Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept {
    Record(Op::Draw, (static_cast<uint64>(vertexCount) << 40) |
                         (static_cast<uint64>(instanceCount) << 20) | firstVertex);
}

void RecordingDevice::DrawIndexed(uint32 indexCount, uint32 instanceCount,
                                  uint32 firstIndex) noexcept {
    Record(Op::DrawIndexed, (static_cast<uint64>(indexCount) << 40) |
                                (static_cast<uint64>(instanceCount) << 20) | firstIndex);
}

Result<void> RecordingDevice::CopyPassTarget(TextureHandle destination) noexcept {
    if (!recording_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!textures_.IsValid(destination.slot)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    Record(Op::CopyPassTarget, destination.slot.index);
    return Result<void>::Success();
}

Result<uint64> RecordingDevice::WriteTimestamp() noexcept {
    if (!timestampsSupported_) {
        return Result<uint64>::Failure(ErrorCode::Unsupported);
    }

    if (timestampPool_.IsEmpty()) {
        auto reset = timestamps_.Reset(TimestampFrameSlots, MaxTimestampsPerFrame);
        if (!reset) {
            return Result<uint64>::Failure(reset.Error());
        }

        auto sized = timestampPool_.TryResize(static_cast<usize>(TimestampFrameSlots) *
                                              MaxTimestampsPerFrame);
        if (!sized) {
            return Result<uint64>::Failure(sized.Error());
        }
    }

    timestamps_.Harvest(timeline_.Completed(), tickSource_);
    auto write = timestamps_.Allocate();
    if (!write) {
        return Result<uint64>::Failure(write.Error());
    }

    tickClock_ += TimestampTickStep;
    timestampPool_[write.Value().index] = tickClock_;
    Record(Op::WriteTimestamp, write.Value().query);
    return Result<uint64>::Success(write.Value().query);
}

void RecordingDevice::PushMarker(StringView name) noexcept {
    ++markerDepth_;
    Record(Op::PushMarker, name.ByteCount());
}

void RecordingDevice::PopMarker() noexcept {
    if (markerDepth_ == 0) {
        return;
    }

    --markerDepth_;
    Record(Op::PopMarker, markerDepth_);
}

usize RecordingDevice::CollectTimestamps(Span<TimestampSample> out) noexcept {
    if (!timestampsSupported_ || !ownership_.IsOwner() || timestampPool_.IsEmpty()) {
        return 0;
    }

    timestamps_.Harvest(timeline_.Completed(), tickSource_);
    return timestamps_.Collect(out);
}

Result<ClockCalibration> RecordingDevice::CalibrateClocks() noexcept {
    if (!timestampsSupported_) {
        return Result<ClockCalibration>::Failure(ErrorCode::Unsupported);
    }

    // 시험이 정한 쌍이 있으면 그것을, 없으면 지금 GPU 시계와 CPU tick의 쌍을 준다.
    ClockCalibration calibration;
    if (fixedCalibration_.gpuFrequency != 0) {
        calibration = fixedCalibration_;
    } else {
        calibration.gpuTicks = tickClock_;
        calibration.cpuTicks = platform::MonotonicTicks();
        calibration.cpuNanoseconds =
            TicksToNanoseconds(calibration.cpuTicks, platform::MonotonicTickFrequency());
        calibration.gpuFrequency = TimestampFrequency;
        calibration.uncertaintyNanoseconds = CalibrationUncertaintyNanoseconds;
    }

    ++calibrations_;
    return Result<ClockCalibration>::Success(calibration);
}

} // namespace rocks::render::testing
