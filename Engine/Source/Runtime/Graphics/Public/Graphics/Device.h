#pragma once

#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <Graphics/DeviceTypes.h>
#include <Platform/Sync.h>

namespace rocks::graphics {

// 한 프레임(또는 offscreen 패스)의 그리기 명령 기록기다. Device::BeginFrame·BeginOffscreen이
// 돌려주며 EndFrame까지만 유효하다. 기록 순서대로 실행된다.
class CommandContext {
public:
    virtual void SetPipeline(PipelineHandle pipeline) noexcept = 0;
    virtual void SetVertexBuffer(BufferHandle buffer) noexcept = 0;
    // 텍스처 슬롯 0에 묶는다. null handle이면 1x1 흰색 텍스처를 쓴다.
    virtual void SetTexture(TextureHandle texture) noexcept = 0;
    virtual void SetConstants(const DrawConstants& constants) noexcept = 0;
    // 묶인 파이프라인·정점 버퍼로 삼각형 목록을 그린다. 파이프라인이나 버퍼가 없으면 무시하고
    // 진단 계수만 올린다.
    virtual void Draw(uint32 vertexCount, uint32 firstVertex = 0) noexcept = 0;

protected:
    ~CommandContext() = default;
};

// 그래픽 장치 계약이다. backend 모듈의 생성 함수(CreateD3D12Device·CreateOpenGLDevice 등)로 만든다.
//
// 사용 예:
//   auto created = graphics::CreateD3D12Device(allocator, {.enableValidation = true});
//   graphics::Device& device = *created.Value();
//   auto swapChain = device.CreateSwapChain({.surface = surface, .width = w, .height = h});
//   auto vertices = device.CreateBuffer({.size = sizeof(data)}, bytes);
//   auto pipeline = device.CreatePipeline(pipelineDesc);
//   auto frame = device.BeginFrame(swapChain.Value(), {0.1f, 0.1f, 0.1f, 1.0f});
//   if (frame) {
//       frame.Value()->SetPipeline(pipeline.Value());
//       frame.Value()->SetVertexBuffer(vertices.Value());
//       frame.Value()->Draw(3);
//       auto submitted = device.EndFrame();          // GpuFencePoint
//   }
//   device.Retire(vertices.Value());                 // 마지막 사용 fence 뒤에 해제된다
//
// 스레드 모델(RenderBase 5.4절 GX11):
// - 소유 스레드 하나가 장치를 쓴다. 처음 소유자는 장치를 만든 스레드다. 제출·present·fence
// 신호·장치
//   상태 변경(EndFrame·WaitIdle·Retire·ReclaimCompleted·스왑체인)은 소유 스레드 전용이다.
// - 기록 객체(CommandContext)는 기록하는 스레드마다 하나이며 소유 스레드가 제출한다. 지금은 소유
//   스레드의 BeginFrame·BeginOffscreen이 돌려주는 하나뿐이다(병렬 기록은 GX4·RG3).
// - 자원·파이프라인 생성의 스레드 안전 여부는 DeviceInfo::concurrentResourceCreation이 공개한다.
//   false면 생성도 소유 스레드 전용이다.
// - "스레드 안전" 표시가 붙은 함수(Info·fence API·소유권 조회)는 어느 스레드에서나 부를 수 있다.
// - 소유권 이관(GX10): 소유 스레드가 ReleaseOwnership으로 놓고 새 스레드가 AcquireOwnership으로
//   잡는다. 그 사이와 다른 스레드에서의 소유 스레드 전용 호출은 Debug·Development 구성에서
//   WrongThread로 거부한다(반환값이 없는 호출은 무시하고 RejectedCalls를 올린다). Release 구성은
//   검사를 생략한다. 기록 객체의 그리기 호출은 검사하지 않는다(소유 스레드의 Begin만 돌려준다).
//
// 계약:
// - 프레임은 한 번에 하나만 기록한다. BeginFrame/BeginOffscreen 뒤 EndFrame 전의 다른 Begin은
//   Busy다.
// - 제출·완료·회수(계획 11.2절, RenderBase 4.4절 GX7): EndFrame·업로드·WaitIdle의 queue 제출은
//   모두 GpuFencePoint를 남긴다. Retire는 자원과 그 descriptor(D3D12 heap 칸)·API 객체를 마지막으로
//   쓴 제출의 fence가 끝난 뒤에 해제한다. last-use는 장치 내부 집계가 정한다: 기록 중인(아직
//   제출하지 않은) 프레임이 쓸 수 있으면 그 프레임의 fence, 아니면 Retire 시점까지 제출한 마지막
//   fence다. OpenGL도 fence sync로 같은 규칙을 따른다(즉시 삭제하지 않는다). 기록 중에도 Retire할
//   수 있으며 이미 기록한 명령은 그 자원으로 실행된다(기록 중인 스왑체인을 Retire하면 그 프레임의
//   present만 생략한다). Retire한 handle은 stale이며 이후 사용은 무시하거나 StaleHandle이다.
// - 실제 해제는 소유 스레드의 EndFrame·WaitIdle·ReclaimCompleted가 한다. 렌더 lane은 fence 완료
//   통지(NotifyFence)를 받아 ReclaimCompleted를 부른다.
// - 장치 손실(D3D12 device removed, OpenGL fence 대기 실패): 장치는 손실 상태가 되고 기다리던 fence
//   대기·통지가 모두 Destroyed로 깨어난다. 이후 BeginFrame·EndFrame도 Destroyed다. 장치를 다시
//   만들어야 한다(새 deviceEpoch).
// - 기록 중에도 자원을 만들 수 있다(텍스처 업로드는 동기 완료된다).
// - 장치 소멸 전에 WaitIdle을 기다리고 모든 자원을 해제한다(소멸자가 대신한다).
class Device {
public:
    virtual ~Device() = default;

    // 스레드 안전.
    virtual DeviceInfo Info() const noexcept = 0;

    // 창 표면에 스왑체인을 만든다. 크기 0이나 빈 표면은 InvalidArgument, OS·API 실패는
    // DependencyFailed다.
    virtual Result<SwapChainHandle> CreateSwapChain(const SwapChainDesc& desc) noexcept = 0;

    // 창 크기에 맞춰 버퍼를 다시 만든다. 0 크기는 InvalidArgument(최소화 중에는 호출하지 않는다).
    virtual Result<void> ResizeSwapChain(SwapChainHandle swapChain, uint32 width,
                                         uint32 height) noexcept = 0;

    // 정점 버퍼를 만들고 initial을 복사한다. initial 크기가 desc.size와 다르면 InvalidArgument다.
    virtual Result<BufferHandle> CreateBuffer(const BufferDesc& desc,
                                              Span<const uint8> initial) noexcept = 0;

    // 텍스처를 만든다. Sampled는 pixels(행 위→아래, 빈틈 없는 width*4바이트 행)가 필요하고,
    // RenderTarget은 pixels가 비어 있어야 한다. 잘못된 크기·형식은 InvalidArgument다.
    virtual Result<TextureHandle> CreateTexture(const TextureDesc& desc,
                                                Span<const uint8> pixels) noexcept = 0;

    // 셰이더를 컴파일하고 파이프라인을 만든다. 셰이더 컴파일 실패는 DependencyFailed다(진단 로그는
    // LastError로 읽는다).
    virtual Result<PipelineHandle> CreatePipeline(const PipelineDesc& desc) noexcept = 0;

    // 스왑체인의 다음 백버퍼에 그리기를 시작한다(clear 포함). 장치 손실 뒤에는 Destroyed다.
    virtual Result<CommandContext*> BeginFrame(SwapChainHandle swapChain,
                                               const ClearColor& clear) noexcept = 0;

    // RenderTarget 텍스처에 그리기를 시작한다(clear 포함). 장치 손실 뒤에는 Destroyed다.
    virtual Result<CommandContext*> BeginOffscreen(TextureHandle target,
                                                   const ClearColor& clear) noexcept = 0;

    // 기록을 graphics queue에 제출하고 그 완료 지점을 돌려준다. BeginFrame이었으면 present한다.
    // 진행 중인 프레임이 없으면 InvalidArgument다. 제출은 했지만 present가 실패하면 실패를
    // 돌려주며 제출의 완료 지점은 LastSubmittedFence로 읽는다. 장치 손실이 드러나면 Destroyed다.
    virtual Result<GpuFencePoint> EndFrame() noexcept = 0;

    // RenderTarget 텍스처를 out(width*height*4바이트, 행 위→아래)으로 읽는다. GPU 완료를 기다린다
    // (시험·캡처용, 프레임 경로에서 쓰지 않는다). 크기가 다르면 InvalidArgument다.
    virtual Result<void> ReadPixels(TextureHandle target, Span<uint8> out) noexcept = 0;

    // 자원을 회수 대기로 넘긴다(위 "제출·완료·회수"). 모르는·이미 회수한 handle은 무시한다.
    virtual void Retire(BufferHandle buffer) noexcept = 0;
    virtual void Retire(TextureHandle texture) noexcept = 0;
    virtual void Retire(PipelineHandle pipeline) noexcept = 0;
    virtual void Retire(SwapChainHandle swapChain) noexcept = 0;

    // fence가 끝난 회수 대기 자원을 해제하고 끝난 제출의 기록 저장소를 재사용 목록에 돌린다.
    // 기다리지 않는다.
    virtual void ReclaimCompleted() noexcept = 0;

    // 제출한 모든 GPU 작업이 끝날 때까지 기다리고 지연 해제를 처리한다. 기록 중이면 아직 제출하지
    // 않은 그 프레임은 기다리지 않으며 그 프레임이 쓸 수 있는 자원도 해제하지 않는다. 장치 손실이면
    // Destroyed다.
    virtual Result<void> WaitIdle() noexcept = 0;

    // --- 스레드 안전 fence API(어느 스레드에서나 부를 수 있다) ---

    // 마지막으로 제출한 graphics queue 지점이다. 제출이 없으면 null 지점이다.
    virtual GpuFencePoint LastSubmittedFence() const noexcept = 0;

    // point가 GPU에서 끝났는지다. 실패: 다른 장치(epoch)·queue의 지점 WrongContext, 제출되지 않은
    // 값 InvalidArgument, 끝나기 전 장치 손실 Destroyed. null 지점은 끝난 것으로 본다.
    virtual Result<bool> IsFenceComplete(GpuFencePoint point) const noexcept = 0;

    // point가 끝날 때까지 기다린다(timeoutMs가 지나면 TimedOut 값). 실패는 IsFenceComplete와 같고
    // 기다리는 동안 장치가 손실되면 Destroyed로 깨어난다.
    virtual Result<platform::WaitResult>
    WaitFence(GpuFencePoint point, uint32 timeoutMs = platform::InfiniteTimeout) noexcept = 0;

    // point가 결정되면 callback을 정확히 한 번 부른다(FenceCallback 계약). Core 작업 이벤트로 잇는
    // 경로는 Graphics/GpuFenceEvent.h다. 실패: IsFenceComplete의 인자 오류, 장치 소멸 중
    // ShuttingDown, 등록 칸 할당 실패는 그 오류다(실패면 callback을 부르지 않는다).
    virtual Result<void> NotifyFence(GpuFencePoint point, FenceCallback callback,
                                     void* context) noexcept = 0;

    // 장치 손실 상태인지다.
    virtual bool IsLost() const noexcept = 0;

    // --- 소유권(GX10, 스레드 안전) ---

    // 이 스레드가 장치 소유 스레드인지다.
    virtual bool IsOwnerThread() const noexcept = 0;

    // 소유 스레드가 소유권을 놓는다. OpenGL은 이 스레드에서 context를 뗀다. 그 뒤
    // AcquireOwnership할 때까지 소유자가 없다. 실패: 소유 스레드가 아니면 WrongThread, 기록 중이면
    // Busy.
    //
    // 사용 예(렌더 lane 시작):
    //   device.ReleaseOwnership();                          // 게임 lane
    //   lane.Post([&]() noexcept { device.AcquireOwnership(); });   // 렌더 lane
    virtual Result<void> ReleaseOwnership() noexcept = 0;

    // 소유자가 없는 장치를 이 스레드가 잡는다. OpenGL은 이 스레드에 context를 붙인다. 이미 이
    // 스레드가 소유자면 성공이다. 실패: 다른 스레드가 소유 중이면 Busy, context를 붙이지 못하면
    // DependencyFailed(소유자 없음으로 남는다).
    virtual Result<void> AcquireOwnership() noexcept = 0;

    // 소유 스레드가 아닌 곳의 호출을 거부한 누적 수다(Debug·Development, 진단·시험).
    virtual uint64 RejectedCalls() const noexcept = 0;

    // --- 진단 ---

    // 마지막 실패의 진단 문자열(셰이더 컴파일 로그 등)이다. 다음 실패까지 유효하다.
    virtual StringView LastError() const noexcept = 0;

    // 지금까지 무시된 그리기 수(파이프라인·버퍼 미설정, stale handle)다.
    virtual uint64 IgnoredDraws() const noexcept = 0;

    // 장치를 만든 뒤 검증 계층이 보고한 오류의 누적 수다(D3D12 debug layer·DXGI debug의
    // ERROR·CORRUPTION, GL debug output의 ERROR·UNDEFINED_BEHAVIOR). 검증이 꺼져 있으면 0이다.
    // 시험이 "검증 오류 0"을 확인할 때 쓴다.
    virtual uint64 ValidationErrorCount() const noexcept = 0;

    // 회수 대기 중인 자원 수다(진단·시험: 해제 누락 0 확인).
    virtual usize PendingRetireCount() const noexcept = 0;
};

} // namespace rocks::graphics
