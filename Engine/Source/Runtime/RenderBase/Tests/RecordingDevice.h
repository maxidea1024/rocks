#pragma once

#include <Core/Containers/Array.h>
#include <Graphics/Device.h>
#include <Graphics/DeviceOwnership.h>
#include <Graphics/FenceTimeline.h>

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
        SetConstants,
        Draw,
        DrawIndexed,
        EndFrame,
        RetireBuffer,
        RetireTexture,
        RetirePipeline,
        RetireSwapChain,
        WaitIdle
    };

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
    // 있다.
    void Complete(uint64 value) noexcept;

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

    // --- Device ---
    graphics::DeviceInfo Info() const noexcept override;
    Result<graphics::SwapChainHandle>
    CreateSwapChain(const graphics::SwapChainDesc& desc) noexcept override;
    Result<void> ResizeSwapChain(graphics::SwapChainHandle swapChain, uint32 width,
                                 uint32 height) noexcept override;
    Result<graphics::BufferHandle> CreateBuffer(const graphics::BufferDesc& desc,
                                                Span<const uint8> initial) noexcept override;
    Result<graphics::TextureHandle> CreateTexture(const graphics::TextureDesc& desc,
                                                  Span<const uint8> pixels) noexcept override;
    Result<graphics::PipelineHandle>
    CreatePipeline(const graphics::PipelineDesc& desc) noexcept override;
    Result<graphics::CommandContext*>
    BeginFrame(graphics::SwapChainHandle swapChain,
               const graphics::ClearColor& clear) noexcept override;
    Result<graphics::CommandContext*>
    BeginOffscreen(graphics::TextureHandle target,
                   const graphics::ClearColor& clear) noexcept override;
    Result<graphics::GpuFencePoint> EndFrame() noexcept override;
    Result<void> ReadPixels(graphics::TextureHandle target, Span<uint8> out) noexcept override;
    void Retire(graphics::BufferHandle buffer) noexcept override;
    void Retire(graphics::TextureHandle texture) noexcept override;
    void Retire(graphics::PipelineHandle pipeline) noexcept override;
    void Retire(graphics::SwapChainHandle swapChain) noexcept override;
    void ReclaimCompleted() noexcept override;
    Result<void> WaitIdle() noexcept override;
    graphics::GpuFencePoint LastSubmittedFence() const noexcept override;
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

private:
    struct Retired {
        uint64 fenceValue = 0;
        bool pending = false;
    };

    // --- CommandContext ---
    void SetPipeline(graphics::PipelineHandle pipeline) noexcept override;
    void SetVertexBuffers(uint32 firstStream,
                          Span<const graphics::VertexBufferBinding> buffers) noexcept override;
    void SetIndexBuffer(graphics::BufferHandle buffer, graphics::IndexFormat format,
                        uint64 offset) noexcept override;
    void SetTexture(graphics::TextureHandle texture) noexcept override;
    void SetConstants(const graphics::DrawConstants& constants) noexcept override;
    void Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept override;
    void DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex) noexcept override;

    void Record(Op op, uint64 value = 0) noexcept;
    void Defer() noexcept;

    graphics::FenceTimeline timeline_;
    graphics::DeviceOwnership ownership_;
    SlotArray<uint8> buffers_;
    SlotArray<uint8> textures_;
    SlotArray<uint8> pipelines_;
    SlotArray<uint8> swapChains_;
    Array<Retired> retired_;
    Call calls_[MaxCalls];
    usize callCount_ = 0;
    platform::Mutex gate_;
    platform::ManualResetEvent acquireOpen_{true};
    uint64 fenceValue_ = 0;
    uint64 reclaimed_ = 0;
    uint64 ownershipTransfers_ = 0;
    uint64 failedResizes_ = 0;
    bool hold_ = false;
    bool recording_ = false;
};

} // namespace rocks::render::testing
