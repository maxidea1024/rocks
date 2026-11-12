#pragma once

#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <Graphics/GraphicsTypes.h>

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
//   auto device = graphics::CreateD3D12Device(allocator, {.enableValidation = true});
//   auto swapChain = device.Value()->CreateSwapChain({.surface = surface, .width = w, .height =
//   h}); auto vertices = device.Value()->CreateBuffer({.size = sizeof(data)}, bytes); auto pipeline
//   = device.Value()->CreatePipeline(pipelineDesc); auto frame =
//   device.Value()->BeginFrame(swapChain.Value(), {0.1f, 0.1f, 0.1f, 1.0f}); if (frame) {
//       frame.Value()->SetPipeline(pipeline.Value());
//       frame.Value()->SetVertexBuffer(vertices.Value());
//       frame.Value()->Draw(3);
//       auto presented = device.Value()->EndFrame();
//   }
//
// 계약:
// - 생성한 스레드에서만 사용한다(D3D12 큐·OpenGL context 소유 스레드).
// - 프레임은 한 번에 하나만 기록한다. BeginFrame/BeginOffscreen 뒤 EndFrame 전의 다른 Begin은
// Busy다.
// - Destroy는 GPU 사용이 끝난 뒤 실제로 해제한다(D3D12는 프레임 fence 완료 후, OpenGL은 즉시 —
// 드라이버가
//   수명을 관리한다). 파괴한 handle은 stale이며 이후 사용은 무시하거나 StaleHandle이다.
// - 장치 소멸 전에 WaitIdle을 기다리고 모든 자원을 해제한다(소멸자가 대신한다).
class Device {
public:
    virtual ~Device() = default;

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

    // 스왑체인의 다음 백버퍼에 그리기를 시작한다(clear 포함).
    virtual Result<CommandContext*> BeginFrame(SwapChainHandle swapChain,
                                               const ClearColor& clear) noexcept = 0;

    // RenderTarget 텍스처에 그리기를 시작한다(clear 포함).
    virtual Result<CommandContext*> BeginOffscreen(TextureHandle target,
                                                   const ClearColor& clear) noexcept = 0;

    // 기록을 제출한다. BeginFrame이었으면 present한다. 진행 중인 프레임이 없으면 InvalidArgument다.
    virtual Result<void> EndFrame() noexcept = 0;

    // RenderTarget 텍스처를 out(width*height*4바이트, 행 위→아래)으로 읽는다. GPU 완료를 기다린다
    // (시험·캡처용, 프레임 경로에서 쓰지 않는다). 크기가 다르면 InvalidArgument다.
    virtual Result<void> ReadPixels(TextureHandle target, Span<uint8> out) noexcept = 0;

    virtual void Destroy(BufferHandle buffer) noexcept = 0;
    virtual void Destroy(TextureHandle texture) noexcept = 0;
    virtual void Destroy(PipelineHandle pipeline) noexcept = 0;
    virtual void Destroy(SwapChainHandle swapChain) noexcept = 0;

    // 제출한 모든 GPU 작업이 끝날 때까지 기다리고 지연 해제를 처리한다.
    virtual Result<void> WaitIdle() noexcept = 0;

    // 마지막 실패의 진단 문자열(셰이더 컴파일 로그 등)이다. 다음 실패까지 유효하다.
    virtual StringView LastError() const noexcept = 0;

    // 지금까지 무시된 그리기 수(파이프라인·버퍼 미설정, stale handle)다.
    virtual uint64 IgnoredDraws() const noexcept = 0;

    // 검증 계층이 보고한 오류 수다(D3D12 debug layer의 ERROR·CORRUPTION, GL debug output의 ERROR).
    // 검증이 꺼져 있으면 0이다. 시험이 "검증 오류 0"을 확인할 때 쓴다.
    virtual uint64 ValidationErrorCount() const noexcept = 0;
};

} // namespace rocks::graphics
