#pragma once

#include <Core/Containers/Array.h>
#include <Graphics/Device.h>
#include <Graphics/DeviceOwnership.h>
#include <Graphics/FenceTimeline.h>
#include <Graphics/SplitTransitionTracker.h>
#include <Graphics/SubresourceStates.h>
#include <Graphics/StagingPool.h>
#include <Graphics/TimestampRing.h>

namespace rocks::render::testing {

// 기록 backend다(Docs/Design/RenderBase.ko.md 5.7절 "기록 backend 대조"). 장치 호출을 순서대로
// 기록하고 GPU 완료를 시험이 정한다(HoldCompletions). 렌더 lane의 인라인·lane 모드 결과 비교, slot
// 대기, 장치 손실 주입, 소유권 이관 중 표면 작업 시험에 쓴다.
//
// 기록 대상은 상태를 바꾸는 호출(생성·Begin·그리기·제출·Retire·스왑체인)이다.
// 회수(ReclaimCompleted가 실제로 지운 수)·소유권 이관은 fence 완료 시점에 따라 위치가 달라지므로
// 기록 열에 넣지 않고 계수로 센다.
class RecordingDevice final : public graphics::Device, private graphics::CommandContext {
public:
    enum class Op : uint8 {
        CreateSwapChain,
        ResizeSwapChain,
        CreateBuffer,
        CreateTexture,
        CreatePipeline,
        BeginFrame,
        BeginOffscreen,
        SetPipeline,
        SetVertexBuffers,
        SetIndexBuffer,
        SetTexture,
        // value는 받은 byte 수(위 32 bit)와 첫 float × 255(아래 32 bit)다. 거부된 호출은 넣지
        // 않는다.
        SetDrawConstants,
        Draw,
        DrawIndexed,
        CopyPassTarget,
        EndFrame,
        RetireBuffer,
        RetireTexture,
        RetirePipeline,
        RetireSwapChain,
        WaitIdle,
        UpdateBuffer,
        UpdateTexture,
        UploadFromStaging,
        RequestReadback,
        WriteTimestamp,
        PushMarker,
        PopMarker,
        // value는 TransitionLog의 순번이다.
        Transition,
        // 분리 전이 begin·end다. value는 짝 번호다(전이마다의 내용은 TransitionLog).
        BeginTransitions,
        EndTransitions,
        // 텍스처 view 생성이다. value는 (첫 mip << ViewOpMipShift) | mip 수다.
        CreateTextureView,
        // value는 묶은 view의 첫 mip이다(모르는 view면 NumericLimits<uint32>::Max()).
        SetWritableTexture,
        // async compute 기록 시작(PassDesc::queue = AsyncCompute)과 제출이다. EndCompute의 value는
        // compute queue fence 값이다.
        BeginCompute,
        EndCompute,
        // Device::QueueWait다. value는 QueueWaitValue(queue, point)다.
        QueueWait,
        // transient heap·placed 자원(GX12)이다. CreatePlaced의 value는 heap 안 offset, Acquire의
        // value는 획득한 자원의 slot 번호(버퍼면 AcquireBufferBit을 더한다)다.
        CreateTransientHeap,
        CreatePlaced,
        Acquire
    };

    // Op::Acquire value에서 버퍼 표시 bit다.
    static constexpr uint64 AcquireBufferBit = 1ull << 32;
    // 기록 장치 placed 자원의 정렬(byte)이다.
    static constexpr uint64 TransientAlignment = 256;

    // Op::QueueWait의 value다: 기다리는 queue가 AsyncCompute면 맨 위 bit, 아래는 기다릴 지점의
    // 값이다.
    static constexpr uint64 QueueWaitComputeBit = 1ull << 63;

    static constexpr uint64 QueueWaitValue(graphics::QueueKind queue, uint64 pointValue) noexcept {
        return pointValue | (queue == graphics::QueueKind::AsyncCompute ? QueueWaitComputeBit : 0);
    }

    // 기록한 전이의 단계다. Full은 Transition, Begin·End는 분리 전이의 두 끝이다.
    enum class TransitionPhase : uint8 {
        Full,
        Begin,
        End
    };

    // 기록한 명시 전이 하나다(Op::Transition). texture가 false면 버퍼다. index는 handle의 slot
    // 번호다.
    struct TransitionCall {
        bool texture = true;
        uint32 index = 0;
        graphics::SubresourceRange range;
        graphics::ResourceAccess before = graphics::ResourceAccess::Unknown;
        graphics::ResourceAccess after = graphics::ResourceAccess::Unknown;
        TransitionPhase phase = TransitionPhase::Full;
        // 분리 전이의 짝 번호다(Full은 0).
        uint32 split = 0;

        bool operator==(const TransitionCall&) const noexcept = default;
    };

    // Op::CreateTextureView 값에서 첫 mip의 bit 위치다.
    static constexpr uint32 ViewOpMipShift = 16;

    // 기록 장치가 추적하는 텍스처 mip 수 상한이다(Graphics 텍스처 mip 상한과 같다).
    static constexpr uint32 MaxTrackedMips = 16;

    struct Call {
        Op op = Op::WaitIdle;
        uint64 value = 0;

        bool operator==(const Call&) const noexcept = default;
    };

    static constexpr usize MaxCalls = 1024;

    explicit RecordingDevice(Allocator& allocator) noexcept;
    ~RecordingDevice() override;

    // true면 EndFrame의 GPU 완료를 시험이 Complete로 게시할 때까지 미룬다. false면 제출 즉시
    // 완료다.
    void HoldCompletions(bool hold) noexcept;

    // value까지의 제출을 끝낸다(부른 스레드에서 fence 통지가 불린다). 어느 스레드에서나 부를 수
    // 있다. CompleteCompute는 compute queue의 제출이다.
    void Complete(uint64 value) noexcept;
    void CompleteCompute(uint64 value) noexcept;

    // compute queue(DeviceFeatures::asyncCompute·efficientAsyncCompute)를 공개한다(RenderBase 8절
    // RG4). 기본은 없음이다. 장치를 쓰기 전에 부른다.
    //
    // compute queue 검사(결정적 경쟁 검출): 텍스처·버퍼마다 마지막으로 접근한 (queue, 그 기록의
    // fence 값)을 기억하고, 다른 queue가 그 자원에 접근할 때 그 queue가 QueueWait로 앞 접근의
    // 제출을 기다리지 않았으면 검증 오류다. async compute 기록의 그리기와 compute 상태 밖 전이도
    // 검증 오류다(실제 장치는 무시한다).
    void SetAsyncCompute(bool available, bool efficient = true) noexcept;

    // transient heap(DeviceFeatures::transientHeaps)을 공개한다(RenderBase 8절 GX12). 기본은
    // 없음이다. 장치를 쓰기 전에 부른다.
    //
    // alias 겹침 검출: placed 자원마다 (heap, 구간, 활성)을 두고, 획득(ResourceTransition::acquire,
    // attachment acquire)이 그 자원을 활성으로, 같은 heap에서 구간이 겹치는 다른 자원을 비활성으로
    // 바꾼다. 비활성 placed 자원을 쓰면(전이·묶기·attachment·복사) 검증 오류다. 처음 만든 placed
    // 자원도 획득 전에는 비활성이다.
    void SetTransientHeaps(bool available) noexcept;

    // compute queue에 제출한 마지막 값이다.
    uint64 SubmittedCompute() const noexcept;

    // 지금까지 제출한 모든 제출을 끝낸다.
    void CompleteAll() noexcept;

    // 장치 손실을 주입한다.
    void InjectLost() noexcept;

    // 다음 AcquireOwnership이 OpenAcquire까지 막히게 한다(소유권 이관 중 작업 시험).
    void GateAcquire() noexcept;
    void OpenAcquire() noexcept;

    uint64 Submitted() const noexcept;

    Span<const Call> Calls() const noexcept {
        return Span<const Call>(calls_, callCount_);
    }

    // 명시 전이 기록이다(GX5). 전이마다 하나다.
    Span<const TransitionCall> TransitionLog() const noexcept {
        return transitions_.View();
    }

    // 장치가 추적한 텍스처 mip의 접근 상태다(시험이 최종 상태를 단언한다). 모르는 handle은
    // Unknown이다.
    //
    // 분리 전이(D3D12와 같은 규칙): begin은 짝만 열고, end가 before 대조와 상태 변경을 한다. 짝
    // 검사: 모르는·이미 끝난 짝의 end는 IgnoredDraws, begin과 다른 목록의 end·끝나지 않은
    // 짝(EndFrame)·열린 짝의 subresource를 건드리는 Transition·begin은 검증 오류다.
    //
    // 추적 규칙(D3D12 backend와 같다): 만든 텍스처는 SrvGraphics, 만든 버퍼는 VertexOrIndex에서
    // 시작한다. BeginPass(BeginOffscreen 포함)는 attachment subresource를 RenderTarget·
    // DepthWrite·DepthRead로 바꾼다(attachment 전이, 기록은 Op::BeginOffscreen). EndFrame은
    // 상태를 바꾸지 않는다. Transition은 before가 Unknown이 아니면 추적 상태와 같은지
    // 검사하고(다르면 검증 오류) after로 바꾼다. SetTexture는 SrvGraphics가 아닌 텍스처를 묶으면
    // 검증 오류다(이 장치는 암묵 전이를 하지 않는다).
    graphics::ResourceAccess TextureAccess(graphics::TextureHandle texture, uint32 mip = 0,
                                           uint32 slice = 0) const noexcept;
    graphics::ResourceAccess BufferAccess(graphics::BufferHandle buffer) const noexcept;

    uint64 Reclaimed() const noexcept {
        return reclaimed_;
    }

    uint64 OwnershipTransfers() const noexcept {
        return ownershipTransfers_;
    }

    // ResizeSwapChain이 실패한 수다(소유권 이관 중 거부 0 확인).
    uint64 FailedResizes() const noexcept {
        return failedResizes_;
    }

    // false면 timestamp·clock calibration을 지원하지 않는 장치로 공개한다(GLES 확장 없음과 같은
    // "없음" 공개 시험). 기본은 true다. 장치를 쓰기 전에 부른다.
    void SetTimestampsSupported(bool supported) noexcept {
        timestampsSupported_ = supported;
    }

    // CalibrateClocks가 돌려줄 쌍을 고정한다(gpuFrequency 0이면 고정을 푼다). GPU 구간 변환을
    // 정확히 단언하는 시험이 쓴다.
    void SetCalibration(const graphics::ClockCalibration& calibration) noexcept {
        fixedCalibration_ = calibration;
    }

    // CalibrateClocks가 성공한 횟수다.
    uint64 Calibrations() const noexcept {
        return calibrations_;
    }

    // 기록 backend의 timestamp 주파수(초당 tick)다. WriteTimestamp마다 TimestampTickStep씩 증가한
    // tick을 낸다.
    static constexpr uint64 TimestampFrequency = 1000000000;
    static constexpr uint64 TimestampTickStep = 100;
    // 기본 calibration 쌍의 오차 반경(ns)이다(기록 장치는 쌍을 한 번에 읽는다고 가정한다).
    static constexpr uint64 CalibrationUncertaintyNanoseconds = 250;

    // --- Device ---
    graphics::DeviceInfo Info() const noexcept override;
    const graphics::DeviceCaps& Caps() const noexcept override;
    uint32 GetPixelFormatSampleCounts(graphics::PixelFormat format) const noexcept override;
    Result<graphics::PipelineHandle>
    CreateComputePipeline(const graphics::ComputePipelineDesc& desc) noexcept override;
    Result<graphics::BufferViewHandle>
    CreateBufferView(graphics::BufferHandle buffer,
                     const graphics::BufferViewDesc& desc) noexcept override;
    Result<void> ReadBuffer(graphics::BufferHandle buffer, uint64 offset,
                            Span<uint8> out) noexcept override;
    Result<graphics::SwapChainHandle>
    CreateSwapChain(const graphics::SwapChainDesc& desc) noexcept override;
    Result<void> ResizeSwapChain(graphics::SwapChainHandle swapChain, uint32 width,
                                 uint32 height) noexcept override;
    Result<graphics::BufferHandle> CreateBuffer(const graphics::BufferDesc& desc,
                                                Span<const uint8> initial) noexcept override;
    Result<graphics::TextureSubresources>
    GetTextureSubresources(graphics::TextureHandle texture) const noexcept override;
    Result<graphics::TextureHandle> CreateTexture(const graphics::TextureDesc& desc,
                                                  Span<const uint8> pixels) noexcept override;
    Result<graphics::TransientHeapHandle>
    CreateTransientHeap(const graphics::TransientHeapDesc& desc) noexcept override;
    void RetireTransientHeap(graphics::TransientHeapHandle heap) noexcept override;
    Result<graphics::TransientPlacement>
    GetTexturePlacement(const graphics::TextureDesc& desc) const noexcept override;
    Result<graphics::TransientPlacement>
    GetBufferPlacement(const graphics::BufferDesc& desc) const noexcept override;
    Result<graphics::TextureHandle>
    CreatePlacedTexture(graphics::TransientHeapHandle heap, uint64 offset,
                        const graphics::TextureDesc& desc) noexcept override;
    Result<graphics::BufferHandle>
    CreatePlacedBuffer(graphics::TransientHeapHandle heap, uint64 offset,
                       const graphics::BufferDesc& desc) noexcept override;
    // 범위·access 규칙만 본다(ResolveTextureViewRange, 차원은 2D·2D 배열로 본다). 형식은 보지
    // 않는다.
    Result<graphics::TextureHandle>
    CreateTextureView(graphics::TextureHandle texture,
                      const graphics::TextureViewDesc& desc) noexcept override;
    Result<graphics::PipelineHandle>
    CreatePipeline(const graphics::PipelineDesc& desc) noexcept override;
    // 어느 스레드에서나 부를 수 있다(셰이더 집합의 첫 사용 생성 경쟁 시험). 호출을 기록하지 않고
    // CreatedShaders·RetiredShaders로 센다.
    Result<graphics::ShaderHandle>
    CreateShader(const graphics::ShaderCodeDesc& desc) noexcept override;
    Result<graphics::CommandContext*>
    BeginFrame(graphics::SwapChainHandle swapChain,
               const graphics::ClearColor& clear) noexcept override;
    Result<graphics::CommandContext*> BeginPass(const graphics::PassDesc& pass) noexcept override;
    Result<graphics::GpuFencePoint> EndFrame() noexcept override;
    Result<void> ReadPixels(graphics::TextureHandle target, Span<uint8> out) noexcept override;
    void Retire(graphics::BufferHandle buffer) noexcept override;
    void Retire(graphics::BufferViewHandle view) noexcept override;
    void Retire(graphics::TextureHandle texture) noexcept override;
    void Retire(graphics::PipelineHandle pipeline) noexcept override;
    void Retire(graphics::ShaderHandle shader) noexcept override;
    void Retire(graphics::SwapChainHandle swapChain) noexcept override;
    void ReclaimCompleted() noexcept override;
    Result<void> WaitIdle() noexcept override;
    Result<void> UpdateBuffer(graphics::BufferHandle buffer, uint64 offset,
                              Span<const uint8> data) noexcept override;
    Result<void> UpdateTexture(graphics::TextureHandle texture, uint32 mipLevel,
                               Span<const uint8> pixels) noexcept override;
    Result<graphics::GpuFencePoint> SubmitUploads() noexcept override;
    graphics::UploadStats UploadStatistics() const noexcept override;
    Result<graphics::StagingBuffer> AcquireStaging(uint64 bytes) noexcept override;
    Result<void> UploadTextureFromStaging(graphics::TextureHandle texture, uint32 mipLevel,
                                          graphics::StagingHandle staging,
                                          uint64 offset) noexcept override;
    Result<void> UploadBufferFromStaging(graphics::BufferHandle buffer, uint64 destinationOffset,
                                         graphics::StagingHandle staging, uint64 sourceOffset,
                                         uint64 size) noexcept override;
    void ReleaseStaging(graphics::StagingHandle staging) noexcept override;
    graphics::StagingStats StagingStatistics() const noexcept override;
    Result<graphics::ReadbackRequest>
    RequestReadback(graphics::TextureHandle target) noexcept override;
    Result<bool> TryReadback(graphics::ReadbackHandle readback, Span<uint8> out) noexcept override;
    void ReleaseReadback(graphics::ReadbackHandle readback) noexcept override;
    usize CollectTimestamps(Span<graphics::TimestampSample> out) noexcept override;
    Result<graphics::ClockCalibration>
    CalibrateClocks(graphics::QueueKind queue = graphics::QueueKind::Graphics) noexcept override;

    // readback이 돌려줄 byte 값이다(시험이 결과를 알아본다).
    void SetReadbackFill(uint8 value) noexcept {
        readbackFill_ = value;
    }
    graphics::GpuFencePoint LastSubmittedFence(
        graphics::QueueKind queue = graphics::QueueKind::Graphics) const noexcept override;
    Result<void> QueueWait(graphics::QueueKind queue,
                           graphics::GpuFencePoint point) noexcept override;
    Result<bool> IsFenceComplete(graphics::GpuFencePoint point) const noexcept override;
    Result<platform::WaitResult> WaitFence(graphics::GpuFencePoint point,
                                           uint32 timeoutMs) noexcept override;
    Result<void> NotifyFence(graphics::GpuFencePoint point, graphics::FenceCallback callback,
                             void* context) noexcept override;
    bool IsLost() const noexcept override;
    bool IsOwnerThread() const noexcept override;
    Result<void> ReleaseOwnership() noexcept override;
    Result<void> AcquireOwnership() noexcept override;
    uint64 RejectedCalls() const noexcept override;
    StringView LastError() const noexcept override;
    uint64 IgnoredDraws() const noexcept override;
    uint64 ValidationErrorCount() const noexcept override;
    usize PendingRetireCount() const noexcept override;

    // CreateShader를 arrivals번째 호출이 들어올 때까지 붙잡는다(명시 gate — 경쟁하는 생성이 모두
    // 장치 생성까지 간 뒤에 게시하게 한다). 0이면 끈다.
    void HoldShaderCreation(uint32 arrivals) noexcept;
    // 만든·Retire한 장치 셰이더 수와 지금 살아 있는 수다.
    uint64 CreatedShaders() const noexcept;
    uint64 RetiredShaders() const noexcept;
    usize LiveShaders() const noexcept;
    // 마지막으로 만든 파이프라인의 VS handle이다.
    graphics::ShaderHandle LastPipelineVertexShader() const noexcept {
        return lastVertexShader_;
    }

    // CreatePipeline 호출 수(실패 포함)다.
    uint64 PipelineCreateCalls() const noexcept {
        return pipelineCreateCalls_;
    }

private:
    // 회수 대기 하나다: queue마다 마지막 사용 값과, 열린 기록이 그 queue였는지다.
    struct Retired {
        uint64 fenceValue = 0;
        bool pending = false;
        uint64 computeFenceValue = 0;
        bool computePending = false;
    };

    // placed 자원 하나의 heap 구간과 활성 여부다(slot 번호로 찾는다, heap이 null이면 placed 아님).
    struct PlacedState {
        SlotHandle heap;
        uint64 offset = 0;
        uint64 bytes = 0;
        bool active = false;
    };

    struct HeapState {
        graphics::TransientHeapKind kind = graphics::TransientHeapKind::Buffers;
        uint64 bytes = 0;
    };

    // 자원 하나를 마지막으로 접근한 기록의 queue와 fence 값이다(0이면 아직 없음).
    struct QueueUse {
        graphics::QueueKind queue = graphics::QueueKind::Graphics;
        uint64 value = 0;
    };

    struct Readback {
        uint64 fenceValue = 0;
        uint64 bytes = 0;
    };

    // subresource(mip × slice)마다 추적한 상태다. view(CreateTextureView)면 상태 표는 비고 원
    // 텍스처와 범위를 가진다.
    struct TextureState {
        graphics::SubresourceStates<graphics::ResourceAccess> states;
        // attachment subresource 계산용 차원이다(3D는 mip 하나가 subresource 하나).
        graphics::TextureDimension dimension = graphics::TextureDimension::Texture2D;
        bool view = false;
        SlotHandle source;
        graphics::ResolvedSubresourceRange range;
        graphics::ViewAccess access = graphics::ViewAccess::Read;
    };

    // view가 묶이는 원 텍스처 범위의 상태가 모두 allowed 가운데 하나인지 본다(아니면 검증 오류).
    void CheckViewStates(const TextureState& view, graphics::ResourceAccess allowed) noexcept;

    // CPU 메모리 staging이다.
    class CpuStaging final : public graphics::StagingSource {
    public:
        Result<graphics::StagingMemory> CreateStaging(uint64 bytes) noexcept override;
        void DestroyStaging(const graphics::StagingMemory& memory) noexcept override;

        Allocator* allocator = nullptr;
    };

    // 기록한 tick을 pool 칸에서 읽는다(GPU 완료 뒤에만 Harvest가 부른다).
    class TickSource final : public graphics::TimestampSource {
    public:
        graphics::TimestampReadState ReadTimestamps(uint32 first, uint32 count,
                                                    uint64* ticks) noexcept override;

        Array<uint64>* pool = nullptr;
    };

    uint64 SubmitInternal() noexcept;

    // 지점 point의 queue 타임라인이다(compute queue가 없으면 compute 지점은 null).
    const graphics::FenceTimeline* TimelineOf(graphics::GpuFencePoint point) const noexcept;

    // 열린 기록이 slot index의 자원에 접근한다(compute queue 경쟁 검사, 위 SetAsyncCompute).
    void TouchResource(Array<QueueUse>& uses, uint32 index) noexcept;
    void TouchTexture(SlotHandle texture) noexcept;
    void TouchBuffer(SlotHandle buffer) noexcept;

    // async compute 기록에서 그리기 계열 호출이 오면 검증 오류를 센다. 그 기록이면 true다.
    bool RejectOnComputeQueue() noexcept;

    // placed 자원 표(textures·buffers의 slot 번호)다. 없는 칸은 placed 아님이다.
    PlacedState* PlacedOf(Array<PlacedState>& table, uint32 index) noexcept;
    // placed 자원을 만들 때 그 칸을 채운다(heap 검사 뒤).
    Result<void> MarkPlaced(Array<PlacedState>& table, uint32 index, SlotHandle heap, uint64 offset,
                            uint64 bytes) noexcept;
    // 획득: 그 자원을 활성으로, 같은 heap의 겹치는 다른 자원을 비활성으로 바꾼다.
    void AcquirePlaced(bool buffer, uint32 index) noexcept;
    // 비활성 placed 자원을 쓰면 검증 오류를 센다.
    void CheckPlacedActive(Array<PlacedState>& table, uint32 index) noexcept;
    // placement 검사(heap·종류·정렬·범위)다.
    Result<void> CheckPlacement(graphics::TransientHeapHandle heap, uint64 offset,
                                const graphics::TransientPlacement& placement) const noexcept;

    // --- CommandContext ---
    void SetPipeline(graphics::PipelineHandle pipeline) noexcept override;
    void SetVertexBuffers(uint32 firstStream,
                          Span<const graphics::VertexBufferBinding> buffers) noexcept override;
    void SetIndexBuffer(graphics::BufferHandle buffer, graphics::IndexFormat format,
                        uint64 offset) noexcept override;
    void SetTexture(graphics::TextureHandle texture) noexcept override;
    void SetBufferView(uint32 slot, graphics::BufferViewHandle view) noexcept override;
    void SetWritableBufferView(uint32 slot, graphics::BufferViewHandle view) noexcept override;
    void SetWritableTexture(uint32 slot, graphics::TextureHandle view) noexcept override;
    void DrawIndirect(graphics::BufferHandle arguments, uint64 offset) noexcept override;
    void DrawIndexedIndirect(graphics::BufferHandle arguments, uint64 offset) noexcept override;
    void Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) noexcept override;
    void DispatchIndirect(graphics::BufferHandle arguments, uint64 offset) noexcept override;
    void SetViewport(const graphics::Viewport& viewport) noexcept override;
    void SetScissor(const graphics::ScissorRect& scissor) noexcept override;
    void SetStencilReference(uint32 reference) noexcept override;
    Result<void> CopyBuffer(graphics::BufferHandle destination, uint64 destinationOffset,
                            graphics::BufferHandle source, uint64 sourceOffset,
                            uint64 size) noexcept override;
    Result<void> CopyTexture(graphics::TextureHandle destination,
                             graphics::TextureHandle source) noexcept override;
    Result<void> ClearWritableBufferView(graphics::BufferViewHandle view,
                                         uint32 value) noexcept override;
    Result<void> SetDrawConstants(Span<const uint8> bytes) noexcept override;
    void Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept override;
    void DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex) noexcept override;
    Result<void> CopyPassTarget(graphics::TextureHandle destination) noexcept override;
    Result<uint64> WriteTimestamp() noexcept override;
    void PushMarker(StringView name) noexcept override;
    void PopMarker() noexcept override;
    void Transition(Span<const graphics::ResourceTransition> transitions) noexcept override;
    graphics::SplitTransition
    BeginTransitions(Span<const graphics::ResourceTransition> transitions) noexcept override;
    void EndTransitions(graphics::SplitTransition split,
                        Span<const graphics::ResourceTransition> transitions) noexcept override;
    // 전이를 추적 상태에 적용하고 기록한다(Transition·EndTransitions 공용).
    void ApplyTransitions(Span<const graphics::ResourceTransition> transitions,
                          TransitionPhase phase, uint32 split) noexcept;
    // 열린 분리 전이의 subresource와 겹치면 검증 오류를 센다.
    void CheckPendingOverlap(Span<const graphics::ResourceTransition> transitions) noexcept;

    void Record(Op op, uint64 value = 0) noexcept;
    void CheckDeclared(graphics::ResourceAccess tracked,
                       graphics::ResourceAccess declared) noexcept;
    void Defer() noexcept;

    graphics::FenceTimeline timeline_;
    graphics::FenceTimeline computeTimeline_;
    graphics::DeviceCaps caps_;
    uint64 computeFenceValue_ = 0;
    // 지금 기록의 queue다.
    graphics::QueueKind recordingQueue_ = graphics::QueueKind::Graphics;
    // queue마다 QueueWait로 기다린 상대 queue 값의 최댓값이다(QueueKind 순번).
    uint64 waited_[2] = {};
    Array<QueueUse> textureUses_;
    Array<QueueUse> bufferUses_;
    SlotArray<HeapState> heaps_;
    Array<PlacedState> placedTextures_;
    Array<PlacedState> placedBuffers_;
    graphics::DeviceOwnership ownership_;
    SlotArray<graphics::ResourceAccess> buffers_;
    Allocator& allocator_;
    SlotArray<TextureState> textures_;
    Array<TransitionCall> transitions_;
    // 열린 분리 전이 짝과 그 전이다.
    graphics::SplitTransitionTracker splits_;
    struct PendingTransition {
        uint32 split = 0;
        graphics::ResourceTransition transition;
    };
    Array<PendingTransition> pendingSplits_;
    uint64 validationErrors_ = 0;
    uint64 ignoredDraws_ = 0;
    // 파이프라인마다 선언한 그리기별 상수 byte 수다(SetDrawConstants 크기 검사).
    SlotArray<uint32> pipelines_;
    // 묶인 파이프라인이다(stale·null이면 SetDrawConstants가 거부한다).
    graphics::PipelineHandle boundPipeline_;
    SlotArray<uint8> swapChains_;
    // 장치 셰이더(단계)다. shaderGate_가 지킨다.
    platform::Mutex shaderGate_;
    SlotArray<uint8> shaders_;
    platform::ManualResetEvent shaderRelease_{true};
    uint32 shaderHold_ = 0;
    uint32 shaderArrivals_ = 0;
    uint64 createdShaders_ = 0;
    uint64 retiredShaders_ = 0;
    graphics::ShaderHandle lastVertexShader_;
    uint64 pipelineCreateCalls_ = 0;
    Array<Retired> retired_;
    SlotArray<Readback> readbacks_;
    CpuStaging stagingSource_;
    graphics::StagingPool staging_;
    uint8 readbackFill_ = 0;
    graphics::TimestampRing timestamps_;
    Array<uint64> timestampPool_;
    TickSource tickSource_;
    uint64 tickClock_ = 0;
    uint32 markerDepth_ = 0;
    bool timestampsSupported_ = true;
    Call calls_[MaxCalls];
    usize callCount_ = 0;
    graphics::ClockCalibration fixedCalibration_;
    uint64 calibrations_ = 0;
    platform::Mutex gate_;
    platform::ManualResetEvent acquireOpen_{true};
    uint64 fenceValue_ = 0;
    uint64 reclaimed_ = 0;
    uint64 ownershipTransfers_ = 0;
    uint64 failedResizes_ = 0;
    bool hold_ = false;
    bool recording_ = false;
    // 지금 패스의 resolve 대상이다(EndFrame이 그 subresource를 ResolveDestination으로 바꾼다).
    struct PendingResolve {
        SlotHandle texture;
        uint32 mipLevel = 0;
        uint32 slice = 0;
    };
    PendingResolve resolves_[graphics::MaxColorTargets];
    uint32 resolveCount_ = 0;
};

} // namespace rocks::render::testing
