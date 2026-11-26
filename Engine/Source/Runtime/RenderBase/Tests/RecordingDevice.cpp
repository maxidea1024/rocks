#include "RecordingDevice.h"

#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks::render::testing {

using namespace rocks::graphics;

RecordingDevice::RecordingDevice(Allocator& allocator) noexcept
    : timeline_(allocator, NewDeviceEpoch(), QueueKind::Graphics),
      computeTimeline_(allocator, timeline_.DeviceEpoch(), QueueKind::AsyncCompute),
      textureUses_(allocator), bufferUses_(allocator), heaps_(allocator),
      placedTextures_(allocator), placedBuffers_(allocator), buffers_(allocator),
      bufferStrides_(allocator), allocator_(allocator), textures_(allocator),
      transitions_(allocator), splits_(allocator), pendingSplits_(allocator), pipelines_(allocator),
      swapChains_(allocator), shaders_(allocator), bufferViews_(allocator),
      constantBuffers_(allocator), retired_(allocator), readbacks_(allocator),
      staging_(allocator, stagingSource_, graphics::DefaultStagingBudgetBytes),
      timestamps_(allocator), timestampPool_(allocator) {
    stagingSource_.allocator = &allocator;
    tickSource_.pool = &timestampPool_;
    // compute 그룹 수 한도는 D3D12와 같은 축별 65535다(SetComputeGroupLimit로 바꾼다).
    for (uint32& limit : caps_.limits.computeGroupCount) {
        limit = DefaultComputeGroupLimit;
    }
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
    computeTimeline_.Close();
}

void RecordingDevice::SetAsyncCompute(bool available, bool efficient) noexcept {
    caps_.features.asyncCompute = available;
    caps_.features.efficientAsyncCompute = available && efficient;
}

uint64 RecordingDevice::SubmittedCompute() const noexcept {
    return computeTimeline_.LastSubmitted().value;
}

void RecordingDevice::CompleteCompute(uint64 value) noexcept {
    computeTimeline_.Publish(value);
}

const FenceTimeline* RecordingDevice::TimelineOf(GpuFencePoint point) const noexcept {
    return point.queue == QueueKind::AsyncCompute ? &computeTimeline_ : &timeline_;
}

void RecordingDevice::TouchResource(Array<QueueUse>& uses, uint32 index) noexcept {
    if (!recording_) {
        return;
    }

    if (index >= uses.Size() && !uses.TryResize(static_cast<usize>(index) + 1)) {
        platform::Fatal("RecordingDevice queue use table allocation failed");
    }

    // 이 기록의 fence 값(제출하면 받을 값)이다.
    const bool compute = recordingQueue_ == QueueKind::AsyncCompute;
    const uint64 current = compute ? computeFenceValue_ + 1 : fenceValue_ + 1;
    QueueUse& use = uses[index];
    if (use.value != 0 && use.queue != recordingQueue_ &&
        waited_[static_cast<uint32>(recordingQueue_)] < use.value) {
        ++validationErrors_;
    }

    use.queue = recordingQueue_;
    use.value = current;
}

void RecordingDevice::TouchTexture(SlotHandle texture) noexcept {
    const TextureState* state = textures_.Find(texture);
    if (state == nullptr) {
        return;
    }

    const uint32 index = state->view ? state->source.index : texture.index;
    TouchResource(textureUses_, index);
    if (recording_) {
        CheckPlacedActive(placedTextures_, index);
    }
}

void RecordingDevice::TouchBuffer(SlotHandle buffer) noexcept {
    if (buffers_.IsValid(buffer)) {
        TouchResource(bufferUses_, buffer.index);
        if (recording_) {
            CheckPlacedActive(placedBuffers_, buffer.index);
        }
    }
}

// --- transient heap(GX12) ---

void RecordingDevice::SetTransientHeaps(bool available) noexcept {
    caps_.features.transientHeaps = available;
}

RecordingDevice::PlacedState* RecordingDevice::PlacedOf(Array<PlacedState>& table,
                                                        uint32 index) noexcept {
    if (index >= table.Size() || table[index].heap.IsNull()) {
        return nullptr;
    }

    return &table[index];
}

Result<void> RecordingDevice::MarkPlaced(Array<PlacedState>& table, uint32 index, SlotHandle heap,
                                         uint64 offset, uint64 bytes) noexcept {
    if (index >= table.Size()) {
        auto resized = table.TryResize(static_cast<usize>(index) + 1);
        if (!resized) {
            return resized;
        }
    }

    PlacedState placed;
    placed.heap = heap;
    placed.offset = offset;
    placed.bytes = bytes;
    table[index] = placed;
    return Result<void>::Success();
}

void RecordingDevice::AcquirePlaced(bool buffer, uint32 index) noexcept {
    PlacedState* acquired = PlacedOf(buffer ? placedBuffers_ : placedTextures_, index);
    if (acquired == nullptr) {
        return;
    }

    const auto deactivate = [acquired](Array<PlacedState>& table) noexcept {
        for (PlacedState& other : table) {
            if (&other != acquired && other.heap == acquired->heap &&
                other.offset < acquired->offset + acquired->bytes &&
                acquired->offset < other.offset + other.bytes) {
                other.active = false;
            }
        }
    };
    deactivate(placedTextures_);
    deactivate(placedBuffers_);
    acquired->active = true;
    Record(Op::Acquire, index + (buffer ? AcquireBufferBit : 0));
}

void RecordingDevice::CheckPlacedActive(Array<PlacedState>& table, uint32 index) noexcept {
    const PlacedState* placed = PlacedOf(table, index);
    if (placed != nullptr && !placed->active) {
        ++validationErrors_;
    }
}

Result<TransientHeapHandle>
RecordingDevice::CreateTransientHeap(const TransientHeapDesc& desc) noexcept {
    if (!caps_.features.transientHeaps) {
        return Result<TransientHeapHandle>::Failure(ErrorCode::Unsupported);
    }

    if (desc.bytes == 0) {
        return Result<TransientHeapHandle>::Failure(ErrorCode::InvalidArgument);
    }

    HeapState heap;
    heap.kind = desc.kind;
    heap.bytes = desc.bytes;
    auto added = heaps_.TryAdd(heap);
    if (!added) {
        return Result<TransientHeapHandle>::Failure(added.Error());
    }

    Record(Op::CreateTransientHeap, desc.bytes);
    return Result<TransientHeapHandle>::Success(TransientHeapHandle{added.Value()});
}

void RecordingDevice::RetireTransientHeap(TransientHeapHandle heap) noexcept {
    if (heaps_.TryRemove(heap.slot)) {
        Defer();
    }
}

namespace {

uint64 AlignTransient(uint64 bytes) noexcept {
    const uint64 alignment = RecordingDevice::TransientAlignment;
    return (bytes + alignment - 1) / alignment * alignment;
}

} // namespace

Result<TransientPlacement>
RecordingDevice::GetTexturePlacement(const TextureDesc& desc) const noexcept {
    if (!caps_.features.transientHeaps) {
        return Result<TransientPlacement>::Failure(ErrorCode::Unsupported);
    }

    if (desc.usage == TextureUsage::Sampled || !ValidateTextureDesc(desc)) {
        return Result<TransientPlacement>::Failure(ErrorCode::InvalidArgument);
    }

    TransientPlacement placement;
    placement.bytes = AlignTransient(TextureDataBytes(desc));
    placement.alignment = TransientAlignment;
    placement.kind = TransientHeapKindOf(desc);
    return Result<TransientPlacement>::Success(placement);
}

Result<TransientPlacement>
RecordingDevice::GetBufferPlacement(const BufferDesc& desc) const noexcept {
    if (!caps_.features.transientHeaps) {
        return Result<TransientPlacement>::Failure(ErrorCode::Unsupported);
    }

    if (!ValidateBufferDesc(desc)) {
        return Result<TransientPlacement>::Failure(ErrorCode::InvalidArgument);
    }

    TransientPlacement placement;
    placement.bytes = AlignTransient(desc.size);
    placement.alignment = TransientAlignment;
    placement.kind = TransientHeapKind::Buffers;
    return Result<TransientPlacement>::Success(placement);
}

Result<void> RecordingDevice::CheckPlacement(TransientHeapHandle heap, uint64 offset,
                                             const TransientPlacement& placement) const noexcept {
    const HeapState* state = heaps_.Find(heap.slot);
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (state->kind != placement.kind || offset % placement.alignment != 0 ||
        offset > state->bytes || state->bytes - offset < placement.bytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<TextureHandle> RecordingDevice::CreatePlacedTexture(TransientHeapHandle heap, uint64 offset,
                                                           const TextureDesc& desc) noexcept {
    auto placement = GetTexturePlacement(desc);
    if (!placement) {
        return Result<TextureHandle>::Failure(placement.Error());
    }

    auto checked = CheckPlacement(heap, offset, placement.Value());
    if (!checked) {
        return Result<TextureHandle>::Failure(checked.Error());
    }

    auto created = CreateTexture(desc, {});
    if (!created) {
        return created;
    }

    auto marked = MarkPlaced(placedTextures_, created.Value().slot.index, heap.slot, offset,
                             placement.Value().bytes);
    if (!marked) {
        return Result<TextureHandle>::Failure(marked.Error());
    }

    Record(Op::CreatePlaced, offset);
    return created;
}

Result<BufferHandle> RecordingDevice::CreatePlacedBuffer(TransientHeapHandle heap, uint64 offset,
                                                         const BufferDesc& desc) noexcept {
    auto placement = GetBufferPlacement(desc);
    if (!placement) {
        return Result<BufferHandle>::Failure(placement.Error());
    }

    auto checked = CheckPlacement(heap, offset, placement.Value());
    if (!checked) {
        return Result<BufferHandle>::Failure(checked.Error());
    }

    // placed 버퍼는 초기 데이터가 없다(CreateBuffer의 초기 데이터 크기 검사를 거치지 않는다).
    auto added = buffers_.TryAdd(ResourceAccess::VertexOrIndex);
    if (!added) {
        return Result<BufferHandle>::Failure(added.Error());
    }

    while (bufferStrides_.Size() <= added.Value().index) {
        if (!bufferStrides_.TryPushBack(0u)) {
            return Result<BufferHandle>::Failure(ErrorCode::OutOfMemory);
        }
    }

    bufferStrides_[added.Value().index] = desc.stride;
    Record(Op::CreateBuffer, desc.size);
    auto marked =
        MarkPlaced(placedBuffers_, added.Value().index, heap.slot, offset, placement.Value().bytes);
    if (!marked) {
        return Result<BufferHandle>::Failure(marked.Error());
    }

    Record(Op::CreatePlaced, offset);
    return Result<BufferHandle>::Success(BufferHandle{added.Value()});
}

bool RecordingDevice::RejectOnComputeQueue() noexcept {
    if (recording_ && recordingQueue_ == QueueKind::AsyncCompute) {
        ++validationErrors_;
        return true;
    }

    return false;
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
    computeTimeline_.Publish(SubmittedCompute());
}

void RecordingDevice::InjectLost() noexcept {
    timeline_.MarkLost();
    computeTimeline_.MarkLost();
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

    auto added = buffers_.TryAdd(ResourceAccess::VertexOrIndex);
    if (!added) {
        return Result<BufferHandle>::Failure(added.Error());
    }

    while (bufferStrides_.Size() <= added.Value().index) {
        if (!bufferStrides_.TryPushBack(0u)) {
            return Result<BufferHandle>::Failure(ErrorCode::OutOfMemory);
        }
    }

    bufferStrides_[added.Value().index] = desc.stride;
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
    if (createTextureError_ != ErrorCode::None) {
        return Result<TextureHandle>::Failure(createTextureError_);
    }

    TextureState state;
    const uint32 mipLevels =
        desc.mipLevels == 0 || desc.mipLevels > MaxTrackedMips ? 1u : desc.mipLevels;
    const uint32 slices = TextureSliceCount(desc) == 0 ? 1u : TextureSliceCount(desc);
    state.dimension = desc.dimension;
    state.format = desc.format;
    state.sampleCount = desc.sampleCount;
    state.comparison = desc.sampler.comparison;
    auto reset = state.states.Reset(allocator_, mipLevels, slices, ResourceAccess::SrvGraphics);
    if (!reset) {
        return Result<TextureHandle>::Failure(reset.Error());
    }

    auto added = textures_.TryAdd(Move(state));
    if (!added) {
        return Result<TextureHandle>::Failure(added.Error());
    }

    Record(Op::CreateTexture, (static_cast<uint64>(desc.width) << 32) | desc.height);
    return Result<TextureHandle>::Success(TextureHandle{added.Value()});
}

Result<TextureHandle> RecordingDevice::CreateTextureView(TextureHandle texture,
                                                         const TextureViewDesc& desc) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<TextureHandle>::Failure(owner.Error());
    }

    const TextureState* source = textures_.Find(texture.slot);
    if (source == nullptr || source->view) {
        return Result<TextureHandle>::Failure(ErrorCode::StaleHandle);
    }

    TextureSubresources shape;
    shape.mipLevels = source->states.MipLevels();
    shape.slices = source->states.Slices();
    // 기록 장치는 텍스처 용도를 기억하지 않으므로 쓰기 view는 Storage로 본다.
    const TextureUsage usage =
        desc.access == ViewAccess::ReadWrite ? TextureUsage::Storage : TextureUsage::Sampled;
    auto range = ResolveTextureViewRange(source->dimension, usage, shape, desc);
    if (!range) {
        return Result<TextureHandle>::Failure(range.Error());
    }

    TextureState state;
    state.dimension = TextureViewDimensionOf(source->dimension, desc);
    state.format = desc.format != PixelFormat::Unknown ? desc.format : source->format;
    state.sampleCount = source->sampleCount;
    state.comparison = source->comparison;
    state.view = true;
    state.source = texture.slot;
    state.range = range.Value();
    state.access = desc.access;
    auto added = textures_.TryAdd(Move(state));
    if (!added) {
        return Result<TextureHandle>::Failure(added.Error());
    }

    Record(Op::CreateTextureView, (static_cast<uint64>(range.Value().firstMip) << ViewOpMipShift) |
                                      range.Value().mipCount);
    return Result<TextureHandle>::Success(TextureHandle{added.Value()});
}

Result<ShaderHandle> RecordingDevice::CreateShader(const ShaderCodeDesc& desc) noexcept {
    if (desc.kind != graphics::ShaderCodeKindOf(Info().backend) || desc.code.IsEmpty()) {
        return Result<ShaderHandle>::Failure(ErrorCode::InvalidArgument);
    }

    bool hold = false;
    {
        platform::LockGuard lock(shaderGate_);
        if (shaderHold_ != 0) {
            ++shaderArrivals_;
            hold = shaderArrivals_ < shaderHold_;
            if (!hold) {
                shaderRelease_.Signal();
            }
        }
    }

    if (hold) {
        (void)shaderRelease_.Wait();
    }

    platform::LockGuard lock(shaderGate_);
    auto added = shaders_.TryAdd(static_cast<uint8>(desc.stage));
    if (!added) {
        return Result<ShaderHandle>::Failure(added.Error());
    }

    ++createdShaders_;
    return Result<ShaderHandle>::Success(ShaderHandle{added.Value()});
}

void RecordingDevice::Retire(ShaderHandle shader) noexcept {
    platform::LockGuard lock(shaderGate_);
    if (shaders_.TryRemove(shader.slot)) {
        ++retiredShaders_;
    }
}

void RecordingDevice::HoldShaderCreation(uint32 arrivals) noexcept {
    platform::LockGuard lock(shaderGate_);
    shaderHold_ = arrivals;
    shaderArrivals_ = 0;
    if (arrivals == 0) {
        shaderRelease_.Signal();
    } else {
        shaderRelease_.Reset();
    }
}

uint64 RecordingDevice::CreatedShaders() const noexcept {
    platform::LockGuard lock(const_cast<platform::Mutex&>(shaderGate_));
    return createdShaders_;
}

uint64 RecordingDevice::RetiredShaders() const noexcept {
    platform::LockGuard lock(const_cast<platform::Mutex&>(shaderGate_));
    return retiredShaders_;
}

usize RecordingDevice::LiveShaders() const noexcept {
    platform::LockGuard lock(const_cast<platform::Mutex&>(shaderGate_));
    return shaders_.Size();
}

Result<PipelineHandle> RecordingDevice::CreatePipeline(const PipelineDesc& desc) noexcept {
    ++pipelineCreateCalls_;
    lastVertexShader_ = desc.vertexShader;
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<PipelineHandle>::Failure(owner.Error());
    }

    if (!graphics::IsValidDrawConstantBytes(desc.drawConstantBytes)) {
        return Result<PipelineHandle>::Failure(ErrorCode::InvalidArgument);
    }

    auto added = pipelines_.TryAdd(desc.drawConstantBytes);
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
    recordingQueue_ = QueueKind::Graphics;
    Record(Op::BeginFrame);
    return Result<CommandContext*>::Success(this);
}

// 기록 장치는 색 attachment 하나(첫 색)만 기록한다(렌더 그래프 시험 범위).
Result<CommandContext*> RecordingDevice::BeginPass(const PassDesc& pass) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<CommandContext*>::Failure(owner.Error());
    }

    if (timeline_.IsLost()) {
        return Result<CommandContext*>::Failure(ErrorCode::Destroyed);
    }

    if (recording_) {
        return Result<CommandContext*>::Failure(ErrorCode::Busy);
    }

    // async compute 기록이다: attachment가 없어야 하고 compute queue가 있어야 한다.
    if (pass.queue == QueueKind::AsyncCompute) {
        if (!caps_.features.asyncCompute) {
            return Result<CommandContext*>::Failure(ErrorCode::Unsupported);
        }

        if (pass.colorCount != 0 || !pass.depth.texture.IsNull()) {
            return Result<CommandContext*>::Failure(ErrorCode::InvalidArgument);
        }

        recording_ = true;
        recordingQueue_ = QueueKind::AsyncCompute;
        resolveCount_ = 0;
        Record(Op::BeginCompute);
        return Result<CommandContext*>::Success(this);
    }

    // attachment는 살아 있는 텍스처여야 한다(attachment 없는 패스는 compute·복사 기록이다).
    for (uint32 index = 0; index < pass.colorCount; ++index) {
        const TextureHandle resolve = pass.colors[index].resolveTarget;
        if (!textures_.IsValid(pass.colors[index].texture.slot) ||
            (!resolve.IsNull() && !textures_.IsValid(resolve.slot))) {
            return Result<CommandContext*>::Failure(ErrorCode::StaleHandle);
        }
    }

    // resolve 대상은 패스 끝에 ResolveDestination이 된다(원본은 RenderTarget 그대로).
    resolveCount_ = 0;
    for (uint32 index = 0; index < pass.colorCount; ++index) {
        const ColorAttachment& color = pass.colors[index];
        if (!color.resolveTarget.IsNull()) {
            resolves_[resolveCount_++] = {color.resolveTarget.slot, color.resolveMipLevel,
                                          color.resolveSlice};
        }
    }

    if (!pass.depth.texture.IsNull() && !textures_.IsValid(pass.depth.texture.slot)) {
        return Result<CommandContext*>::Failure(ErrorCode::StaleHandle);
    }

    // attachment 전이다(Device.h Transition 주석): 그 mip·slice의 subresource 하나만 바꾼다.
    const auto attach = [this](TextureHandle texture, uint32 mip, uint32 slice,
                               ResourceAccess access) noexcept {
        TextureState* state = textures_.Find(texture.slot);
        const ResolvedSubresourceRange used = AttachmentSubresource(state->dimension, mip, slice);
        if (used.firstMip < state->states.MipLevels() && used.firstSlice < state->states.Slices()) {
            state->states.At(used.firstMip, used.firstSlice) = access;
        }
    };
    for (uint32 index = 0; index < pass.colorCount; ++index) {
        attach(pass.colors[index].texture, pass.colors[index].mipLevel, pass.colors[index].slice,
               ResourceAccess::RenderTarget);
    }

    if (!pass.depth.texture.IsNull()) {
        attach(pass.depth.texture, pass.depth.mipLevel, pass.depth.slice,
               pass.depth.readOnly ? ResourceAccess::DepthRead : ResourceAccess::DepthWrite);
    }

    recording_ = true;
    recordingQueue_ = QueueKind::Graphics;
    // placed attachment의 획득(GX12)이 attachment 사용보다 먼저다.
    for (uint32 index = 0; index < pass.colorCount; ++index) {
        if (pass.colors[index].acquire) {
            AcquirePlaced(false, pass.colors[index].texture.slot.index);
        }
    }

    if (!pass.depth.texture.IsNull() && pass.depth.acquire) {
        AcquirePlaced(false, pass.depth.texture.slot.index);
    }

    for (uint32 index = 0; index < pass.colorCount; ++index) {
        TouchTexture(pass.colors[index].texture.slot);
        if (!pass.colors[index].resolveTarget.IsNull()) {
            TouchTexture(pass.colors[index].resolveTarget.slot);
        }
    }

    if (!pass.depth.texture.IsNull()) {
        TouchTexture(pass.depth.texture.slot);
    }

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
    if (recordingQueue_ == QueueKind::AsyncCompute) {
        recordingQueue_ = QueueKind::Graphics;
        validationErrors_ += splits_.Close();
        pendingSplits_.Clear();
        const uint64 value = ++computeFenceValue_;
        for (Retired& entry : retired_) {
            if (entry.computePending) {
                entry.computePending = false;
                entry.computeFenceValue = value;
            }
        }

        while (markerDepth_ != 0) {
            PopMarker();
        }

        computeTimeline_.Submitted(value, nullptr);
        Record(Op::EndCompute, value);
        bool hold = false;
        {
            platform::LockGuard lock(gate_);
            hold = hold_;
        }

        if (!hold) {
            computeTimeline_.Publish(value);
        }

        if (computeTimeline_.IsLost()) {
            return Result<GpuFencePoint>::Failure(ErrorCode::Destroyed);
        }

        return Result<GpuFencePoint>::Success(computeTimeline_.Point(value));
    }

    for (uint32 index = 0; index < resolveCount_; ++index) {
        TextureState* state = textures_.Find(resolves_[index].texture);
        if (state != nullptr) {
            const ResolvedSubresourceRange used = AttachmentSubresource(
                state->dimension, resolves_[index].mipLevel, resolves_[index].slice);
            if (used.firstMip < state->states.MipLevels() &&
                used.firstSlice < state->states.Slices()) {
                state->states.At(used.firstMip, used.firstSlice) =
                    ResourceAccess::ResolveDestination;
            }
        }
    }

    resolveCount_ = 0;
    // 끝나지 않은 분리 전이는 계약 위반이다.
    validationErrors_ += splits_.Close();
    pendingSplits_.Clear();
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
    // queue마다 마지막 사용이다: 열린 기록의 queue는 그 기록의 값, 다른 queue는 마지막 제출이다.
    Retired entry;
    entry.fenceValue = fenceValue_;
    entry.pending = recording_ && recordingQueue_ == QueueKind::Graphics;
    entry.computeFenceValue = computeFenceValue_;
    entry.computePending = recording_ && recordingQueue_ == QueueKind::AsyncCompute;
    auto pushed = retired_.TryPushBack(entry);
    if (!pushed) {
        platform::Fatal("RecordingDevice retire list allocation failed");
    }
}

// 기록 장치는 능력 질의·버퍼 view를 쓰지 않는 시험용이다(compute queue 표시 말고는 빈 능력, view
// Unsupported).
const DeviceCaps& RecordingDevice::Caps() const noexcept {
    return caps_;
}

uint32 RecordingDevice::GetPixelFormatSampleCounts(PixelFormat format) const noexcept {
    (void)format;
    return 0;
}

PixelFormatCaps RecordingDevice::GetPixelFormatCaps(PixelFormat format) const noexcept {
    (void)format;
    return pixelFormatCaps_;
}

Result<BufferViewHandle> RecordingDevice::CreateBufferView(BufferHandle buffer,
                                                           const BufferViewDesc& desc) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<BufferViewHandle>::Failure(owner.Error());
    }

    if (!buffers_.IsValid(buffer.slot)) {
        return Result<BufferViewHandle>::Failure(ErrorCode::StaleHandle);
    }

    BufferViewState state;
    state.buffer = buffer.slot;
    state.access = desc.access;
    state.kind = desc.kind;
    state.shaderType = desc.kind == BufferViewKind::Typed
                           ? GetPixelFormatInfo(desc.format).shaderType
                           : PixelShaderType::None;
    state.stride =
        desc.kind == BufferViewKind::Structured && buffer.slot.index < bufferStrides_.Size()
            ? bufferStrides_[buffer.slot.index]
            : 0u;
    auto added = bufferViews_.TryAdd(state);
    if (!added) {
        return Result<BufferViewHandle>::Failure(added.Error());
    }

    Record(Op::CreateBufferView, (desc.access == ViewAccess::ReadWrite ? 1ull << 32 : 0ull) |
                                     static_cast<uint32>(desc.kind));
    return Result<BufferViewHandle>::Success(BufferViewHandle{added.Value()});
}

bool RecordingDevice::CheckBufferView(BufferViewHandle view, ViewAccess access,
                                      ResourceAccess allowed) noexcept {
    const BufferViewState* state = bufferViews_.Find(view.slot);
    if (state == nullptr) {
        return false;
    }

    const ResourceAccess* tracked = buffers_.Find(state->buffer);
    if (state->access != access || tracked == nullptr || !HasAnyAccess(*tracked, allowed)) {
        ++validationErrors_;
    }

    TouchBuffer(state->buffer);
    return true;
}

Result<void> RecordingDevice::ReadBuffer(BufferHandle buffer, uint64 offset,
                                         Span<uint8> out) noexcept {
    (void)buffer;
    (void)offset;
    (void)out;
    return Result<void>::Failure(ErrorCode::Unsupported);
}

void RecordingDevice::Retire(BufferViewHandle view) noexcept {
    if (!ownership_.Check() || !bufferViews_.TryRemove(view.slot)) {
        return;
    }

    Defer();
    Record(Op::RetireBufferView);
}

namespace {

uint32 ConstantFirstWord(Span<const uint8> bytes) noexcept {
    uint32 word = 0;
    if (bytes.Size() >= sizeof(word)) {
        platform::MemoryCopy(&word, bytes.Data(), sizeof(word));
    }

    return word;
}

} // namespace

// 실제 장치와 같은 규칙(크기·수명·갱신 대상)으로 검사한다.
Result<ConstantBufferHandle>
RecordingDevice::CreateConstantBuffer(ConstantBufferLifetime lifetime,
                                      Span<const uint8> contents) noexcept {
    if ((lifetime != ConstantBufferLifetime::SingleFrame &&
         lifetime != ConstantBufferLifetime::MultiFrame) ||
        !graphics::IsValidConstantBufferBytes(contents.Size())) {
        return Result<ConstantBufferHandle>::Failure(ErrorCode::InvalidArgument);
    }

    ConstantState state;
    state.lifetime = lifetime;
    state.bytes = static_cast<uint32>(contents.Size());
    state.word = ConstantFirstWord(contents);
    auto added = constantBuffers_.TryAdd(state);
    if (!added) {
        return Result<ConstantBufferHandle>::Failure(added.Error());
    }

    Record(Op::CreateConstantBuffer, (static_cast<uint64>(lifetime) << 32) | state.bytes);
    return Result<ConstantBufferHandle>::Success(ConstantBufferHandle{added.Value()});
}

Result<void> RecordingDevice::UpdateConstantBuffer(ConstantBufferHandle buffer,
                                                   Span<const uint8> contents) noexcept {
    ConstantState* state = constantBuffers_.Find(buffer.slot);
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (state->lifetime != ConstantBufferLifetime::MultiFrame || contents.Size() != state->bytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    state->word = ConstantFirstWord(contents);
    Record(Op::UpdateConstantBuffer, state->word);
    return Result<void>::Success();
}

void RecordingDevice::Retire(ConstantBufferHandle buffer) noexcept {
    if (!ownership_.Check() || !constantBuffers_.TryRemove(buffer.slot)) {
        return;
    }

    Defer();
    Record(Op::RetireConstantBuffer);
}

void RecordingDevice::SetConstantBuffer(graphics::ShaderStage stage, uint32 slot,
                                        ConstantBufferHandle buffer) noexcept {
    const ConstantState* state = constantBuffers_.Find(buffer.slot);
    Record(Op::SetConstantBuffer,
           ConstantValue(stage, slot, state != nullptr ? state->word : UnboundConstantWord));
}

Result<void> RecordingDevice::SetConstantData(graphics::ShaderStage stage, uint32 slot,
                                              Span<const uint8> bytes) noexcept {
    if (!recording_ || slot >= graphics::MaxConstantBufferSlots ||
        !graphics::IsValidConstantBufferBytes(bytes.Size())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Record(Op::SetConstantData, ConstantValue(stage, slot, ConstantFirstWord(bytes)));
    return Result<void>::Success();
}

// 읽기 view는 셰이더 읽기(그래픽·compute) 상태여야 한다.
void RecordingDevice::SetBufferView(uint32 slot, BufferViewHandle view) noexcept {
    const bool valid = CheckBufferView(view, ViewAccess::Read,
                                       ResourceAccess::SrvGraphics | ResourceAccess::SrvCompute);
    Record(Op::SetBufferView, (static_cast<uint64>(slot) << 1) | (valid ? 1u : 0u));
}

// compute 파이프라인은 그리기별 상수 크기만 기억한다(SetDrawConstants 검사, 그래픽과 같은 표).
Result<PipelineHandle>
RecordingDevice::CreateComputePipeline(const ComputePipelineDesc& desc) noexcept {
    ++pipelineCreateCalls_;
    lastComputeShader_ = desc.computeShader;
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<PipelineHandle>::Failure(owner.Error());
    }

    if (!graphics::ValidateComputePipelineDesc(desc)) {
        return Result<PipelineHandle>::Failure(ErrorCode::InvalidArgument);
    }

    auto added = pipelines_.TryAdd(desc.drawConstantBytes);
    if (!added) {
        return Result<PipelineHandle>::Failure(added.Error());
    }

    Record(Op::CreateComputePipeline, desc.drawConstantBytes);
    return Result<PipelineHandle>::Success(PipelineHandle{added.Value()});
}

// 쓰기 view는 UAV 상태여야 한다.
void RecordingDevice::SetWritableBufferView(uint32 slot, BufferViewHandle view) noexcept {
    const bool valid = CheckBufferView(view, ViewAccess::ReadWrite, UavAccessMask);
    Record(Op::SetWritableBufferView, (static_cast<uint64>(slot) << 1) | (valid ? 1u : 0u));
}

void RecordingDevice::SetWritableTexture(uint32 slot, TextureHandle view) noexcept {
    (void)slot;
    const TextureState* state = textures_.Find(view.slot);
    if (state == nullptr || !state->view || state->access != ViewAccess::ReadWrite) {
        ++validationErrors_;
        Record(Op::SetWritableTexture, NumericLimits<uint32>::Max());
        return;
    }

    CheckViewStates(*state, UavAccessMask);
    Record(Op::SetWritableTexture, state->range.firstMip);
}

void RecordingDevice::CheckViewStates(const TextureState& view, ResourceAccess allowed) noexcept {
    const TextureState* source = textures_.Find(view.source);
    if (source == nullptr) {
        ++validationErrors_;
        return;
    }

    for (uint32 slice = view.range.firstSlice;
         slice < view.range.firstSlice + view.range.sliceCount; ++slice) {
        for (uint32 mip = view.range.firstMip; mip < view.range.firstMip + view.range.mipCount;
             ++mip) {
            if (!HasAnyAccess(source->states.At(mip, slice), allowed)) {
                ++validationErrors_;
                return;
            }
        }
    }
}

void RecordingDevice::DrawIndirect(BufferHandle arguments, uint64 offset) noexcept {
    (void)arguments;
    (void)offset;
}

void RecordingDevice::DrawIndexedIndirect(BufferHandle arguments, uint64 offset) noexcept {
    (void)arguments;
    (void)offset;
}

// 그룹 수가 한도 밖이거나 파이프라인이 없으면 실제 장치처럼 무시한다(IgnoredDraws).
void RecordingDevice::Dispatch(uint32 groupCountX, uint32 groupCountY,
                               uint32 groupCountZ) noexcept {
    const uint32 counts[3] = {groupCountX, groupCountY, groupCountZ};
    bool inside = recording_ && pipelines_.IsValid(boundPipeline_.slot);
    for (uint32 axis = 0; axis < 3; ++axis) {
        inside = inside && counts[axis] <= caps_.limits.computeGroupCount[axis];
    }

    if (!inside) {
        ++ignoredDraws_;
        return;
    }

    Record(Op::Dispatch, DispatchValue(groupCountX, groupCountY, groupCountZ));
}

// 인자 버퍼는 IndirectArgs 상태여야 한다(아니면 검증 오류, 렌더 그래프가 전이를 낸다).
void RecordingDevice::DispatchIndirect(BufferHandle arguments, uint64 offset) noexcept {
    const ResourceAccess* tracked = buffers_.Find(arguments.slot);
    if (!recording_ || tracked == nullptr || !pipelines_.IsValid(boundPipeline_.slot) ||
        offset % sizeof(uint32) != 0) {
        ++ignoredDraws_;
        return;
    }

    if (!HasAnyAccess(*tracked, ResourceAccess::IndirectArgs)) {
        ++validationErrors_;
    }

    TouchBuffer(arguments.slot);
    Record(Op::DispatchIndirect, offset);
}

void RecordingDevice::SetViewport(const Viewport& viewport) noexcept {
    (void)viewport;
}

void RecordingDevice::SetScissor(const ScissorRect& scissor) noexcept {
    (void)scissor;
}

void RecordingDevice::SetStencilReference(uint32 reference) noexcept {
    (void)reference;
}

Result<void> RecordingDevice::CopyBuffer(BufferHandle destination, uint64 destinationOffset,
                                         BufferHandle source, uint64 sourceOffset,
                                         uint64 size) noexcept {
    (void)destination;
    (void)destinationOffset;
    (void)source;
    (void)sourceOffset;
    (void)size;
    return Result<void>::Failure(ErrorCode::Unsupported);
}

Result<void> RecordingDevice::CopyTexture(TextureHandle destination,
                                          TextureHandle source) noexcept {
    (void)destination;
    (void)source;
    return Result<void>::Failure(ErrorCode::Unsupported);
}

Result<void> RecordingDevice::ClearWritableBufferView(BufferViewHandle view,
                                                      uint32 value) noexcept {
    (void)view;
    (void)value;
    return Result<void>::Failure(ErrorCode::Unsupported);
}

void RecordingDevice::Retire(BufferHandle buffer) noexcept {
    if (!ownership_.Check() || !buffers_.TryRemove(buffer.slot)) {
        return;
    }

    if (buffer.slot.index < placedBuffers_.Size()) {
        placedBuffers_[buffer.slot.index] = PlacedState{};
    }

    Defer();
    Record(Op::RetireBuffer);
}

void RecordingDevice::Retire(TextureHandle texture) noexcept {
    if (!ownership_.Check() || !textures_.TryRemove(texture.slot)) {
        return;
    }

    if (texture.slot.index < placedTextures_.Size()) {
        placedTextures_[texture.slot.index] = PlacedState{};
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
    const uint64 computeCompleted = computeTimeline_.Completed();
    staging_.Reclaim(completed);
    for (usize index = retired_.Size(); index > 0; --index) {
        const Retired& entry = retired_[index - 1];
        if (!entry.pending && entry.fenceValue <= completed && !entry.computePending &&
            entry.computeFenceValue <= computeCompleted) {
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
    auto computeWaited = computeTimeline_.Wait(computeTimeline_.LastSubmitted());
    ReclaimCompleted();
    if (!waited) {
        return Result<void>::Failure(waited.Error());
    }

    if (!computeWaited) {
        return Result<void>::Failure(computeWaited.Error());
    }

    return Result<void>::Success();
}

GpuFencePoint RecordingDevice::LastSubmittedFence(QueueKind queue) const noexcept {
    return queue == QueueKind::AsyncCompute ? computeTimeline_.LastSubmitted()
                                            : timeline_.LastSubmitted();
}

Result<void> RecordingDevice::QueueWait(QueueKind queue, GpuFencePoint point) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return owner;
    }

    if (!caps_.features.asyncCompute) {
        return SingleQueueWait(timeline_, queue, point);
    }

    auto checked = TimelineOf(point)->IsComplete(point);
    if (!checked) {
        return Result<void>::Failure(checked.Error());
    }

    if (point.IsNull() || point.queue == queue) {
        return Result<void>::Success();
    }

    uint64& waited = waited_[static_cast<uint32>(queue)];
    waited = point.value > waited ? point.value : waited;
    Record(Op::QueueWait, QueueWaitValue(queue, point.value));
    return Result<void>::Success();
}

Result<bool> RecordingDevice::IsFenceComplete(GpuFencePoint point) const noexcept {
    return TimelineOf(point)->IsComplete(point);
}

Result<platform::WaitResult> RecordingDevice::WaitFence(GpuFencePoint point,
                                                        uint32 timeoutMs) noexcept {
    return const_cast<FenceTimeline*>(TimelineOf(point))->Wait(point, timeoutMs);
}

Result<void> RecordingDevice::NotifyFence(GpuFencePoint point, FenceCallback callback,
                                          void* context) noexcept {
    return const_cast<FenceTimeline*>(TimelineOf(point))->Notify(point, callback, context);
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
    return ignoredDraws_;
}

uint64 RecordingDevice::NullTextureBindings() const noexcept {
    return nullTextureBindings_;
}

uint64 RecordingDevice::ValidationErrorCount() const noexcept {
    return validationErrors_;
}

Result<graphics::TextureViewInfo>
RecordingDevice::GetTextureViewInfo(TextureHandle texture) noexcept {
    const TextureState* state = textures_.Find(texture.slot);
    if (state == nullptr) {
        return Result<graphics::TextureViewInfo>::Failure(ErrorCode::StaleHandle);
    }

    graphics::TextureViewInfo info;
    info.dimension = state->dimension;
    info.format = state->format;
    info.sampleCount = state->sampleCount;
    info.access = state->access;
    info.sampled = state->access == ViewAccess::Read;
    info.comparison = state->comparison;
    return Result<graphics::TextureViewInfo>::Success(info);
}

Result<graphics::BufferViewInfo>
RecordingDevice::GetBufferViewInfo(BufferViewHandle view) noexcept {
    const BufferViewState* state = bufferViews_.Find(view.slot);
    if (state == nullptr) {
        return Result<graphics::BufferViewInfo>::Failure(ErrorCode::StaleHandle);
    }

    return Result<graphics::BufferViewInfo>::Success(
        graphics::BufferViewInfo{state->kind, state->access, state->shaderType, state->stride});
}

Result<graphics::TextureSubresources>
RecordingDevice::GetTextureSubresources(TextureHandle texture) const noexcept {
    const TextureState* state = textures_.Find(texture.slot);
    if (state == nullptr) {
        return Result<graphics::TextureSubresources>::Failure(ErrorCode::StaleHandle);
    }

    return Result<graphics::TextureSubresources>::Success(
        graphics::TextureSubresources{state->states.MipLevels(), state->states.Slices()});
}

ResourceAccess RecordingDevice::TextureAccess(TextureHandle texture, uint32 mip,
                                              uint32 slice) const noexcept {
    const TextureState* state = textures_.Find(texture.slot);
    if (state == nullptr || mip >= state->states.MipLevels() || slice >= state->states.Slices()) {
        return ResourceAccess::Unknown;
    }

    return state->states.At(mip, slice);
}

ResourceAccess RecordingDevice::BufferAccess(BufferHandle buffer) const noexcept {
    const ResourceAccess* state = buffers_.Find(buffer.slot);
    return state == nullptr ? ResourceAccess::Unknown : *state;
}

void RecordingDevice::CheckDeclared(ResourceAccess tracked, ResourceAccess declared) noexcept {
    if (declared != ResourceAccess::Unknown && declared != ResourceAccess::Discard &&
        declared != tracked) {
        ++validationErrors_;
    }
}

void RecordingDevice::Transition(Span<const ResourceTransition> transitions) noexcept {
    if (!recording_) {
        return;
    }

    CheckPendingOverlap(transitions);
    ApplyTransitions(transitions, TransitionPhase::Full, 0);
}

namespace {

// 두 전이가 같은 자원의 겹치는 subresource(mip·slice 모두)를 가리키는지다.
bool Overlaps(const ResourceTransition& left, const ResourceTransition& right) noexcept {
    if (!left.buffer.IsNull() || !right.buffer.IsNull()) {
        return left.buffer == right.buffer && !left.buffer.IsNull();
    }

    if (!(left.texture == right.texture)) {
        return false;
    }

    return SubresourceRangesOverlap(left.range, right.range);
}

} // namespace

void RecordingDevice::CheckPendingOverlap(Span<const ResourceTransition> transitions) noexcept {
    for (const PendingTransition& pending : pendingSplits_) {
        for (const ResourceTransition& transition : transitions) {
            if (Overlaps(pending.transition, transition)) {
                ++validationErrors_;
                return;
            }
        }
    }
}

SplitTransition
RecordingDevice::BeginTransitions(Span<const ResourceTransition> transitions) noexcept {
    if (!recording_) {
        return {};
    }

    CheckPendingOverlap(transitions);
    const SplitTransition split = splits_.Begin(transitions);
    if (split.IsNull()) {
        return split;
    }

    for (const ResourceTransition& transition : transitions) {
        PendingTransition pending;
        pending.split = split.id;
        pending.transition = transition;
        if (!pendingSplits_.TryPushBack(pending)) {
            platform::Fatal("RecordingDevice split transition list allocation failed");
        }

        TransitionCall call;
        call.texture = transition.buffer.IsNull();
        call.index = call.texture ? transition.texture.slot.index : transition.buffer.slot.index;
        call.range = transition.range;
        call.before = transition.before;
        call.after = transition.after;
        call.phase = TransitionPhase::Begin;
        call.split = split.id;
        if (!transitions_.TryPushBack(call)) {
            platform::Fatal("RecordingDevice transition log allocation failed");
        }
    }

    Record(Op::BeginTransitions, split.id);
    return split;
}

void RecordingDevice::EndTransitions(SplitTransition split,
                                     Span<const ResourceTransition> transitions) noexcept {
    if (!recording_) {
        return;
    }

    const SplitEndResult ended = splits_.End(split, transitions);
    if (ended == SplitEndResult::Unknown) {
        ++ignoredDraws_;
    } else if (ended == SplitEndResult::Mismatch) {
        ++validationErrors_;
    }

    for (usize index = 0; index < pendingSplits_.Size();) {
        if (pendingSplits_[index].split == split.id && !split.IsNull()) {
            pendingSplits_[index] = pendingSplits_[pendingSplits_.Size() - 1];
            auto popped = pendingSplits_.TryPopBack();
            (void)popped;
            continue;
        }

        ++index;
    }

    if (ended == SplitEndResult::Null || ended == SplitEndResult::Unknown) {
        CheckPendingOverlap(transitions);
    }

    Record(Op::EndTransitions, split.id);
    ApplyTransitions(transitions, TransitionPhase::End, split.id);
}

void RecordingDevice::ApplyTransitions(Span<const ResourceTransition> transitions,
                                       TransitionPhase phase, uint32 split) noexcept {
    for (const ResourceTransition& transition : transitions) {
        if (!CanCombineAccess(transition.after)) {
            ++ignoredDraws_;
            continue;
        }

        // async compute 기록은 compute 상태 사이 전이만 낸다(Device.h CommandContext 주석).
        if (recordingQueue_ == QueueKind::AsyncCompute &&
            (!IsComputeQueueAccess(transition.before) || !IsComputeQueueAccess(transition.after))) {
            ++validationErrors_;
        }

        // placed 자원의 획득(GX12)은 사용 검사보다 먼저다. before는 Discard여야 한다.
        if (transition.acquire) {
            if (transition.before != ResourceAccess::Discard) {
                ++validationErrors_;
            }

            const TextureState* view =
                transition.buffer.IsNull() ? textures_.Find(transition.texture.slot) : nullptr;
            AcquirePlaced(!transition.buffer.IsNull(),
                          !transition.buffer.IsNull()     ? transition.buffer.slot.index
                          : view != nullptr && view->view ? view->source.index
                                                          : transition.texture.slot.index);
        }

        if (!transition.buffer.IsNull()) {
            TouchBuffer(transition.buffer.slot);
        } else {
            TouchTexture(transition.texture.slot);
        }

        TransitionCall call;
        call.range = transition.range;
        call.before = transition.before;
        call.after = transition.after;
        call.phase = phase;
        call.split = split;
        if (!transition.buffer.IsNull()) {
            ResourceAccess* state = buffers_.Find(transition.buffer.slot);
            if (state == nullptr) {
                ++ignoredDraws_;
                continue;
            }

            CheckDeclared(*state, transition.before);
            if (transition.after != ResourceAccess::Discard) {
                *state = transition.after;
            }

            call.texture = false;
            call.index = transition.buffer.slot.index;
        } else {
            TextureState* state = textures_.Find(transition.texture.slot);
            const ResolvedSubresourceRange range =
                state == nullptr
                    ? ResolvedSubresourceRange{}
                    : ResolveSubresourceRange(transition.range, state->states.MipLevels(),
                                              state->states.Slices());
            if (range.IsEmpty()) {
                ++ignoredDraws_;
                continue;
            }

            for (uint32 slice = range.firstSlice; slice < range.firstSlice + range.sliceCount;
                 ++slice) {
                for (uint32 mip = range.firstMip; mip < range.firstMip + range.mipCount; ++mip) {
                    ResourceAccess& tracked = state->states.At(mip, slice);
                    CheckDeclared(tracked, transition.before);
                    if (transition.after != ResourceAccess::Discard) {
                        tracked = transition.after;
                    }
                }
            }

            call.index = transition.texture.slot.index;
        }

        auto pushed = transitions_.TryPushBack(call);
        if (!pushed) {
            platform::Fatal("RecordingDevice transition log allocation failed");
        }

        Record(Op::Transition, transitions_.Size() - 1);
    }
}

usize RecordingDevice::PendingRetireCount() const noexcept {
    return retired_.Size();
}

void RecordingDevice::SetPipeline(PipelineHandle pipeline) noexcept {
    boundPipeline_ = pipelines_.IsValid(pipeline.slot) ? pipeline : PipelineHandle{};
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

void RecordingDevice::SetTexture(ShaderStage stage, uint32 slot, TextureHandle texture) noexcept {
    const TextureState* state = textures_.Find(texture.slot);
    TouchTexture(texture.slot);
    // async compute 기록은 compute 읽기 상태여야 한다.
    const ResourceAccess wholeRead = recordingQueue_ == QueueKind::AsyncCompute
                                         ? ResourceAccess::SrvCompute
                                         : ResourceAccess::SrvGraphics;
    // view는 그 범위만 셰이더 읽기(그래픽·compute) 상태여야 한다.
    if (state != nullptr && state->view) {
        if (state->access != ViewAccess::Read) {
            ++validationErrors_;
        } else {
            CheckViewStates(*state, ResourceAccess::SrvGraphics | ResourceAccess::SrvCompute);
        }
    } else if (state != nullptr) {
        for (uint32 index = 0; index < state->states.Count(); ++index) {
            if (!HasAnyAccess(state->states[index], wholeRead)) {
                ++validationErrors_;
                break;
            }
        }
    }

    // 실제 장치처럼 흰 텍스처로 대신 묶는 경우(null·stale·ReadWrite view)를 센다.
    if (state == nullptr || (state->view && state->access != ViewAccess::Read)) {
        ++nullTextureBindings_;
    }

    Record(Op::SetTexture, ResourceValue(stage, slot, state != nullptr ? 1u : 0u));
}

Result<void> RecordingDevice::SetSampler(ShaderStage stage, uint32 slot,
                                         const SamplerDesc* sampler) noexcept {
    if (!recording_ || ConstantBufferStageIndex(stage) >= ConstantBufferStageCount ||
        slot >= MaxResourceSlots || (sampler != nullptr && !IsValidSamplerDesc(*sampler))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const uint32 word = sampler != nullptr ? 1u + static_cast<uint32>(sampler->filter) : 0u;
    Record(Op::SetSampler, ResourceValue(stage, slot, word));
    return Result<void>::Success();
}

Result<void> RecordingDevice::SetDrawConstants(Span<const uint8> bytes) noexcept {
    // 실제 장치와 같은 계약이다: 묶인 파이프라인이 있고 크기가 그 선언과 같아야 한다.
    const uint32* declared = recording_ ? pipelines_.Find(boundPipeline_.slot) : nullptr;
    if (declared == nullptr || bytes.Size() != *declared) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    float first = 0.0f;
    if (bytes.Size() >= sizeof(first)) {
        platform::MemoryCopy(&first, bytes.Data(), sizeof(first));
    }

    Record(Op::SetDrawConstants,
           (static_cast<uint64>(bytes.Size()) << 32) | static_cast<uint32>(first * 255.0f));
    return Result<void>::Success();
}

void RecordingDevice::Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept {
    if (RejectOnComputeQueue()) {
        return;
    }

    Record(Op::Draw, (static_cast<uint64>(vertexCount) << 40) |
                         (static_cast<uint64>(instanceCount) << 20) | firstVertex);
}

void RecordingDevice::DrawIndexed(uint32 indexCount, uint32 instanceCount,
                                  uint32 firstIndex) noexcept {
    if (RejectOnComputeQueue()) {
        return;
    }

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

Result<ClockCalibration> RecordingDevice::CalibrateClocks(QueueKind queue) noexcept {
    if (!timestampsSupported_ ||
        (queue == QueueKind::AsyncCompute && !caps_.features.asyncCompute)) {
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
