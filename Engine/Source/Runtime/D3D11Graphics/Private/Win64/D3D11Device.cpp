#include "D3D11Caps.h"
#include "D3D11DeviceImpl.h"

#include <Core/Diagnostics/Log.h>
#include <Platform/Atomics.h>

#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks::graphics {

namespace d3d11 {

namespace {

// 요청하는 feature level이다. 11_0이 최소다(UE D3D11RHI와 같다).
constexpr D3D_FEATURE_LEVEL FeatureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

} // namespace

bool FenceEventSource::WaitSubmission(uint64 value, void* token) noexcept {
    (void)token;
    uint64 completed = fence->GetCompletedValue();
    if (completed == RemovedFenceValue) {
        return false;
    }

    if (completed >= value) {
        return true;
    }

    if (FAILED(fence->SetEventOnCompletion(value, event))) {
        return false;
    }

    WaitForSingleObject(event, INFINITE);
    completed = fence->GetCompletedValue();
    return completed != RemovedFenceValue;
}

D3D11Device::D3D11Device(Allocator& allocator) noexcept
    : allocator_(allocator), adapterName_(allocator), lastError_(allocator), buffers_(allocator),
      textures_(allocator), pipelines_(allocator), swapChains_(allocator), readbacks_(allocator),
      bufferViews_(allocator), retired_(allocator), dedicated_(allocator), uploadOps_(allocator),
      messageScratch_(allocator), timeline_(allocator, NewDeviceEpoch(), QueueKind::Graphics),
      ring_(allocator), timestamps_(allocator) {
}

D3D11Device::~D3D11Device() {
    // 장치는 소유 스레드(또는 소유자가 없을 때 아무 스레드)에서 파괴한다.
    if (!ownership_.IsOwner() && !ownership_.Acquire()) {
        platform::Fatal("graphics device destroyed while another thread owns it");
    }

    if (immediate_ && fence_ && fenceEvent_ != nullptr) {
        auto idle = WaitIdle();
        (void)idle;
    }

    // 모든 제출이 끝났거나(WaitIdle) 장치가 제거되어 관찰 스레드가 곧 빠져나온다.
    timeline_.StopWatcher();
    timeline_.Close();
    if (staging_) {
        staging_->Clear();
    }

    uploadOps_.Clear();
    readbacks_.Clear();
    retired_.Clear();
    for (const DedicatedUpload& upload : dedicated_) {
        (void)allocator_.Deallocate(upload.memory);
    }

    dedicated_.Clear();
    swapChains_.Clear();
    textures_.Clear();
    pipelines_.Clear();
    bufferViews_.Clear();
    buffers_.Clear();
    if (ringData_ != nullptr) {
        (void)allocator_.Deallocate(ringData_);
        ringData_ = nullptr;
    }

    if (immediate_) {
        immediate_->ClearState();
        immediate_->Flush();
    }

    if (fenceEvent_ != nullptr) {
        CloseHandle(fenceEvent_);
    }

    if (watcherEvent_ != nullptr) {
        CloseHandle(watcherEvent_);
    }
}

Result<void> D3D11Device::Initialize(const DeviceDesc& desc) noexcept {
    // DXGI debug는 DXGI SDK layer가 있을 때만 켜진다. 없으면 일반 factory로 계속한다.
    HRESULT created = E_FAIL;
    if (desc.enableValidation) {
        created = CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(factory_.Out()));
    }

    if (FAILED(created)) {
        created = CreateDXGIFactory2(0, IID_PPV_ARGS(factory_.Out()));
    }

    if (FAILED(created)) {
        return Fail("CreateDXGIFactory2", created);
    }

    // 후보별 실패를 모아 생성 보고에 남긴다.
    Diagnostic attempts;
    ComRef<IDXGIAdapter1> adapter;
    if (!desc.preferSoftware) {
        ComRef<IDXGIFactory6> factory6;
        if (SUCCEEDED(factory_->QueryInterface(IID_PPV_ARGS(factory6.Out())))) {
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

                if (CreateOnAdapter(candidate.Get(), desc.enableValidation)) {
                    adapter = Move(candidate);
                    break;
                }

                attempts.Text("adapter ")
                    .Number(index)
                    .Text(": ")
                    .Text(lastError_.CStr())
                    .Text("; ");
            }
        }
    }

    if (!device_) {
        ComRef<IDXGIAdapter1> warp;
        const HRESULT enumerated = factory_->EnumWarpAdapter(IID_PPV_ARGS(warp.Out()));
        if (FAILED(enumerated)) {
            auto failed = Fail("EnumWarpAdapter", enumerated);
            (void)failed;
        } else if (CreateOnAdapter(warp.Get(), desc.enableValidation)) {
            adapter = Move(warp);
            software_ = true;
        }

        if (!device_) {
            attempts.Text("WARP: ").Text(lastError_.CStr());
            // 마지막 후보(WARP)까지 필수 인터페이스가 없으면 미지원, 그 밖은 의존 실패다.
            const ErrorCode code =
                featureMissing_ ? ErrorCode::Unsupported : ErrorCode::DependencyFailed;
            auto assigned = lastError_.TryAssign(StringView(attempts.CStr(), attempts.Length()));
            (void)assigned;
            failureTruncated_ = failureTruncated_ || attempts.Truncated();
            return Result<void>::Failure(code);
        }
    }

    DXGI_ADAPTER_DESC1 info = {};
    adapter->GetDesc1(&info);
    // UTF-16 128자(Description 크기)는 UTF-8로 최대 384 byte다.
    char name[512] = {};
    const int written = WideCharToMultiByte(CP_UTF8, 0, info.Description, -1, name,
                                            static_cast<int>(sizeof(name)), nullptr, nullptr);
    if (written > 1) {
        auto assigned = adapterName_.TryAssign(StringView(name, static_cast<usize>(written - 1)));
        (void)assigned;
    }

    if (validation_) {
        ConfigureValidationQueues();
    }

    const HRESULT deferred = device_->CreateDeferredContext(0, deferred_.Out());
    if (FAILED(deferred)) {
        return Fail("CreateDeferredContext", deferred);
    }

    // marker는 기록 context(deferred)에서 얻는다. 없으면 debugMarkers를 거짓으로 공개한다.
    if (FAILED(deferred_->QueryInterface(IID_PPV_ARGS(annotation_.Out())))) {
        annotation_.Reset();
    }

    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    watcherEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fenceEvent_ == nullptr || watcherEvent_ == nullptr) {
        return Fail("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
    }

    fenceSource_.fence = fence_.Get();
    fenceSource_.event = watcherEvent_;
    auto watching = timeline_.StartWatcher(fenceSource_, "D3D11Fence");
    if (!watching) {
        return Fail("fence watcher thread creation failed");
    }

    auto states = CreateSharedStates();
    if (!states) {
        return states;
    }

    // 장치 능력(GX1)과 형식 능력은 장치 수명 동안 바뀌지 않으므로 한 번 질의해
    // 둔다(PixelFormat.ko.md PF4). MSAA 표본 수가 2 이상이면 Multisample 능력이다.
    caps_ = QueryDeviceCaps(device_.Get(), adapter.Get());
    for (uint32 number = 0; number <= MaxPixelFormatNumber; ++number) {
        const PixelFormat format = static_cast<PixelFormat>(number);
        PixelFormatCaps formatCaps = QueryFormatCaps(device_.Get(), format);
        sampleCounts_[number] = QuerySampleCounts(device_.Get(), format, formatCaps);
        if (sampleCounts_[number] > SampleCountBit(1)) {
            formatCaps = formatCaps | PixelFormatCaps::Multisample;
        }

        formatCaps_[number] = formatCaps;
    }

    // 업로드 ring(GX8): 시스템 메모리 하나를 나눠 쓴다.
    const uint64 ringBytes =
        desc.uploadRingBytes != 0 ? desc.uploadRingBytes : DefaultUploadRingBytes;
    auto ringReset = ring_.Reset(ringBytes);
    if (!ringReset) {
        return ringReset;
    }

    auto ringMemory = allocator_.Allocate(static_cast<usize>(ringBytes), UploadAlignment);
    if (!ringMemory) {
        return Result<void>::Failure(ringMemory.Error());
    }

    ringData_ = static_cast<uint8*>(ringMemory.Value());
    stagingSource_.allocator = &allocator_;
    const uint64 stagingBudget =
        desc.stagingBudgetBytes != 0 ? desc.stagingBudgetBytes : DefaultStagingBudgetBytes;
    auto pool =
        UniquePtr<StagingPool>::TryMake(allocator_, allocator_, stagingSource_, stagingBudget);
    if (!pool) {
        return Result<void>::Failure(pool.Error());
    }

    staging_ = Move(pool.Value());
    InitializeTimestamps();

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
    DrainValidationMessages();
    return Result<void>::Success();
}

Result<void> D3D11Device::CreateOnAdapter(IDXGIAdapter1* adapter, bool requestDebug) noexcept {
    device_.Reset();
    immediate_.Reset();
    fence_.Reset();
    featureMissing_ = false;
    ComRef<ID3D11Device> device;
    ComRef<ID3D11DeviceContext> context;
    // SINGLETHREADED를 쓰지 않는다: 자원 생성을 다른 스레드에서도
    // 부른다(concurrentResourceCreation).
    UINT flags = requestDebug ? D3D11_CREATE_DEVICE_DEBUG : 0u;
    D3D_FEATURE_LEVEL level = {};
    HRESULT result = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                                       FeatureLevels, ARRAYSIZE(FeatureLevels), D3D11_SDK_VERSION,
                                       device.Out(), &level, context.Out());
    if (result == DXGI_ERROR_SDK_COMPONENT_MISSING && requestDebug) {
        // debug layer(Graphics Tools)가 설치되지 않았다. 검증 없이 계속한다.
        flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
        result = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, FeatureLevels,
                                   ARRAYSIZE(FeatureLevels), D3D11_SDK_VERSION, device.Out(),
                                   &level, context.Out());
    }

    if (FAILED(result)) {
        return Fail("D3D11CreateDevice", result);
    }

    // GX7(다른 스레드의 GPU 완료 대기)에는 ID3D11Fence가 필요하다.
    // 아래 셋이 없으면 계약을 지킬 수 없으므로 이 어댑터는 미지원이다(featureMissing_).
    result = device->QueryInterface(IID_PPV_ARGS(device_.Out()));
    if (FAILED(result)) {
        featureMissing_ = true;
        return Fail("ID3D11Device5 (Windows 10 1703 or later)", result);
    }

    result = context->QueryInterface(IID_PPV_ARGS(immediate_.Out()));
    if (FAILED(result)) {
        device_.Reset();
        featureMissing_ = true;
        return Fail("ID3D11DeviceContext4 (Windows 10 1703 or later)", result);
    }

    // WDDM 2.0 미만 드라이버는 monitored fence가 없다.
    result = device_->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.Out()));
    if (FAILED(result)) {
        device_.Reset();
        immediate_.Reset();
        featureMissing_ = true;
        return Fail("ID3D11Device5::CreateFence (WDDM 2.0 driver)", result);
    }

    validation_ = (flags & D3D11_CREATE_DEVICE_DEBUG) != 0;
    return Result<void>::Success();
}

Result<void> D3D11Device::CreateSharedStates() noexcept {
    // 앞면은 화면 반시계다(UE D3D11State.cpp:211, ShaderSystem 7.2절 8항, D3D12와 같다). 컬
    // 모드마다 상태 하나를 만든다. 깊이는 아직 PipelineDesc에 없다(쓰지 않는다).
    constexpr CullMode Modes[CullModeCount] = {CullMode::None, CullMode::Back, CullMode::Front};
    HRESULT result = S_OK;
    for (CullMode mode : Modes) {
        D3D11_RASTERIZER_DESC raster = {};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = RasterCullMode(mode);
        raster.FrontCounterClockwise = TRUE;
        raster.DepthClipEnable = TRUE;
        result =
            device_->CreateRasterizerState(&raster, rasterizers_[static_cast<usize>(mode)].Out());
        if (FAILED(result)) {
            return Fail("CreateRasterizerState", result);
        }
    }

    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depth.StencilEnable = FALSE;
    result = device_->CreateDepthStencilState(&depth, depthDisabled_.Out());
    if (FAILED(result)) {
        return Fail("CreateDepthStencilState", result);
    }

    D3D11_BLEND_DESC blend = {};
    D3D11_RENDER_TARGET_BLEND_DESC& target = blend.RenderTarget[0];
    target.BlendEnable = FALSE;
    target.SrcBlend = D3D11_BLEND_ONE;
    target.DestBlend = D3D11_BLEND_ZERO;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_ZERO;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    result = device_->CreateBlendState(&blend, opaqueBlend_.Out());
    if (FAILED(result)) {
        return Fail("CreateBlendState", result);
    }

    // 소스 알파로 섞는다(D3D12 파이프라인과 같은 식).
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    result = device_->CreateBlendState(&blend, alphaBlend_.Out());
    if (FAILED(result)) {
        return Fail("CreateBlendState", result);
    }

    // 그리기 상수(D3D12 루트 상수 20개 대응)다. SetConstants마다 WRITE_DISCARD로 갈아 끼운다.
    static_assert(sizeof(DrawConstants) % 16 == 0, "DrawConstants must be a multiple of 16 bytes");
    D3D11_BUFFER_DESC constants = {};
    constants.ByteWidth = sizeof(DrawConstants);
    constants.Usage = D3D11_USAGE_DYNAMIC;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    result = device_->CreateBuffer(&constants, nullptr, constants_.Out());
    if (FAILED(result)) {
        return Fail("CreateBuffer (draw constants)", result);
    }

    return Result<void>::Success();
}

void D3D11Device::FillReport(ErrorCode code, DeviceCreateReport& report) const noexcept {
    report = DeviceCreateReport{};
    report.code = code;
    report.nativeCode = static_cast<int64>(static_cast<uint32>(failureResult_));
    const bool stageCut = CopyTruncated(report.stage, sizeof(report.stage), failureStage_);
    const bool detailCut = CopyTruncated(report.detail, sizeof(report.detail), lastError_.CStr());
    report.truncated = failureTruncated_ || stageCut || detailCut;
}

DeviceInfo D3D11Device::Info() const noexcept {
    DeviceInfo info;
    info.backend = Backend::D3D11;
    info.adapterName = adapterName_.View();
    info.deviceEpoch = timeline_.DeviceEpoch();
    // 생성은 free-threaded 장치 호출이고 갱신은 내부 잠금 아래 업로드 기록에 쌓여 소유 스레드의
    // 다음 제출이 실행한다.
    info.concurrentResourceCreation = true;
    info.validationEnabled = validation_;
    info.software = software_;
    // 프레임 slot마다 TIMESTAMP_DISJOINT로 감싸므로 끊김을 알아챈다. calibration은 GPU가 쉬는
    // 동안 잰 timestamp와 앞뒤 CPU 시각이다.
    info.timestamps = timestampFrequency_ != 0;
    info.timestampFrequency = timestampFrequency_;
    info.timestampDisjointDetection = info.timestamps;
    info.clockCalibration = info.timestamps;
    info.debugMarkers = annotation_.Get() != nullptr;
    info.apiVersion = device_ ? static_cast<uint32>(device_->GetFeatureLevel()) : 0;
    return info;
}

GpuFencePoint D3D11Device::LastSubmittedFence() const noexcept {
    return timeline_.LastSubmitted();
}

Result<bool> D3D11Device::IsFenceComplete(GpuFencePoint point) const noexcept {
    return timeline_.IsComplete(point);
}

Result<platform::WaitResult> D3D11Device::WaitFence(GpuFencePoint point,
                                                    uint32 timeoutMs) noexcept {
    return timeline_.Wait(point, timeoutMs);
}

Result<void> D3D11Device::NotifyFence(GpuFencePoint point, FenceCallback callback,
                                      void* context) noexcept {
    return timeline_.Notify(point, callback, context);
}

bool D3D11Device::IsLost() const noexcept {
    return timeline_.IsLost();
}

bool D3D11Device::IsOwnerThread() const noexcept {
    return ownership_.IsOwner();
}

Result<void> D3D11Device::ReleaseOwnership() noexcept {
    if (!ownership_.IsOwner()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (recording_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    return ownership_.Release();
}

Result<void> D3D11Device::AcquireOwnership() noexcept {
    return ownership_.Acquire();
}

uint64 D3D11Device::RejectedCalls() const noexcept {
    return ownership_.Rejected();
}

StringView D3D11Device::LastError() const noexcept {
    return lastError_.View();
}

uint64 D3D11Device::IgnoredDraws() const noexcept {
    return ignoredDraws_;
}

uint64 D3D11Device::ValidationErrorCount() const noexcept {
    DrainValidationMessages();
    return validationErrors_;
}

usize D3D11Device::PendingRetireCount() const noexcept {
    platform::LockGuard lock(gate_);
    return retired_.Size();
}

void D3D11Device::ReclaimCompleted() noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    ProcessRetired();
}

Result<void> D3D11Device::Fail(const char* stage, HRESULT result) noexcept {
    DetectLost(result);
    Diagnostic message;
    message.Text(stage).Text(" failed (HRESULT ").Hex(static_cast<uint32>(result)).Text(")");
    auto assigned = lastError_.TryAssign(StringView(message.CStr(), message.Length()));
    (void)assigned;
    failureTruncated_ = CopyTruncated(failureStage_, sizeof(failureStage_), stage);
    failureResult_ = result;
    // 실패는 호출자가 결과·LastError로 받아 결정하는 쪽이 기록한다(LoggingSystem 13절). 장치는 손실
    // 전이(DetectLost)만 직접 남긴다.
    return Result<void>::Failure(timeline_.IsLost() ? ErrorCode::Destroyed
                                                    : ErrorCode::DependencyFailed);
}

Result<void> D3D11Device::Fail(const char* message) noexcept {
    auto assigned = lastError_.TryAssign(StringView(message, platform::CStringLength(message)));
    (void)assigned;
    failureTruncated_ = CopyTruncated(failureStage_, sizeof(failureStage_), message);
    failureResult_ = S_OK;
    return Result<void>::Failure(timeline_.IsLost() ? ErrorCode::Destroyed
                                                    : ErrorCode::DependencyFailed);
}

Result<void> D3D11Device::Reject(const Diagnostic& message) noexcept {
    auto assigned = lastError_.TryAssign(StringView(message.CStr(), message.Length()));
    (void)assigned;
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

void D3D11Device::DetectLost(HRESULT failure) noexcept {
    if (!device_) {
        return;
    }

    const HRESULT reason = device_->GetDeviceRemovedReason();
    if (IsDeviceRemovedResult(failure) || reason != S_OK) {
        timeline_.MarkLost();
        // 손실 전이 때 한 번 남긴다(여러 스레드가 같은 손실을 보아도 한 번).
        if (platform::AtomicExchange<platform::MemoryOrder::AcquireRelease>(&lossLogged_, 1u) ==
            0) {
            Log::Error(
                LogGraphicsD3D11,
                "D3D11 device removed on {Adapter}: {Reason} (removed reason HRESULT "
                "{ReasonCode:#x}, failing call HRESULT {HResult:#x}); the device is lost and "
                "every later call fails",
                adapterName_.View(), DeviceRemovedReasonName(reason), static_cast<uint32>(reason),
                static_cast<uint32>(failure));
        }
    }
}

Result<uint64> D3D11Device::SignalQueue() noexcept {
    if (timeline_.IsLost()) {
        return Result<uint64>::Failure(ErrorCode::Destroyed);
    }

    const uint64 value = fenceValue_ + 1;
    const HRESULT signaled = immediate_->Signal(fence_.Get(), value);
    if (FAILED(signaled)) {
        return FailWith<uint64>("ID3D11DeviceContext4::Signal", signaled);
    }

    // D3D11은 명령을 모아 두므로 신호가 GPU에 닿도록 내보낸다(관찰 스레드가 영원히 기다리지 않게).
    immediate_->Flush();
    fenceValue_ = value;
    timeline_.Submitted(value, nullptr);
    return Result<uint64>::Success(value);
}

uint64 D3D11Device::CompletedFence() noexcept {
    const uint64 completed = fence_->GetCompletedValue();
    if (completed == RemovedFenceValue) {
        timeline_.MarkLost();
        return RemovedFenceValue;
    }

    timeline_.Publish(completed);
    return completed;
}

Result<void> D3D11Device::WaitFenceOwner(uint64 value) noexcept {
    if (fence_->GetCompletedValue() < value) {
        const HRESULT armed = fence_->SetEventOnCompletion(value, fenceEvent_);
        if (FAILED(armed)) {
            return Fail("ID3D11Fence::SetEventOnCompletion", armed);
        }

        WaitForSingleObject(fenceEvent_, INFINITE);
    }

    (void)CompletedFence();
    if (timeline_.IsLost()) {
        return Result<void>::Failure(ErrorCode::Destroyed);
    }

    return Result<void>::Success();
}

void D3D11Device::RetireObjects(ComRef<IUnknown> (&objects)[MaxRetiredObjects]) noexcept {
    Retired entry;
    for (usize index = 0; index < MaxRetiredObjects; ++index) {
        entry.objects[index] = Move(objects[index]);
    }

    // 제출된 작업은 모두 signal 뒤에 있으므로 마지막 signal 값이 끝나면 더 쓰지 않는다. 기록 중이면
    // 아직 제출하지 않은 프레임이 쓸 수 있으므로 그 프레임의 signal 값을 EndFrame에서 정한다.
    entry.fenceValue = fenceValue_;
    entry.pending = recording_;
    // 아직 실행하지 않은 업로드 기록(갱신·readback 복사)이 이 자원을 쓸 수 있다.
    entry.uploadPending = uploadOps_.Size() != 0;
    // 용량은 자원을 만들 때 미리 확보했으므로(ReserveRetireCapacity) 실패하지 않는다.
    auto pushed = retired_.TryPushBack(Move(entry));
    if (!pushed) {
        platform::Fatal("D3D11 retire list capacity invariant violated");
    }
}

void D3D11Device::KeepUntilDestruction(ComRef<IUnknown>&& object) noexcept {
    Retired entry;
    entry.fenceValue = RetainUntilDestructionFence;
    entry.objects[0] = Move(object);
    auto pushed = retired_.TryPushBack(Move(entry));
    if (!pushed) {
        // 기록할 공간조차 없으면 해제하지 않고 버린다(GPU가 읽는 중일 수 있는 메모리를 놓는
        // 것보다 낫다).
        (void)entry.objects[0].Detach();
    }
}

Result<void> D3D11Device::ReserveRetireCapacity(usize extra) noexcept {
    const usize live = buffers_.Size() + bufferViews_.Size() + textures_.Size() +
                       pipelines_.Size() + readbacks_.Size() + swapChains_.Size();
    return retired_.TryEnsureCapacity(retired_.Size() + live + extra);
}

void D3D11Device::AssignPendingRetires(uint64 value) noexcept {
    for (Retired& entry : retired_) {
        if (entry.pending) {
            entry.pending = false;
            entry.fenceValue = value;
        }
    }
}

void D3D11Device::AssignPendingUploads(uint64 value) noexcept {
    for (Retired& entry : retired_) {
        if (entry.uploadPending) {
            entry.uploadPending = false;
            entry.fenceValue = value > entry.fenceValue ? value : entry.fenceValue;
        }
    }

    for (DedicatedUpload& upload : dedicated_) {
        if (upload.uploadPending) {
            upload.uploadPending = false;
            upload.fenceValue = value;
        }
    }
}

void D3D11Device::ProcessRetired() noexcept {
    const uint64 completed = CompletedFence();
    ring_.Reclaim(completed);
    staging_->Reclaim(completed);
    for (usize index = retired_.Size(); index > 0; --index) {
        const Retired& entry = retired_[index - 1];
        if (entry.pending || entry.uploadPending || entry.fenceValue > completed) {
            continue;
        }

        auto removed = retired_.TryRemoveAtSwap(index - 1);
        (void)removed;
    }

    for (usize index = dedicated_.Size(); index > 0; --index) {
        const DedicatedUpload& upload = dedicated_[index - 1];
        if (upload.uploadPending || upload.fenceValue > completed) {
            continue;
        }

        (void)allocator_.Deallocate(upload.memory);
        auto removed = dedicated_.TryRemoveAtSwap(index - 1);
        (void)removed;
    }
}

void D3D11Device::ConfigureValidationQueues() noexcept {
    if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(infoQueue_.Out())))) {
        // 오류만 저장해 저장 한도를 경고가 채우지 않게 한다.
        D3D11_MESSAGE_SEVERITY severities[] = {D3D11_MESSAGE_SEVERITY_CORRUPTION,
                                               D3D11_MESSAGE_SEVERITY_ERROR};
        D3D11_INFO_QUEUE_FILTER filter = {};
        filter.AllowList.NumSeverities = ARRAYSIZE(severities);
        filter.AllowList.pSeverityList = severities;
        (void)infoQueue_->PushStorageFilter(&filter);
    }

    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(dxgiInfoQueue_.Out())))) {
        DXGI_INFO_QUEUE_MESSAGE_SEVERITY severities[] = {
            DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR};
        DXGI_INFO_QUEUE_FILTER filter = {};
        filter.AllowList.NumSeverities = ARRAYSIZE(severities);
        filter.AllowList.pSeverityList = severities;
        (void)dxgiInfoQueue_->PushStorageFilter(DxgiDebugDxgi, &filter);
    }
}

void D3D11Device::DrainValidationMessages() const noexcept {
    if (infoQueue_) {
        const UINT64 count = infoQueue_->GetNumStoredMessages();
        for (UINT64 index = 0; index < count; ++index) {
            SIZE_T length = 0;
            if (FAILED(infoQueue_->GetMessage(index, nullptr, &length)) || length == 0) {
                ++validationErrors_;
                continue;
            }

            auto* message = static_cast<D3D11_MESSAGE*>(Scratch(length));
            if (message == nullptr || FAILED(infoQueue_->GetMessage(index, message, &length)) ||
                message->Severity == D3D11_MESSAGE_SEVERITY_ERROR ||
                message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION) {
                // 읽지 못한 메시지도 오류로 센다(필터가 오류만 저장한다).
                ++validationErrors_;
            }

            // 원문을 메시지 ID마다 한 번 남긴다(세기는 위와 같다).
            if (message != nullptr && message->pDescription != nullptr &&
                validationFilter_.First(static_cast<uint32>(message->ID))) {
                Log::Warning(
                    LogGraphicsD3D11Validation, "D3D11 debug layer message {Id}: {Message}",
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

void* D3D11Device::Scratch(SIZE_T length) const noexcept {
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

Result<StagingMemory> SystemStagingSource::CreateStaging(uint64 bytes) noexcept {
    auto memory = allocator->Allocate(static_cast<usize>(bytes), UploadAlignment);
    if (!memory) {
        return Result<StagingMemory>::Failure(memory.Error());
    }

    StagingMemory staging;
    staging.resource = memory.Value();
    staging.data = static_cast<uint8*>(memory.Value());
    staging.size = bytes;
    return Result<StagingMemory>::Success(staging);
}

void SystemStagingSource::DestroyStaging(const StagingMemory& memory) noexcept {
    (void)allocator->Deallocate(memory.resource);
}

} // namespace d3d11

Result<UniquePtr<Device>> CreateD3D11Device(Allocator& allocator, const DeviceDesc& desc,
                                            DeviceCreateReport* report) noexcept {
    auto created = UniquePtr<d3d11::D3D11Device>::TryMake(allocator, allocator);
    if (!created) {
        if (report != nullptr) {
            *report = DeviceCreateReport{};
            report->code = created.Error();
        }

        return Result<UniquePtr<Device>>::Failure(created.Error());
    }

    auto initialized = created.Value()->Initialize(desc);
    if (!initialized) {
        if (report != nullptr) {
            created.Value()->FillReport(initialized.Error(), *report);
        }

        return Result<UniquePtr<Device>>::Failure(initialized.Error());
    }

    if (report != nullptr) {
        *report = DeviceCreateReport{};
    }

    // 생성 실패 로그는 backend를 고르는 호출자가 report로 남긴다(LoggingSystem 13절).
    const DeviceInfo info = created.Value()->Info();
    Log::Info(LogGraphicsD3D11, "Created {Backend} device on {Adapter} (validation {Validation})",
              BackendName(info.backend), info.adapterName, info.validationEnabled);
    return Result<UniquePtr<Device>>::Success(UniquePtr<Device>(Move(created.Value())));
}

} // namespace rocks::graphics
