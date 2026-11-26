#include "D3D12DeviceImpl.h"

// async compute queue다(Docs/Design/RenderBase 8절 "RG4 async compute 설계"). graphics
// queue와 따로 도는 D3D12 compute queue에 compute 기록을 제출하고, queue 사이 순서는
// ID3D12CommandQueue::Wait로 맞춘다(UE D3D12의 sync point 신호·대기와 같은 자리,
// D3D12LegacyBarriers.cpp:1580–1635). compute 기록은 compute queue가 받는 상태 사이 전이만
// 낸다(graphics 상태 barrier는 graphics queue에서).
namespace rocks::graphics::d3d12 {

namespace {

// compute 명령 목록이 받지 않는 resource state다(D3D12 "Using resource barriers": compute 목록의
// barrier는 픽셀 셰이더·출력 병합·IA index·resolve 상태를 다루지 못한다).
constexpr D3D12_RESOURCE_STATES GraphicsOnlyStates =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_RENDER_TARGET |
    D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ |
    D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_STREAM_OUT |
    D3D12_RESOURCE_STATE_RESOLVE_SOURCE | D3D12_RESOURCE_STATE_RESOLVE_DEST;

} // namespace

void D3D12Device::InitializeComputeQueue() noexcept {
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComRef<ID3D12CommandQueue> queue;
    ComRef<ID3D12Fence> fence;
    ComRef<ID3D12CommandAllocator> allocator;
    ComRef<ID3D12GraphicsCommandList> list;
    if (FAILED(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.Out()))) ||
        FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.Out()))) ||
        FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                               IID_PPV_ARGS(allocator.Out()))) ||
        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator.Get(),
                                          nullptr, IID_PPV_ARGS(list.Out())))) {
        // compute queue 없이 계속한다(graphics pipe로 낮춘다).
        Log::Warning(LogGraphicsD3D12, "D3D12 async compute queue creation failed; async compute "
                                       "work runs on the graphics queue");
        caps_.features.asyncCompute = false;
        caps_.features.efficientAsyncCompute = false;
        return;
    }

    list->Close();
    if (!computeAllocators_.TryReserve(MaxCommandAllocators)) {
        caps_.features.asyncCompute = false;
        caps_.features.efficientAsyncCompute = false;
        return;
    }

    AllocatorRecord first;
    first.allocator = Move(allocator);
    (void)computeAllocators_.TryPushBack(Move(first));
    computeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    computeWatcherEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (computeEvent_ == nullptr || computeWatcherEvent_ == nullptr) {
        caps_.features.asyncCompute = false;
        caps_.features.efficientAsyncCompute = false;
        return;
    }

    computeFenceSource_.fence = fence.Get();
    computeFenceSource_.event = computeWatcherEvent_;
    computeQueue_ = Move(queue);
    computeFence_ = Move(fence);
    computeList_ = Move(list);
    auto watching = computeTimeline_.StartWatcher(computeFenceSource_, "D3D12ComputeFence");
    if (!watching) {
        computeQueue_.Reset();
        caps_.features.asyncCompute = false;
        caps_.features.efficientAsyncCompute = false;
        return;
    }

    caps_.features.asyncCompute = true;
    // compute queue timestamp다(queue마다 주파수가 다를 수 있다). 만들지 못하면 compute 기록의
    // WriteTimestamp가 Unsupported다.
    UINT64 frequency = 0;
    if (SUCCEEDED(computeQueue_->GetTimestampFrequency(&frequency)) && frequency != 0) {
        constexpr uint32 Count = TimestampFrameSlots * MaxTimestampsPerFrame;
        D3D12_QUERY_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        heapDesc.Count = Count;
        const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
        const D3D12_RESOURCE_DESC buffer = BufferResource(uint64{Count} * sizeof(uint64));
        ComRef<ID3D12QueryHeap> queries;
        ComRef<ID3D12Resource> readback;
        if (SUCCEEDED(device_->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(queries.Out()))) &&
            SUCCEEDED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(readback.Out()))) &&
            computeTimestamps_.Reset(TimestampFrameSlots, MaxTimestampsPerFrame)) {
            computeTimestampHeap_ = Move(queries);
            computeTimestampReadback_ = Move(readback);
            computeTimestampSource_.buffer = computeTimestampReadback_.Get();
            computeTimestampFrequency_ = frequency;
        }
    }
}

void D3D12Device::ShutdownComputeQueue() noexcept {
    computeTimeline_.StopWatcher();
    computeTimeline_.Close();
    if (computeEvent_ != nullptr) {
        CloseHandle(computeEvent_);
        computeEvent_ = nullptr;
    }

    if (computeWatcherEvent_ != nullptr) {
        CloseHandle(computeWatcherEvent_);
        computeWatcherEvent_ = nullptr;
    }
}

uint64 D3D12Device::ComputeCompletedValue() const noexcept {
    if (!computeFence_) {
        return NumericLimits<uint64>::Max();
    }

    return computeFence_->GetCompletedValue();
}

const FenceTimeline& D3D12Device::TimelineOf(GpuFencePoint point) const noexcept {
    return point.queue == QueueKind::AsyncCompute ? computeTimeline_ : timeline_;
}

bool D3D12Device::AdaptForQueue(D3D12_RESOURCE_STATES& states) const noexcept {
    if (!computeRecording_) {
        return true;
    }

    // 셰이더 읽기(PIXEL | NON_PIXEL)는 compute 목록에서 NON_PIXEL만 남긴다.
    if ((states & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0) {
        states &= ~D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    return (states & GraphicsOnlyStates) == 0;
}

Result<uint32> D3D12Device::PickComputeAllocator() noexcept {
    const uint64 completed = computeFence_->GetCompletedValue();
    if (completed == D3D12FenceSource::RemovedFenceValue) {
        computeTimeline_.MarkLost();
        timeline_.MarkLost();
        return Result<uint32>::Failure(ErrorCode::Destroyed);
    }

    uint32 oldest = NumericLimits<uint32>::Max();
    for (uint32 index = 0; index < computeAllocators_.Size(); ++index) {
        const AllocatorRecord& record = computeAllocators_[index];
        if (record.inUse) {
            continue;
        }

        if (record.fenceValue <= completed) {
            return Result<uint32>::Success(index);
        }

        if (oldest == NumericLimits<uint32>::Max() ||
            record.fenceValue < computeAllocators_[oldest].fenceValue) {
            oldest = index;
        }
    }

    if (computeAllocators_.Size() < MaxCommandAllocators) {
        AllocatorRecord record;
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                   IID_PPV_ARGS(record.allocator.Out())))) {
            return FailWith<uint32>("CreateCommandAllocator (compute) failed");
        }

        auto pushed = computeAllocators_.TryPushBack(Move(record));
        if (!pushed) {
            return Result<uint32>::Failure(pushed.Error());
        }

        return Result<uint32>::Success(static_cast<uint32>(computeAllocators_.Size() - 1));
    }

    if (oldest == NumericLimits<uint32>::Max()) {
        return Result<uint32>::Failure(ErrorCode::Busy);
    }

    auto waited = WaitComputeOwner(computeAllocators_[oldest].fenceValue);
    if (!waited) {
        return Result<uint32>::Failure(waited.Error());
    }

    return Result<uint32>::Success(oldest);
}

Result<void> D3D12Device::WaitComputeOwner(uint64 value) noexcept {
    if (computeFence_->GetCompletedValue() < value) {
        if (FAILED(computeFence_->SetEventOnCompletion(value, computeEvent_))) {
            return Fail("compute fence wait setup failed");
        }

        WaitForSingleObject(computeEvent_, INFINITE);
    }

    const uint64 completed = computeFence_->GetCompletedValue();
    if (completed == D3D12FenceSource::RemovedFenceValue) {
        computeTimeline_.MarkLost();
        timeline_.MarkLost();
        return Result<void>::Failure(ErrorCode::Destroyed);
    }

    computeTimeline_.Publish(completed);
    return Result<void>::Success();
}

Result<CommandContext*> D3D12Device::BeginComputeRecording() noexcept {
    auto picked = PickComputeAllocator();
    if (!picked) {
        return Result<CommandContext*>::Failure(picked.Error());
    }

    AllocatorRecord& record = computeAllocators_[picked.Value()];
    if (FAILED(record.allocator->Reset()) ||
        FAILED(computeList_->Reset(record.allocator.Get(), nullptr))) {
        auto failed = Fail("compute command list reset failed");
        return Result<CommandContext*>::Failure(failed.Error());
    }

    computeRecordingAllocator_ = picked.Value();
    record.inUse = true;
    recording_ = true;
    computeRecording_ = true;
    list_ = computeList_.Get();
    ResetBindings();
    target_ = {};
    presentTarget_ = {};
    // compute root signature와 shader-visible heap이다(dispatch가 root 인자를 건다).
    appliedGraphicsRoot_ = nullptr;
    appliedComputeRoot_ = nullptr;
    ID3D12DescriptorHeap* heaps[] = {online_.Get(), onlineSamplers_.Get()};
    list_->SetDescriptorHeaps(2, heaps);
    return Result<CommandContext*>::Success(this);
}

Result<GpuFencePoint> D3D12Device::EndComputeRecording() noexcept {
    const usize unended = splits_.Close();
    if (validation_ && unended != 0) {
        validationErrors_ += unended;
        auto failed = Fail("split transitions were begun but not ended before EndFrame");
        (void)failed;
    }

    CloseMarkers();
    const TimestampRange stamps = computeTimestamps_.OpenRange();
    if (stamps.count != 0) {
        list_->ResolveQueryData(computeTimestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                stamps.first, stamps.count, computeTimestampReadback_.Get(),
                                uint64{stamps.first} * sizeof(uint64));
    }

    recording_ = false;
    computeRecording_ = false;
    list_ = graphicsList_.Get();
    AllocatorRecord& allocator = computeAllocators_[computeRecordingAllocator_];
    allocator.inUse = false;
    // 쌓인 업로드는 graphics queue에 먼저 제출하고 compute queue가 그것을 기다린다(업로드는 다음
    // 제출 전에 실행된다).
    auto uploads = FlushUploadsLocked();
    const auto abandon = [&]() noexcept {
        allocator.fenceValue = computeFenceValue_;
        CloseTransientRings(true, computeFenceValue_);
        AssignPendingComputeRetires(computeFenceValue_);
        computeTimestamps_.Close(computeFenceValue_, false);
        SettleTracking(false);
    };
    if (!uploads) {
        (void)computeList_->Close();
        abandon();
        return Result<GpuFencePoint>::Failure(uploads.Error());
    }

    if (FAILED(computeList_->Close())) {
        abandon();
        return FailWith<GpuFencePoint>("compute command list close failed");
    }

    if (uploads.Value() != 0) {
        const HRESULT waited = computeQueue_->Wait(fence_.Get(), uploads.Value());
        if (FAILED(waited)) {
            DetectLost(waited);
            abandon();
            return FailWith<GpuFencePoint>("compute queue wait for uploads failed");
        }
    }

    ID3D12CommandList* lists[] = {computeList_.Get()};
    computeQueue_->ExecuteCommandLists(1, lists);
    SettleTracking(true);
    const uint64 value = computeFenceValue_ + 1;
    const HRESULT signaled = computeQueue_->Signal(computeFence_.Get(), value);
    if (FAILED(signaled)) {
        DetectLost(signaled);
        allocator.fenceValue = RetainUntilDestructionFence;
        CloseTransientRings(true, RetainUntilDestructionFence);
        AssignPendingComputeRetires(RetainUntilDestructionFence);
        computeTimestamps_.Close(RetainUntilDestructionFence, false);
        DrainValidationMessages();
        return FailWith<GpuFencePoint>("compute queue signal failed");
    }

    computeFenceValue_ = value;
    computeTimeline_.Submitted(value, nullptr);
    allocator.fenceValue = value;
    CloseTransientRings(true, value);
    AssignPendingComputeRetires(value);
    computeTimestamps_.Close(value, true);
    ProcessRetired();
    DrainValidationMessages();
    return Result<GpuFencePoint>::Success(computeTimeline_.Point(value));
}

void D3D12Device::AssignPendingComputeRetires(uint64 value) noexcept {
    for (Retired& entry : retired_) {
        if (entry.computePending) {
            entry.computePending = false;
            entry.computeFenceValue = value;
        }
    }
}

Result<void> D3D12Device::QueueWait(QueueKind queue, GpuFencePoint point) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return owner;
    }

    platform::LockGuard lock(gate_);
    if (!computeQueue_) {
        return SingleQueueWait(timeline_, queue, point);
    }

    auto checked = TimelineOf(point).IsComplete(point);
    if (!checked) {
        return Result<void>::Failure(checked.Error());
    }

    if (point.IsNull() || point.queue == queue) {
        return Result<void>::Success();
    }

    ID3D12CommandQueue* waiting =
        queue == QueueKind::AsyncCompute ? computeQueue_.Get() : queue_.Get();
    ID3D12Fence* fence =
        point.queue == QueueKind::AsyncCompute ? computeFence_.Get() : fence_.Get();
    const HRESULT waited = waiting->Wait(fence, point.value);
    if (FAILED(waited)) {
        DetectLost(waited);
        return FailWith<void>("ID3D12CommandQueue::Wait failed");
    }

    return Result<void>::Success();
}

} // namespace rocks::graphics::d3d12
