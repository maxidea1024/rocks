#include "D3D12DeviceImpl.h"

#include <DxgiGraphics/DxgiAdapter.h>

namespace rocks::graphics::d3d12 {

D3D12Device::D3D12Device(Allocator& allocator) noexcept
    : adapterName_(allocator), lastError_(allocator), buffers_(allocator), textures_(allocator),
      pipelines_(allocator), swapChains_(allocator), heaps_(allocator), retired_(allocator),
      rtvs_(allocator), dsvs_(allocator), srvs_(allocator), samplers_(allocator),
      allocators_(allocator), messageScratch_(allocator),
      timeline_(allocator, NewDeviceEpoch(), QueueKind::Graphics),
      computeTimeline_(allocator, timeline_.DeviceEpoch(), QueueKind::AsyncCompute),
      computeAllocators_(allocator), computeTimestamps_(allocator), allocator_(allocator),
      readbacks_(allocator), ring_(allocator), timestamps_(allocator), touchedTextures_(allocator),
      bufferViews_(allocator), cpuUavs_(allocator), bufferStates_(allocator), splits_(allocator),
      rootSignatures_(allocator), shaders_(allocator) {
}

D3D12Device::~D3D12Device() {
    // 장치는 소유 스레드(또는 소유자가 없을 때 아무 스레드)에서 파괴한다.
    if (!ownership_.IsOwner() && !ownership_.Acquire()) {
        platform::Fatal("graphics device destroyed while another thread owns it");
    }
    // 초기화가 staging pool 전에 실패했으면 아무것도 제출하지 않았다(WaitIdle의 회수가 pool을
    // 쓰므로 부르지 않는다).
    if (queue_ && fence_ && fenceEvent_ != nullptr && staging_) {
        auto idle = WaitIdle();
        (void)idle;
    }
    // 모든 제출이 끝났거나(WaitIdle) 장치가 제거되어 관찰 스레드가 곧 빠져나온다.
    timeline_.StopWatcher();
    timeline_.Close();
    ShutdownComputeQueue();
    if (staging_) {
        staging_->Clear();
    }
    readbacks_.Clear();
    retired_.Clear();
    swapChains_.Clear();
    textures_.Clear();
    pipelines_.Clear();
    bufferViews_.Clear();
    buffers_.Clear();
    // placed 자원을 모두 놓은 뒤 heap을 놓는다(GX12).
    heaps_.Clear();
    if (fenceEvent_ != nullptr) {
        CloseHandle(fenceEvent_);
    }
    if (watcherEvent_ != nullptr) {
        CloseHandle(watcherEvent_);
    }
}

Result<void> D3D12Device::Initialize(const DeviceDesc& desc) noexcept {
    UINT factoryFlags = 0;
    // GPU 기반 검증은 debug layer를 함께 켠다(UE 5.8 D3D12Adapter.cpp CreateRootDevice와 같다).
    // 장치를 만들기 전에 켜야 한다.
    if (desc.enableValidation || desc.enableGpuValidation) {
        ComRef<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.Out())))) {
            debug->EnableDebugLayer();
            validation_ = true;
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            if (desc.enableGpuValidation) {
                ComRef<ID3D12Debug1> debug1;
                if (SUCCEEDED(debug.TryQueryAs(debug1))) {
                    debug1->SetEnableGPUBasedValidation(TRUE);
                    gpuValidation_ = true;
                } else {
                    Log::Warning(LogGraphicsD3D12,
                                 "D3D12 GPU-based validation was requested but ID3D12Debug1 is not "
                                 "available; continuing with the debug layer only");
                }
            }
        }
    }

    const HRESULT factoryCreated = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(factory_.Out()));
    if (FAILED(factoryCreated)) {
        return Fail("CreateDXGIFactory2", factoryCreated);
    }

    ComRef<IDXGIAdapter1> adapter;
    if (!desc.preferSoftware) {
        ComRef<IDXGIFactory6> factory6;
        if (SUCCEEDED(factory_.TryQueryAs(factory6))) {
            for (UINT index = 0;; ++index) {
                ComRef<IDXGIAdapter1> candidate;
                if (FAILED(factory6->EnumAdapterByGpuPreference(
                        index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                        IID_PPV_ARGS(candidate.Out())))) {
                    break;
                }
                DXGI_ADAPTER_DESC1 info = {};
                candidate->GetDesc1(&info);
                if ((info.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                    continue;
                }
                if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                                                IID_PPV_ARGS(device_.Out())))) {
                    adapter = Move(candidate);
                    break;
                }
            }
        }
    }

    if (!device_) {
        // 하드웨어 후보가 없거나 모두 실패했다. 마지막 수단인 WARP의 실패 원인을 남긴다.
        const HRESULT warp = factory_->EnumWarpAdapter(IID_PPV_ARGS(adapter.Out()));
        if (FAILED(warp)) {
            return Fail("EnumWarpAdapter", warp);
        }

        const HRESULT created =
            D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device_.Out()));
        if (FAILED(created)) {
            return Fail("D3D12CreateDevice (WARP)", created);
        }
        software_ = true;
    }

    // transient heap(GX12)의 CREATE_NOT_ZEROED는 그 flag를 아는 런타임(ID3D12Device8)에서만
    // 쓴다(UE D3D12Adapter.cpp:1101–1107).
    ComRef<ID3D12Device8> device8;
    heapNotZeroed_ = SUCCEEDED(device_.TryQueryAs(device8));

    // 이름은 진단용이라 읽지 못해도(빈 이름) 장치 생성은 계속한다.
    auto named = dxgi::AssignAdapterName(adapter.Get(), adapterName_);
    (void)named;

    if (validation_) {
        ConfigureValidationQueues();
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    const HRESULT queueCreated =
        device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue_.Out()));
    if (FAILED(queueCreated)) {
        return Fail("CreateCommandQueue", queueCreated);
    }

    auto reservedAllocators = allocators_.TryReserve(MaxCommandAllocators);
    if (!reservedAllocators) {
        return reservedAllocators;
    }
    auto firstAllocator = AddCommandAllocator();
    if (!firstAllocator) {
        return Result<void>::Failure(firstAllocator.Error());
    }
    // 저장소 하나에는 열린 list가 하나만 붙을 수 있으므로 만들자마자 닫는다.
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          allocators_[0].allocator.Get(), nullptr,
                                          IID_PPV_ARGS(graphicsList_.Out())))) {
        return Fail("CreateCommandList failed");
    }
    list_ = graphicsList_.Get();
    list_->Close();
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          allocators_[0].allocator.Get(), nullptr,
                                          IID_PPV_ARGS(uploadList_.Out())))) {
        return Fail("CreateCommandList failed");
    }
    uploadList_->Close();

    const HRESULT fenceCreated =
        device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.Out()));
    if (FAILED(fenceCreated)) {
        return Fail("CreateFence", fenceCreated);
    }
    // 오류 코드는 실패한 호출 바로 뒤에 읽는다(다음 호출이 덮어쓴다).
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fenceEvent_ == nullptr) {
        return Fail("CreateEventW (fence)", HRESULT_FROM_WIN32(GetLastError()));
    }
    watcherEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (watcherEvent_ == nullptr) {
        return Fail("CreateEventW (watcher)", HRESULT_FROM_WIN32(GetLastError()));
    }
    fenceSource_.fence = fence_.Get();
    fenceSource_.event = watcherEvent_;
    auto watching = timeline_.StartWatcher(fenceSource_, "D3D12Fence");
    if (!watching) {
        return Fail("fence watcher thread creation failed");
    }

    auto rtvs = rtvs_.Initialize(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RtvCapacity, false);
    auto dsvs = dsvs_.Initialize(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DsvCapacity, false);
    auto srvs =
        srvs_.Initialize(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SrvCapacity, true);
    auto samplers = samplers_.Initialize(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                         SamplerCapacity, true);
    if (!rtvs || !dsvs || !srvs || !samplers) {
        return Fail("descriptor heap creation failed");
    }

    // 장치 능력(GX1)과 형식 능력은 장치 수명 동안 바뀌지 않으므로 한 번 질의해
    // 둔다(PixelFormat.ko.md PF3). MSAA 표본 수가 2 이상이면 Multisample 능력이다.
    caps_ = d3d12::QueryDeviceCaps(device_.Get(), adapter.Get(), featureLevel_);
    for (uint32 number = 0; number <= MaxPixelFormatNumber; ++number) {
        const PixelFormat format = static_cast<PixelFormat>(number);
        PixelFormatCaps formatCaps = d3d12::QueryFormatCaps(device_.Get(), format);
        sampleCounts_[number] = d3d12::QuerySampleCounts(device_.Get(), format, formatCaps);
        if (sampleCounts_[number] > SampleCountBit(1)) {
            formatCaps = formatCaps | PixelFormatCaps::Multisample;
        }

        formatCaps_[number] = formatCaps;
    }

    // 버퍼 view slot이 비었을 때 묶는 null SRV다(셰이더가 읽지 않는 table도 채워 둔다).
    auto nullSrv = srvs_.Allocate();
    if (!nullSrv) {
        return Fail("null buffer SRV descriptor allocation failed");
    }

    nullBufferSrv_ = nullSrv.Value();
    D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc = {};
    nullDesc.Format = DXGI_FORMAT_R32_UINT;
    nullDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    device_->CreateShaderResourceView(nullptr, &nullDesc, srvs_.Cpu(nullBufferSrv_));

    auto compute = CreateComputeObjects();
    if (!compute) {
        return compute;
    }

    // 업로드 ring(GX8): upload heap 버퍼 하나를 계속 map해 두고 구간을 나눠 쓴다.
    const uint64 ringBytes =
        desc.uploadRingBytes != 0 ? desc.uploadRingBytes : DefaultUploadRingBytes;
    auto ringReset = ring_.Reset(ringBytes);
    if (!ringReset) {
        return ringReset;
    }
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC ringDesc = BufferResource(ringBytes);
    void* ringMapped = nullptr;
    const D3D12_RANGE noRead = {0, 0};
    const HRESULT ringCreated = device_->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &ringDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(ringBuffer_.Out()));
    if (FAILED(ringCreated)) {
        return Fail("CreateCommittedResource (upload ring)", ringCreated);
    }

    const HRESULT ringMappedResult = ringBuffer_->Map(0, &noRead, &ringMapped);
    if (FAILED(ringMappedResult)) {
        return Fail("ID3D12Resource::Map (upload ring)", ringMappedResult);
    }
    ringData_ = static_cast<uint8*>(ringMapped);
    stagingSource_.device = device_.Get();
    const uint64 stagingBudget =
        desc.stagingBudgetBytes != 0 ? desc.stagingBudgetBytes : DefaultStagingBudgetBytes;
    auto pool =
        UniquePtr<StagingPool>::TryMake(allocator_, allocator_, stagingSource_, stagingBudget);
    if (!pool) {
        return Result<void>::Failure(pool.Error());
    }
    staging_ = Move(pool.Value());
    InitializeTimestamps();
    InitializeComputeQueue();

    // 텍스처를 묶지 않은 그리기가 쓸 1x1 흰색 텍스처다.
    const uint8 white[4] = {255, 255, 255, 255};
    TextureDesc whiteDesc;
    whiteDesc.width = 1;
    whiteDesc.height = 1;
    whiteDesc.format = PixelFormat::RGBA8Unorm;
    auto whiteTexture = CreateTexture(whiteDesc, Span<const uint8>(white, 4));
    if (!whiteTexture) {
        return Result<void>::Failure(whiteTexture.Error());
    }
    white_ = whiteTexture.Value();
    return Result<void>::Success();
}

DeviceInfo D3D12Device::Info() const noexcept {
    DeviceInfo info;
    info.backend = Backend::D3D12;
    info.adapterName = adapterName_.View();
    info.deviceEpoch = timeline_.DeviceEpoch();
    // 생성·갱신은 내부 잠금 아래 업로드 기록에 쌓이고 소유 스레드의 다음 제출이 실행한다.
    info.concurrentResourceCreation = true;
    info.validationEnabled = validation_;
    info.gpuValidationEnabled = gpuValidation_;
    info.software = software_;
    // graphics queue timestamp와 clock calibration이다. queue timestamp는 끊기지 않으므로
    // disjoint 감지는 해당 없다.
    info.timestamps = timestampHeap_.Get() != nullptr;
    info.timestampFrequency = timestampFrequency_;
    info.clockCalibration = info.timestamps;
    info.debugMarkers = true;
    info.apiVersion = featureLevel_;
    return info;
}

const DeviceCaps& D3D12Device::Caps() const noexcept {
    return caps_;
}

uint32 D3D12Device::GetPixelFormatSampleCounts(PixelFormat format) const noexcept {
    const uint32 number = PixelFormatNumber(format);
    return number <= MaxPixelFormatNumber ? sampleCounts_[number] : 0;
}

void D3D12Device::ReclaimCompleted() noexcept {
    if (!ownership_.Check()) {
        return;
    }
    platform::LockGuard lock(gate_);

    ProcessRetired();
}

Result<void> D3D12Device::WaitIdle() noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }
    platform::LockGuard lock(gate_);
    return WaitIdleLocked();
}

Result<void> D3D12Device::WaitIdleLocked() noexcept {
    // 쌓인 업로드도 제출해 함께 기다린다.
    auto uploads = FlushUploadsLocked();
    if (!uploads) {
        ProcessRetired();
        return Result<void>::Failure(uploads.Error());
    }
    auto signaled = SignalQueue();
    if (!signaled) {
        // 장치가 제거되었으면 fence 완료 값이 최대라 회수 대기 항목을 모두 놓을 수 있다.
        ProcessRetired();
        return Result<void>::Failure(signaled.Error());
    }
    auto waited = WaitFenceOwner(signaled.Value());
    // compute queue의 제출도 기다린다(RG4).
    if (waited && computeQueue_ && computeFenceValue_ != 0) {
        waited = WaitComputeOwner(computeFenceValue_);
    }

    // 기록 중인 프레임의 보류 항목은 아직 제출되지 않았으므로 남긴다.
    ProcessRetired();
    if (!waited) {
        return waited;
    }
    return Result<void>::Success();
}

GpuFencePoint D3D12Device::LastSubmittedFence(QueueKind queue) const noexcept {
    if (queue == QueueKind::AsyncCompute) {
        return computeQueue_ ? computeTimeline_.LastSubmitted() : GpuFencePoint{};
    }

    return timeline_.LastSubmitted();
}

Result<bool> D3D12Device::IsFenceComplete(GpuFencePoint point) const noexcept {
    return TimelineOf(point).IsComplete(point);
}

Result<platform::WaitResult> D3D12Device::WaitFence(GpuFencePoint point,
                                                    uint32 timeoutMs) noexcept {
    return const_cast<FenceTimeline&>(TimelineOf(point)).Wait(point, timeoutMs);
}

Result<void> D3D12Device::NotifyFence(GpuFencePoint point, FenceCallback callback,
                                      void* context) noexcept {
    return const_cast<FenceTimeline&>(TimelineOf(point)).Notify(point, callback, context);
}

bool D3D12Device::IsLost() const noexcept {
    return timeline_.IsLost();
}

bool D3D12Device::IsOwnerThread() const noexcept {
    return ownership_.IsOwner();
}

Result<void> D3D12Device::ReleaseOwnership() noexcept {
    if (!ownership_.IsOwner()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }
    if (recording_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    return ownership_.Release();
}

Result<void> D3D12Device::AcquireOwnership() noexcept {
    return ownership_.Acquire();
}

uint64 D3D12Device::RejectedCalls() const noexcept {
    return ownership_.Rejected();
}

usize D3D12Device::PendingRetireCount() const noexcept {
    platform::LockGuard lock(gate_);
    return retired_.Size();
}

StringView D3D12Device::LastError() const noexcept {
    return lastError_.View();
}

uint64 D3D12Device::IgnoredDraws() const noexcept {
    return ignoredDraws_;
}

uint64 D3D12Device::ValidationErrorCount() const noexcept {
    DrainValidationMessages();
    return validationErrors_;
}

// 진단 문자열을 남기고 실패를 돌려준다. 장치가 손실되었으면 Destroyed다. 실패는 호출자가 결과로
// 받아 결정하는 쪽이 기록한다(LoggingSystem 13절). 장치는 손실 전이만 직접 남긴다.
Result<void> D3D12Device::Fail(const char* message) noexcept {
    auto assigned = lastError_.TryAssign(StringView(message, platform::CStringLength(message)));
    (void)assigned;
    failureTruncated_ = CopyStage(message);
    failureResult_ = S_OK;
    return Result<void>::Failure(timeline_.IsLost() ? ErrorCode::Destroyed
                                                    : ErrorCode::DependencyFailed);
}

// API 호출 stage가 HRESULT로 실패했다. 원본 코드를 보존하고(DeviceCreateReport::nativeCode)
// LastError에 "stage failed (HRESULT 0x...)"로 덧붙인다. 장치 제거면 손실을 게시한다.
Result<void> D3D12Device::Fail(const char* stage, HRESULT result) noexcept {
    DetectLost(result);
    char storage[FailureMessageBytes];
    FixedStringBuilder text{Span<char>(storage)};
    (void)text.TryAppendFormat("{} failed (HRESULT {:#x})", stage, static_cast<uint32>(result));
    auto assigned = lastError_.TryAssign(text.View());
    (void)assigned;
    failureTruncated_ = CopyStage(stage) || text.Truncated();
    failureResult_ = result;
    return Result<void>::Failure(timeline_.IsLost() ? ErrorCode::Destroyed
                                                    : ErrorCode::DependencyFailed);
}

// 실패한 stage 이름을 report용 칸에 복사한다. 잘렸으면 참이다.
bool D3D12Device::CopyStage(const char* stage) noexcept {
    FixedStringBuilder text{Span<char>(failureStage_)};
    (void)text.TryAppend(StringView(stage));
    return text.Truncated();
}

// API 실패가 장치 제거 때문이면 손실을 게시한다(기다리는 쪽을 Destroyed로 깨운다). 손실 전이 때
// GetDeviceRemovedReason과 함께 Error를 한 번 남긴다.
void D3D12Device::DetectLost(HRESULT failure) noexcept {
    // 장치를 만들기 전(팩터리·어댑터 단계)의 실패에는 잃을 장치가 없다.
    if (!device_) {
        return;
    }

    const HRESULT reason = device_->GetDeviceRemovedReason();
    if (failure == DXGI_ERROR_DEVICE_REMOVED || failure == DXGI_ERROR_DEVICE_RESET ||
        reason != S_OK) {
        timeline_.MarkLost();
        if (platform::AtomicExchange<platform::MemoryOrder::AcquireRelease>(&lossLogged_, 1u) ==
            0) {
            Log::Error(LogGraphicsD3D12,
                       "D3D12 device removed on {Adapter}: {Reason} (removed reason HRESULT "
                       "{ReasonCode:#x}, failing call HRESULT {HResult:#x}); the device "
                       "is lost and every later call fails",
                       adapterName_.View(), RemovedReasonName(reason), static_cast<uint32>(reason),
                       static_cast<uint32>(failure));
        }
    }
}

// DeviceCreateReport를 채운다(생성 실패, CreateD3D12Device가 부른다).
void D3D12Device::FillReport(ErrorCode code, DeviceCreateReport& report) const noexcept {
    report = DeviceCreateReport{};
    report.code = code;
    report.nativeCode = static_cast<int64>(static_cast<uint32>(failureResult_));
    FixedStringBuilder stage{Span<char>(report.stage)};
    (void)stage.TryAppend(failureStage_[0] != '\0' ? StringView(failureStage_)
                                                   : StringView("Initialize"));
    FixedStringBuilder detail{Span<char>(report.detail)};
    (void)detail.TryAppend(lastError_.ByteCount() != 0 ? lastError_.View()
                                                       : StringView(ErrorCodeName(code)));
    report.truncated = failureTruncated_ || stage.Truncated() || detail.Truncated();
}

// graphics queue에 다음 fence 값을 신호하고 타임라인에 알린다. 실패하면 값을 쓰지 않는다.
Result<uint64> D3D12Device::SignalQueue() noexcept {
    if (timeline_.IsLost()) {
        return Result<uint64>::Failure(ErrorCode::Destroyed);
    }
    const uint64 value = fenceValue_ + 1;
    const HRESULT signaled = queue_->Signal(fence_.Get(), value);
    if (FAILED(signaled)) {
        DetectLost(signaled);
        return FailWith<uint64>("queue signal failed");
    }
    fenceValue_ = value;
    timeline_.Submitted(value, nullptr);
    return Result<uint64>::Success(value);
}

// 소유 스레드가 직접 본 완료 값을 타임라인에 게시한다(관찰 스레드보다 먼저 알 수 있다).
void D3D12Device::PublishObserved() noexcept {
    const uint64 completed = fence_->GetCompletedValue();
    if (completed == D3D12FenceSource::RemovedFenceValue) {
        timeline_.MarkLost();
        return;
    }
    timeline_.Publish(completed);
}

Result<uint32> D3D12Device::AddCommandAllocator() noexcept {
    AllocatorRecord record;
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(record.allocator.Out())))) {
        return FailWith<uint32>("CreateCommandAllocator failed");
    }
    auto pushed = allocators_.TryPushBack(Move(record));
    if (!pushed) {
        return Result<uint32>::Failure(pushed.Error());
    }
    return Result<uint32>::Success(static_cast<uint32>(allocators_.Size() - 1));
}

// GPU가 다 쓴 기록 저장소를 고른다. 없으면 상한까지 새로 만들고, 상한이면 가장 오래된 제출을
// 기다린다(프레임 slot이 앞서 기다리므로 보통은 기다리지 않는다).
Result<uint32> D3D12Device::PickCommandAllocator() noexcept {
    const uint64 completed = fence_->GetCompletedValue();
    if (completed == D3D12FenceSource::RemovedFenceValue) {
        timeline_.MarkLost();
        return Result<uint32>::Failure(ErrorCode::Destroyed);
    }
    uint32 oldest = NumericLimits<uint32>::Max();
    for (uint32 index = 0; index < allocators_.Size(); ++index) {
        if (allocators_[index].inUse) {
            continue;
        }
        if (allocators_[index].fenceValue <= completed) {
            return Result<uint32>::Success(index);
        }
        if (oldest == NumericLimits<uint32>::Max() ||
            allocators_[index].fenceValue < allocators_[oldest].fenceValue) {
            oldest = index;
        }
    }
    if (allocators_.Size() < MaxCommandAllocators) {
        return AddCommandAllocator();
    }
    // 소유 스레드만 GPU를 기다린다. 다른 스레드(생성·갱신)는 자리가 없으면 Busy다.
    if (oldest == NumericLimits<uint32>::Max() || !ownership_.IsOwner()) {
        return Result<uint32>::Failure(ErrorCode::Busy);
    }
    auto waited = WaitFenceOwner(allocators_[oldest].fenceValue);
    if (!waited) {
        return Result<uint32>::Failure(waited.Error());
    }
    return Result<uint32>::Success(oldest);
}

// 소유 스레드가 value 완료를 직접 기다린다(업로드·readback·WaitIdle·저장소 재사용).
Result<void> D3D12Device::WaitFenceOwner(uint64 value) noexcept {
    if (fence_->GetCompletedValue() < value) {
        if (FAILED(fence_->SetEventOnCompletion(value, fenceEvent_))) {
            return Fail("fence wait setup failed");
        }
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
    PublishObserved();
    if (timeline_.IsLost()) {
        return Result<void>::Failure(ErrorCode::Destroyed);
    }
    return Result<void>::Success();
}

void D3D12Device::KeepUntilDestruction(ComRef<ID3D12Resource>&& object) noexcept {
    Retired entry;
    entry.fenceValue = RetainUntilDestructionFence;
    entry.object.Attach(object.Detach());
    auto pushed = retired_.TryPushBack(Move(entry));
    if (!pushed) {
        // 기록할 공간조차 없으면 해제하지 않고 버린다(GPU가 읽는 중일 수 있는 메모리를 놓는
        // 것보다 낫다).
        (void)entry.object.Detach();
    }
}

void D3D12Device::ConfigureValidationQueues() noexcept {
    if (SUCCEEDED(device_.TryQueryAs(infoQueue_))) {
        // 오류만 저장해 저장 한도(기본 1024개)를 경고가 채우지 않게 한다. GPU 기반 검증 실행은
        // 진단 실행이라 경고도 저장해 남긴다(세지는 않는다).
        D3D12_MESSAGE_SEVERITY severities[] = {D3D12_MESSAGE_SEVERITY_CORRUPTION,
                                               D3D12_MESSAGE_SEVERITY_ERROR,
                                               D3D12_MESSAGE_SEVERITY_WARNING};
        D3D12_INFO_QUEUE_FILTER filter = {};
        filter.AllowList.NumSeverities = gpuValidation_ ? 3 : 2;
        filter.AllowList.pSeverityList = severities;
        (void)infoQueue_->PushStorageFilter(&filter);
    }
    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(dxgiInfoQueue_.Out())))) {
        DXGI_INFO_QUEUE_MESSAGE_SEVERITY severities[] = {
            DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR};
        DXGI_INFO_QUEUE_FILTER filter = {};
        filter.AllowList.NumSeverities = 2;
        filter.AllowList.pSeverityList = severities;
        (void)dxgiInfoQueue_->PushStorageFilter(DxgiDebugDxgi, &filter);
    }
}

// 저장된 검증 메시지 중 오류를 세어 누적하고 비운다. 메시지 길이에 상한을 두지 않는다.
void D3D12Device::DrainValidationMessages() const noexcept {
    if (infoQueue_) {
        const UINT64 count = infoQueue_->GetNumStoredMessages();
        for (UINT64 index = 0; index < count; ++index) {
            SIZE_T length = 0;
            if (FAILED(infoQueue_->GetMessage(index, nullptr, &length)) || length == 0) {
                ++validationErrors_;
                continue;
            }
            auto* message = static_cast<D3D12_MESSAGE*>(Scratch(length));
            if (message == nullptr || FAILED(infoQueue_->GetMessage(index, message, &length)) ||
                message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
                // 읽지 못한 메시지도 오류로 센다(필터가 오류만 저장한다. 경고도 저장하는 GPU 기반
                // 검증 실행에서는 읽지 못한 경고도 오류로 센다).
                ++validationErrors_;
            }

            // 원문을 메시지 ID마다 한 번 남긴다(세기는 위와 같다).
            if (message != nullptr && message->pDescription != nullptr &&
                validationFilter_.First(static_cast<uint32>(message->ID))) {
                Log::Warning(
                    LogGraphicsD3D12Validation, "D3D12 debug layer message {Id}: {Message}",
                    static_cast<uint32>(message->ID),
                    StringView(message->pDescription, message->DescriptionByteLength > 0
                                                          ? message->DescriptionByteLength - 1
                                                          : 0));
            }
        }
        infoQueue_->ClearStoredMessages();
    }
    if (dxgiInfoQueue_) {
        const UINT64 count = dxgiInfoQueue_->GetNumStoredMessages(DxgiDebugDxgi);
        for (UINT64 index = 0; index < count; ++index) {
            SIZE_T length = 0;
            if (FAILED(dxgiInfoQueue_->GetMessage(DxgiDebugDxgi, index, nullptr, &length)) ||
                length == 0) {
                ++validationErrors_;
                continue;
            }
            auto* message = static_cast<DXGI_INFO_QUEUE_MESSAGE*>(Scratch(length));
            if (message == nullptr ||
                FAILED(dxgiInfoQueue_->GetMessage(DxgiDebugDxgi, index, message, &length)) ||
                message->Severity == DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR ||
                message->Severity == DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION) {
                ++validationErrors_;
            }
        }
        dxgiInfoQueue_->ClearStoredMessages(DxgiDebugDxgi);
    }
}

// 메시지 구조체를 담을 16 byte 정렬 버퍼다. 할당 실패면 null이다.
void* D3D12Device::Scratch(SIZE_T length) const noexcept {
    const usize needed = static_cast<usize>(length) + 16;
    if (messageScratch_.Size() < needed) {
        auto resized = messageScratch_.TryResize(needed);
        if (!resized) {
            return nullptr;
        }
    }
    const uptr address = reinterpret_cast<uptr>(messageScratch_.Data());
    return reinterpret_cast<void*>((address + 15u) & ~static_cast<uptr>(15u));
}

void D3D12Device::RetireObject(ComRef<IUnknown>&& object, int64 srv, int64 rtv, int64 dsv,
                               SlotHandle heap) noexcept {
    Retired entry;
    entry.object = Move(object);
    // 제출된 작업은 모두 signal 뒤에 있으므로 마지막 signal 값이 끝나면 더 쓰지 않는다. 기록
    // 중이면 아직 제출하지 않은 프레임이 쓸 수 있으므로 그 프레임의 signal 값을 EndFrame에서
    // 정한다.
    entry.fenceValue = fenceValue_;
    entry.pending = recording_ && !computeRecording_;
    // compute queue도 같은 규칙이다(RG4): 마지막 compute 제출, 기록 중이면 그 기록의 값.
    entry.computeFenceValue = computeFenceValue_;
    entry.computePending = recording_ && computeRecording_;
    // 아직 제출하지 않은 업로드 기록(생성 초기 데이터·갱신·readback 복사)이 이 자원을 쓸 수
    // 있다. 그 묶음이 제출될 때 fence를 받는다.
    entry.uploadPending = uploadOpen_;
    entry.srv = srv;
    entry.rtv = rtv;
    entry.dsv = dsv;
    entry.heap = heap;
    // 용량은 자원을 만들 때 미리 확보했으므로(ReserveRetireCapacity) 실패하지 않는다.
    auto pushed = retired_.TryPushBack(Move(entry));
    if (!pushed) {
        platform::Fatal("D3D12 retire list capacity invariant violated");
    }
}

// 살아 있는 자원이 모두 retire되어도 목록이 자라지 않도록 용량을 확보한다. Retire가 할당 실패로
// GPU 사용 중인 자원을 놓는 경로를 없앤다.
Result<void> D3D12Device::ReserveRetireCapacity(usize extra) noexcept {
    const usize live = buffers_.Size() + bufferViews_.Size() + textures_.Size() +
                       pipelines_.Size() + readbacks_.Size() +
                       swapChains_.Size() * SwapChainRetireEntries + heaps_.Size();
    return retired_.TryEnsureCapacity(retired_.Size() + live + extra);
}

void D3D12Device::AssignPendingRetires(uint64 value) noexcept {
    for (Retired& entry : retired_) {
        if (entry.pending) {
            entry.pending = false;
            entry.fenceValue = value;
        }
    }
}

void D3D12Device::ProcessRetired() noexcept {
    const uint64 completed = fence_->GetCompletedValue();
    const uint64 computeCompleted = ComputeCompletedValue();
    ring_.Reclaim(completed);
    staging_->Reclaim(completed);
    for (usize index = retired_.Size(); index > 0; --index) {
        Retired& entry = retired_[index - 1];
        if (entry.pending || entry.uploadPending || entry.fenceValue > completed ||
            entry.computePending || entry.computeFenceValue > computeCompleted) {
            continue;
        }
        if (entry.srv >= 0) {
            srvs_.Free(static_cast<uint32>(entry.srv));
        }
        if (entry.rtv >= 0) {
            rtvs_.Free(static_cast<uint32>(entry.rtv));
        }
        if (entry.dsv >= 0) {
            dsvs_.Free(static_cast<uint32>(entry.dsv));
        }
        // placed 자원을 놓은 뒤 heap의 placed 수를 줄인다(GX12, 마지막이면 heap도 놓는다).
        const SlotHandle heap = entry.heap;
        auto removed = retired_.TryRemoveAtSwap(index - 1);
        (void)removed;
        if (!heap.IsNull()) {
            ReleasePlacement(heap);
        }
    }
}

} // namespace rocks::graphics::d3d12

namespace rocks::graphics {

Result<UniquePtr<Device>> CreateD3D12Device(Allocator& allocator, const DeviceDesc& desc,
                                            DeviceCreateReport* report) noexcept {
    auto created = UniquePtr<d3d12::D3D12Device>::TryMake(allocator, allocator);
    if (!created) {
        if (report != nullptr) {
            *report = DeviceCreateReport{};
            report->code = created.Error();
        }

        return Result<UniquePtr<Device>>::Failure(created.Error());
    }

    auto initialized = created.Value()->Initialize(desc);
    if (!initialized) {
        // 생성 실패 로그는 backend를 고르는 호출자가 report로 남긴다(LoggingSystem 13절).
        if (report != nullptr) {
            created.Value()->FillReport(initialized.Error(), *report);
        }

        return Result<UniquePtr<Device>>::Failure(initialized.Error());
    }

    if (report != nullptr) {
        *report = DeviceCreateReport{};
    }

    const DeviceInfo info = created.Value()->Info();
    Log::Info(LogGraphicsD3D12,
              "Created {Backend} device on {Adapter} (validation {Validation}, GPU-based "
              "validation {GpuValidation})",
              BackendName(info.backend), info.adapterName, info.validationEnabled,
              info.gpuValidationEnabled);
    return Result<UniquePtr<Device>>::Success(UniquePtr<Device>(Move(created.Value())));
}

} // namespace rocks::graphics
