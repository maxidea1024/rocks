#pragma once

#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <Graphics/DeviceCaps.h>
#include <Graphics/DeviceTypes.h>
#include <Graphics/ResourceAccess.h>
#include <Platform/Sync.h>

namespace rocks::graphics {

// 한 프레임(또는 offscreen 패스)의 그리기 명령 기록기다. Device::BeginFrame·BeginPass가
// 돌려주며 EndFrame까지만 유효하다. 기록 순서대로 실행된다.
class CommandContext {
public:
    // 파이프라인을 묶는다. 이미 묶인 정점 버퍼는 새 파이프라인 배치의 stride로 다시 묶인다.
    virtual void SetPipeline(PipelineHandle pipeline) noexcept = 0;

    // firstStream부터 차례로 정점 버퍼를 묶는다(stream 0–7, offset은 버퍼 안 byte 위치). 범위 밖
    // stream은 무시한다. offset은 4 byte 배수로 둔다: Vulkan은 속성 주소가 성분 크기 배수여야 해서
    // 아니면 그 stream을 묶지 않은 것으로 보고 그리기를 무시한다(IgnoredDraws).
    //
    // 사용 예(위치 stream 0, 인스턴스 stream 1):
    //   const VertexBufferBinding buffers[] = {{positions, 0}, {instances, 0}};
    //   commands.SetVertexBuffers(0, buffers);
    //   commands.Draw(6, instanceCount);
    virtual void SetVertexBuffers(uint32 firstStream,
                                  Span<const VertexBufferBinding> buffers) noexcept = 0;

    // index 버퍼를 묶는다(BufferUsage::Index로 만든 버퍼, offset은 byte 위치). offset은 index 크기
    // 배수로 둔다: Vulkan은 아니면 index 버퍼가 없는 것으로 보고 그리기를 무시한다(IgnoredDraws).
    virtual void SetIndexBuffer(BufferHandle buffer, IndexFormat format,
                                uint64 offset = 0) noexcept = 0;

    // 텍스처 슬롯 0에 묶는다. null handle이면 1x1 흰색 텍스처를 쓴다.
    virtual void SetTexture(TextureHandle texture) noexcept = 0;

    // 버퍼 view slot(0 – MaxDrawBufferViews-1)에 읽기 view를 묶는다(PipelineDesc 고정 배치, GX2).
    // null handle이면 slot을 비운다. 그리기는 파이프라인이 읽는 slot마다 같은 종류(Typed는 같은 값
    // 계열)의 view가 묶였을 때만 실행하고, 아니면 무시하고 IgnoredDraws를 올린다
    // (DrawBufferSlotsSatisfied). 범위 밖 slot, stale handle, ReadWrite view(그래픽 단계 UAV는
    // 없다)도 그 slot을 비운다. 묶인 view의 버퍼를 Retire해도 view가 자원을 붙든다.
    //
    // 사용 예(PS가 t1의 Buffer<float4>를 읽는다):
    //   commands.SetPipeline(pipeline);
    //   commands.SetBufferView(0, lightsView);
    //   commands.Draw(3);
    //
    // 버퍼 상태: 셰이더 읽기 전 접근은 SrvGraphics다. 렌더 그래프 밖 그리기는 장치가 업로드 뒤
    // 읽기를 보장한다(D3D12는 버퍼 암묵 승격, Vulkan은 업로드 기록 끝 barrier, D3D11·OpenGL ES는
    // 드라이버).
    virtual void SetBufferView(uint32 slot, BufferViewHandle view) noexcept = 0;
    virtual void SetConstants(const DrawConstants& constants) noexcept = 0;

    // 기본도형을 그린다(파이프라인 topology, 정점 vertexCount개 × 인스턴스 instanceCount개,
    // firstVertex부터). base
    // instance는 0으로 고정한다(SV_InstanceID의 base 포함 여부가 backend마다 달라서다). 정점 입력이
    // 없는 배치는 정점 버퍼 없이 그린다. 파이프라인이 없거나 배치가 선언한 stream에 정점 버퍼가
    // 묶이지 않았으면 무시하고 IgnoredDraws를 올린다(Development 진단).
    virtual void Draw(uint32 vertexCount, uint32 instanceCount = 1,
                      uint32 firstVertex = 0) noexcept = 0;

    // index 버퍼로 그린다(firstIndex부터 indexCount개). base vertex·base instance는 0이다. index
    // 버퍼가 없으면 Draw와 같이 무시한다. index 값의 범위 검사는 하지 않는다(장치가 index 내용을
    // CPU에 남기지 않는다).
    virtual void DrawIndexed(uint32 indexCount, uint32 instanceCount = 1,
                             uint32 firstIndex = 0) noexcept = 0;

    // --- 명령 기록 확장(RenderBase 4.4절 GX4, Renderer RQ3) ---
    //
    // 간접 그리기·dispatch 인자는 Indirect 용도 버퍼의 offset(4 byte 배수)에 놓인
    // DrawIndirectArguments·DrawIndexedIndirectArguments·DispatchIndirectArguments 하나다. 인자를
    // GPU가 썼다면 앞에 그 버퍼의 IndirectArgs 전이가 필요하다(렌더 그래프 밖 기록은 장치가
    // 메운다). 아래 그리기·dispatch는 조건(파이프라인 종류·묶은 자원·인자 범위)이 맞지 않으면
    // 무시하고 IgnoredDraws를 올린다.

    // Draw와 같은 조건에서 buffer[offset]의 DrawIndirectArguments로 그린다.
    virtual void DrawIndirect(BufferHandle arguments, uint64 offset) noexcept = 0;

    // DrawIndexed와 같은 조건에서 buffer[offset]의 DrawIndexedIndirectArguments로 그린다.
    // OpenGL ES는 간접 인자의 firstIndex에 index 버퍼 offset을 더할 곳이 없어 SetIndexBuffer
    // offset이 0일 때만 그린다(아니면 무시하고 IgnoredDraws).
    virtual void DrawIndexedIndirect(BufferHandle arguments, uint64 offset) noexcept = 0;

    // 묶인 compute 파이프라인(CreateComputePipeline)으로 그룹 groupCountX × Y × Z개를 실행한다.
    // 그룹 수는 DeviceLimits::computeGroupCount 안이어야 한다. 읽는 slot·쓰는 slot에 맞는 view가
    // 묶여야 한다(SetBufferView·SetWritableBufferView). 색·깊이 attachment가 있는 패스 안에서도
    // 부를 수 있다(Vulkan은 render pass를 닫고 다시 연다).
    virtual void Dispatch(uint32 groupCountX, uint32 groupCountY = 1,
                          uint32 groupCountZ = 1) noexcept = 0;

    // Dispatch와 같은 조건에서 buffer[offset]의 DispatchIndirectArguments로 실행한다.
    virtual void DispatchIndirect(BufferHandle arguments, uint64 offset) noexcept = 0;

    // compute 쓰기 버퍼 view slot(0 – MaxDrawBufferViews-1, HLSL u0–u3)에 ReadWrite view를 묶는다.
    // null handle·Read view·stale handle은 slot을 비운다. 그래픽 단계에는 UAV가 없으므로 그리기는
    // 이 slot을 쓰지 않는다.
    virtual void SetWritableBufferView(uint32 slot, BufferViewHandle view) noexcept = 0;

    // viewport·scissor를 바꾼다. 패스 시작 때 둘 다 대상 전체로 돌아간다. 크기 0 viewport는
    // 무시한다.
    virtual void SetViewport(const Viewport& viewport) noexcept = 0;
    virtual void SetScissor(const ScissorRect& scissor) noexcept = 0;

    // 스텐실 기준 값이다(패스 시작 때 0).
    virtual void SetStencilReference(uint32 reference) noexcept = 0;

    // 버퍼 구간 복사다(GPU, 기록 순서). 실패: stale StaleHandle, 0 크기·범위 밖·같은 버퍼끼리(한
    // 자원이 복사 원본·대상 상태를 함께 가질 수 없다)·offset·크기가 4 byte 배수 아님
    // InvalidArgument.
    virtual Result<void> CopyBuffer(BufferHandle destination, uint64 destinationOffset,
                                    BufferHandle source, uint64 sourceOffset,
                                    uint64 size) noexcept = 0;

    // 텍스처 mip 0 전체 복사다(history·distortion용). 두 텍스처는 크기·형식이 같고 view가 아니어야
    // 하며 지금 패스의 attachment가 아니어야 한다. 실패: stale StaleHandle, 조건 위반
    // InvalidArgument.
    virtual Result<void> CopyTexture(TextureHandle destination, TextureHandle source) noexcept = 0;

    // 쓰기 view 범위의 모든 32-bit word를 value로 채운다(UAV clear, D3D
    // ClearUnorderedAccessViewUint· Vulkan vkCmdFillBuffer와 같은 bit 그대로). raw view와
    // R32Uint·R32Sint typed view만 받는다(다른 typed 형식은 형식 변환이 backend마다 다르고, 구조화
    // view는 D3D12가 clear를 받지 않는다). 실패: stale StaleHandle, 기록 중 아님·Read view
    // InvalidArgument, 구조화 view·그 밖의 typed 형식 Unsupported.
    virtual Result<void> ClearWritableBufferView(BufferViewHandle view, uint32 value) noexcept = 0;

    // 지금까지 그린 패스 대상(스왑체인 백버퍼 또는 BeginPass의 색 attachment 0)을 destination
    // RenderTarget에 그대로 복사한다. 복사 결과는 이미지 위쪽이 첫 행인 배치라 ReadPixels로 읽으면
    // offscreen 패스와 같은 배치다. 창 출력 확인(시험)·화면 캡처용이며, 복사 뒤에도 같은 패스에
    // 계속 그릴 수 있다.
    //
    // 사용 예(창에 그린 프레임 읽기):
    //   auto frame = device.BeginFrame(swapChain, clear);
    //   ... 그리기 ...
    //   frame.Value()->CopyPassTarget(capture);   // capture: 창과 같은 크기의 RenderTarget
    //   device.EndFrame();
    //   device.ReadPixels(capture, pixels);
    //
    // 실패: stale handle StaleHandle, RenderTarget이 아니거나 크기·형식이 패스 대상과 다르거나
    // 패스 대상 자신이면 InvalidArgument, 기록 중이 아니면 InvalidArgument.
    virtual Result<void> CopyPassTarget(TextureHandle destination) noexcept = 0;

    // --- 명시 접근 상태와 전이(RenderBase 4.4절 GX5) ---

    // 자원 접근 상태를 바꾼다. 이 호출 앞에 기록한 명령의 접근이 끝난 뒤, 뒤에 기록할 명령 전에
    // 바뀐다(begin·end를 한 지점에서 한다. 분리 전이는 RG2). 렌더 그래프가 pass prologue에서
    // 부른다. 각 전이의 before·after 규칙은 ResourceTransition 주석이다.
    //
    // 사용 예(렌더 타깃으로 그린 텍스처를 다음 pass에서 읽기):
    //   const ResourceTransition toRead{.texture = sceneColor,
    //                                   .before = ResourceAccess::RenderTarget,
    //                                   .after = ResourceAccess::SrvGraphics};
    //   commands.Transition(Span<const ResourceTransition>(&toRead, 1));
    //
    // 장치는 텍스처 subresource마다 현재 상태를 추적한다. BeginFrame·BeginPass는 색 attachment를
    // RenderTarget, 깊이를 DepthWrite(읽기 전용이면 DepthRead)로 바꾸고(attachment 전이),
    // EndFrame은 offscreen 대상을 RenderTarget 그대로 두며 스왑체인 백버퍼만 Present로 돌린다. 명시
    // 전이 없이 다른 상태의 텍스처를 읽는 호출(SetTexture 등)은 장치가 필요하면 암묵 전이로 메운다.
    // 렌더 그래프는 모든 전이를 명시한다.
    //
    // backend 낮추기:
    // - D3D12: 추적한 상태 → after의 resource barrier(mip 단위). UAV → UAV는 UAV barrier. 버퍼는
    //   COMMON 암묵 승격·제출 끝 복귀에 맡기므로 UAV barrier 외에는 아무것도 하지 않는다(버퍼 쓰기
    //   명령이 생기는 GX4에서 다시 본다).
    // - Vulkan: pipeline barrier(image layout과 access·stage mask).
    // - OpenGL ES·D3D11: 드라이버가 암묵 전이하므로 아무것도 하지 않는다. UAV(image·storage) 쓰기
    // 뒤
    //   읽기의 glMemoryBarrier는 그런 자원이 생기는 GX2에서 넣는다.
    //
    // 실패: 기록 중이 아니면 무시한다. stale handle·after가 Unknown이거나 성립하지 않는 조합인
    // 전이는 그 전이만 무시하고 IgnoredDraws를 올린다.
    virtual void Transition(Span<const ResourceTransition> transitions) noexcept = 0;

    // --- GPU 계측(RenderBase 13.2절, GX9) ---

    // 이 지점(앞 명령이 끝난 GPU 시각)의 timestamp를 기록하고 결과를 맞춰 볼 번호를 돌려준다. 값은
    // 이 프레임의 GPU 완료 뒤 Device::CollectTimestamps로 나온다. 기다리지 않는다.
    //
    // 사용 예:
    //   auto begin = commands.WriteTimestamp();
    //   ... 그리기 ...
    //   auto end = commands.WriteTimestamp();
    //   // 완료 뒤: CollectTimestamps에서 query == begin.Value()·end.Value()인 결과의 tick 차이
    //
    // 실패: 장치가 timestamp를 지원하지 않으면 Unsupported(DeviceInfo::timestamps), 앞
    // TimestampFrameSlots개 프레임의 결과가 아직 회수 전이면 Busy, 프레임당 MaxTimestampsPerFrame을
    // 넘으면 CapacityExceeded.
    virtual Result<uint64> WriteTimestamp() noexcept = 0;

    // 이름 구간을 연다(캡처 도구의 debug marker). name(UTF-8)은 호출 동안만 쓰며
    // MaxDebugMarkerNameBytes를 넘으면 글자 경계에서 자른다.
    // DeviceInfo::debugMarkers가 false면 아무것도 하지 않는다. 프레임 끝에 열린 구간은 장치가
    // 닫는다.
    virtual void PushMarker(StringView name) noexcept = 0;

    // 가장 최근 구간을 닫는다. 열린 구간이 없으면 무시하고 IgnoredDraws를 올린다.
    virtual void PopMarker() noexcept = 0;

protected:
    ~CommandContext() = default;
};

// 그래픽 장치 계약이다. backend 모듈의 생성 함수(CreateD3D12Device·CreateOpenGLDevice·
// CreateVulkanDevice 등)로 만든다.
//
// 사용 예:
//   auto created = graphics::CreateD3D12Device(allocator, {.enableValidation = true});
//   graphics::Device& device = *created.Value();
//   auto swapChain = device.CreateSwapChain({.surface = surface, .width = w, .height = h});
//   auto vertices = device.CreateBuffer({.size = sizeof(data)}, bytes);
//   auto pipeline = device.CreatePipeline(pipelineDesc);
//   auto frame = device.BeginFrame(swapChain.Value(), {0.1f, 0.1f, 0.1f, 1.0f});
//   if (frame) {
//       const VertexBufferBinding binding{vertices.Value(), 0};
//       frame.Value()->SetPipeline(pipeline.Value());
//       frame.Value()->SetVertexBuffers(0, Span<const VertexBufferBinding>(&binding, 1));
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
//   스레드의 BeginFrame·BeginPass가 돌려주는 하나뿐이다(병렬 기록은 GX4·RG3).
// - 자원·파이프라인 생성의 스레드 안전 여부는 DeviceInfo::concurrentResourceCreation이 공개한다.
//   false면 생성도 소유 스레드 전용이다.
// - "스레드 안전" 표시가 붙은 함수(Info·fence API·소유권 조회)는 어느 스레드에서나 부를 수 있다.
// - 소유권 이관(GX10): 소유 스레드가 ReleaseOwnership으로 놓고 새 스레드가 AcquireOwnership으로
//   잡는다. 그 사이와 다른 스레드에서의 소유 스레드 전용 호출은 Debug·Development 구성에서
//   WrongThread로 거부한다(반환값이 없는 호출은 무시하고 RejectedCalls를 올린다). Release 구성은
//   검사를 생략한다. 기록 객체의 그리기 호출은 검사하지 않는다(소유 스레드의 Begin만 돌려준다).
//
// 계약:
// - 프레임은 한 번에 하나만 기록한다. BeginFrame·BeginPass 뒤 EndFrame 전의 다른 Begin은
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
// - 기록 중에도 자원을 만들 수 있다. 초기 데이터 업로드는 아래 업로드 경로로 다음 제출 앞에서
//   실행된다(기다리지 않는다).
// - 장치 소멸 전에 WaitIdle을 기다리고 모든 자원을 해제한다(소멸자가 대신한다).
class Device {
public:
    virtual ~Device() = default;

    // 스레드 안전.
    virtual DeviceInfo Info() const noexcept = 0;

    // 장치 능력(전역 한도·기능 표시, RenderBase 4.4절 GX1)이다. 장치 생성 때 정해지고 바뀌지
    // 않으며 참조는 장치 수명 동안 유효하다. 스레드 안전.
    //
    // 사용 예:
    //   if (device.Caps().limits.colorAttachments < 8) { ... Deferred D·E를 끈다 ... }
    virtual const DeviceCaps& Caps() const noexcept = 0;

    // 창 표면에 스왑체인을 만든다. 크기 0이나 빈 표면은 InvalidArgument, OS·API 실패는
    // DependencyFailed다.
    virtual Result<SwapChainHandle> CreateSwapChain(const SwapChainDesc& desc) noexcept = 0;

    // 창 크기에 맞춰 버퍼를 다시 만든다. 0 크기는 InvalidArgument(최소화 중에는 호출하지 않는다).
    virtual Result<void> ResizeSwapChain(SwapChainHandle swapChain, uint32 width,
                                         uint32 height) noexcept = 0;

    // 버퍼를 만들고 initial을 복사한다(BufferDesc·BufferUsage). 실패: 설정 규칙 위반
    // (ValidateBufferDesc)·initial 크기가 desc.size와 다름 InvalidArgument, 크기가
    // DeviceLimits::bufferBytes를 넘으면 Unsupported, API 실패 DependencyFailed.
    virtual Result<BufferHandle> CreateBuffer(const BufferDesc& desc,
                                              Span<const uint8> initial) noexcept = 0;

    // buffer의 범위를 셰이더가 읽거나(Read, SRV) 쓰는(ReadWrite, UAV) view를 만든다(GX2). view는
    // 버퍼 자원을 붙들므로 버퍼를 먼저 Retire해도 view가 산다. Read view는
    // CommandContext::SetBufferView로 묶고, ReadWrite view는 compute 단계(GX3·GX4)에서 묶는다.
    // Retire로 놓는다. 다른 스레드 생성 규칙은 DeviceInfo::concurrentResourceCreation이다.
    //
    // 사용 예:
    //   BufferViewDesc desc{.kind = BufferViewKind::Structured};
    //   auto view = device.CreateBufferView(particles, desc);
    //
    // 실패: stale handle StaleHandle, 규칙 위반(ResolveBufferView)·장치 정렬 위반 InvalidArgument,
    // 형식 능력·범위 한도 밖(CheckBufferViewLimits) Unsupported. 다른 형식·종류로 대체하지 않는다.
    //
    // Vulkan은 typed view 형식이 view 접근만이 아니라 버퍼의 모든 texel 용도를 받아야 한다
    // (VUID-VkBufferViewCreateInfo-format-08778): ShaderRead | ShaderWrite 버퍼의 Read view도
    // 형식에 TexelBufferWrite가 있어야 하고 없으면 Unsupported다. 읽기만 하는 형식은 ShaderRead만
    // 준 버퍼에 만든다(VK_KHR_maintenance5의 view별 usage로 풀 수 있으나 기준 1.1에는 없어 쓰지
    // 않는다).
    virtual Result<BufferViewHandle> CreateBufferView(BufferHandle buffer,
                                                      const BufferViewDesc& desc) noexcept = 0;

    // 버퍼 [offset, offset + out.Size())를 out으로 읽는다. 앞서 쌓인 업로드 뒤에 복사하고 GPU
    // 완료를 기다린다(시험·캡처용, 프레임 경로에서 쓰지 않는다). 실패: stale StaleHandle, 기록 중
    // Busy, 빈 out·범위 밖 InvalidArgument.
    virtual Result<void> ReadBuffer(BufferHandle buffer, uint64 offset,
                                    Span<uint8> out) noexcept = 0;

    // 텍스처를 만든다. Sampled는 pixels(mip 0부터, mip 안은 plane 순서, 행 위→아래 빈틈 없는 행 —
    // MipChainBytes 크기)가 필요하고, RenderTarget·DepthStencil은 pixels가 비어 있어야 한다. 잘못된
    // 크기·형식은 InvalidArgument, 이 장치가 형식·용도를 지원하지 않으면(GetPixelFormatCaps)
    // Unsupported다.
    virtual Result<TextureHandle> CreateTexture(const TextureDesc& desc,
                                                Span<const uint8> pixels) noexcept = 0;

    // 이 장치에서 format이 가진 능력이다(PixelFormat.ko.md 7·8절, 장치 생성 때 정해지고 바뀌지
    // 않는다). backend가 대응하지 않는 형식은 None이다. 스레드 안전.
    //
    // 사용 예:
    //   if (HasPixelFormatCaps(device.GetPixelFormatCaps(PixelFormat::BC7Unorm),
    //                          PixelFormatCaps::Sampled)) { ... }
    virtual PixelFormatCaps GetPixelFormatCaps(PixelFormat format) const noexcept {
        (void)format;
        return PixelFormatCaps::None;
    }

    // format으로 만들 수 있는 MSAA 표본 수 집합이다(비트 n = 2^n 표본, SampleCountBit). 렌더
    // 타깃·깊이 형식이면 1표본(비트 0)을 늘 포함하고, 2표본 이상이 하나라도 있으면 그 형식의
    // GetPixelFormatCaps에 Multisample이 있다. 렌더 타깃·깊이로 만들 수 없는 형식은 0이다. 장치
    // 생성 때 정해진다(D3D12·D3D11 CheckMultisampleQualityLevels, Vulkan
    // vkGetPhysicalDeviceImageFormatProperties sampleCounts, OpenGL ES glGetInternalformativ
    // GL_SAMPLES). MSAA 텍스처 생성은 GX2 텍스처 단계다. 스레드 안전.
    //
    // 사용 예:
    //   if ((device.GetPixelFormatSampleCounts(PixelFormat::RGBA16Float) & SampleCountBit(4)) != 0)
    virtual uint32 GetPixelFormatSampleCounts(PixelFormat format) const noexcept = 0;

    // texture의 자원을 공유하고 형식·plane만 다르게 읽는 view를 만든다(TextureViewDesc). 돌려준
    // handle은 SetTexture로 묶고 Retire로 놓는다. 원 텍스처를 먼저 Retire해도 view가 자원을 붙든다.
    // view에는 업로드·readback·그리기 대상 지정을 할 수 없다(InvalidArgument). 형식·plane 조합이
    // 틀리면 InvalidArgument, 형식이 Sampled가 아니거나 backend가 view를 지원하지 않으면
    // Unsupported다.
    virtual Result<TextureHandle> CreateTextureView(TextureHandle texture,
                                                    const TextureViewDesc& desc) noexcept {
        (void)texture;
        (void)desc;
        return Result<TextureHandle>::Failure(ErrorCode::Unsupported);
    }

    // 텍스처 mip 하나의 plane 하나를 out(PixelFormatPlaneImageBytes 크기, 행 위→아래, 블록 형식은
    // 블록 행)으로 읽는다. 렌더 타깃이 아닌 텍스처·블록 압축·깊이·비디오 plane도 읽는다. GPU 완료를
    // 기다린다(시험·캡처용, 프레임 경로에서 쓰지 않는다). 크기·mip·plane이 틀리면 InvalidArgument,
    // backend가 지원하지 않으면 Unsupported다.
    virtual Result<void> ReadTexture(TextureHandle texture, uint32 mipLevel, uint32 plane,
                                     Span<uint8> out) noexcept {
        (void)texture;
        (void)mipLevel;
        (void)plane;
        (void)out;
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 셰이더를 컴파일하고 파이프라인을 만든다. 셰이더 컴파일 실패는 DependencyFailed다(진단 로그는
    // LastError로 읽는다). 정점 배치 위반(ValidateVertexLayout)과 셰이더 입력 대조 실패는
    // InvalidArgument, backend가 지원하지 않는 정점 형식(OpenGL의 Bgra8Unorm)은 Unsupported다.
    virtual Result<PipelineHandle> CreatePipeline(const PipelineDesc& desc) noexcept = 0;

    // compute 파이프라인을 만든다(ComputePipelineDesc, GX3). 돌려준 handle은 SetPipeline으로 묶고
    // Dispatch로 실행하며 Retire로 놓는다. 실패: 원문 없음·배치 밖 자원 InvalidArgument, 컴파일
    // 실패 DependencyFailed(LastError), compute가 없는 장치 Unsupported.
    virtual Result<PipelineHandle>
    CreateComputePipeline(const ComputePipelineDesc& desc) noexcept = 0;

    // 스왑체인의 다음 백버퍼에 그리기를 시작한다(clear 포함). 장치 손실 뒤에는 Destroyed다.
    virtual Result<CommandContext*> BeginFrame(SwapChainHandle swapChain,
                                               const ClearColor& clear) noexcept = 0;

    // attachment들(PassDesc)에 기록을 시작한다(GX4). 색 attachment는 RenderTarget, 깊이는
    // DepthStencil 텍스처이고 크기가 모두 같아야 한다. 시작 때 attachment를 RenderTarget·DepthWrite
    // (readOnly면 DepthRead)로 전이하고 load 동작을 한다. viewport·scissor는 대상 전체, 스텐실
    // 기준은 0이다. 실패: stale handle StaleHandle, 용도·크기 불일치·colorCount >
    // MaxColorTargets·같은 텍스처 중복 InvalidArgument, colorCount > DeviceLimits::colorAttachments
    // Unsupported, 기록 중 Busy, 장치 손실 Destroyed.
    virtual Result<CommandContext*> BeginPass(const PassDesc& pass) noexcept = 0;

    // RenderTarget 텍스처 하나에 clear로 기록을 시작한다(BeginPass의 간단한 형태).
    Result<CommandContext*> BeginOffscreen(TextureHandle target, const ClearColor& clear) noexcept {
        PassDesc pass;
        pass.colorCount = 1;
        pass.colors[0].texture = target;
        pass.colors[0].clear = clear;
        return BeginPass(pass);
    }

    // 기록을 graphics queue에 제출하고 그 완료 지점을 돌려준다. BeginFrame이었으면 present한다.
    // 진행 중인 프레임이 없으면 InvalidArgument다. 제출은 했지만 present가 실패하면 실패를
    // 돌려주며 제출의 완료 지점은 LastSubmittedFence로 읽는다. 장치 손실이 드러나면 Destroyed다.
    virtual Result<GpuFencePoint> EndFrame() noexcept = 0;

    // RenderTarget 텍스처를 out(width*height*4바이트, 행 위→아래)으로 읽는다. GPU 완료를 기다린다
    // (시험·캡처용, 프레임 경로에서 쓰지 않는다). 크기가 다르면 InvalidArgument다.
    virtual Result<void> ReadPixels(TextureHandle target, Span<uint8> out) noexcept = 0;

    // 자원을 회수 대기로 넘긴다(위 "제출·완료·회수"). 모르는·이미 회수한 handle은 무시한다.
    virtual void Retire(BufferHandle buffer) noexcept = 0;
    virtual void Retire(BufferViewHandle view) noexcept = 0;
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

    // --- 업로드·readback(GX8, RenderBase 4.4절) ---
    //
    // 업로드는 "map 쓰기 → 복사"다: 데이터를 CPU 가시 메모리(업로드 ring 또는 staging)에 쓰고 GPU
    // 복사를 업로드 기록에 쌓는다. 쌓인 기록은 소유 스레드의 다음
    // 제출(EndFrame·WaitIdle·SubmitUploads· ReadPixels·RequestReadback)이 그 제출보다 먼저
    // 실행한다. 그래서 갱신 뒤에 기록한 그리기는 새 내용을 본다(기록 중에 갱신하면 그 프레임 전체가
    // 새 내용을 본다). 생성의 초기 데이터도 같은 경로다. ring 구간·staging은 그것을 읽은 제출의
    // fence가 끝난 뒤에만 다시 쓴다. 생성·갱신·staging 업로드의 스레드 규칙은
    // DeviceInfo::concurrentResourceCreation을 따른다.
    //
    // 사용 예(렌더 lane의 갱신 작업):
    //   device.UpdateTexture(texture, 0, pixels);     // 다음 EndFrame 전에 복사된다
    //   auto staging = device.AcquireStaging(mipBytes);   // 스트리밍: IO가 staging에 바로 읽는다
    //   ... staging.Value().data에 읽기 ...
    //   device.UploadTextureFromStaging(texture, mip, staging.Value().handle, 0);
    //   device.ReleaseStaging(staging.Value().handle);   // 업로드 fence 뒤 재사용된다

    // 버퍼 [offset, offset + data.Size()) 내용을 바꾼다. 실패: 모르는 handle StaleHandle, 빈
    // data·범위 밖 InvalidArgument, ring·staging을 잡지 못하면 그 오류다.
    virtual Result<void> UpdateBuffer(BufferHandle buffer, uint64 offset,
                                      Span<const uint8> data) noexcept = 0;

    // 텍스처 mip 하나를 바꾼다(pixels는 빈틈 없는 행, 첫 행이 위). 실패: 모르는 handle StaleHandle,
    // mip 범위 밖·크기 불일치 InvalidArgument.
    virtual Result<void> UpdateTexture(TextureHandle texture, uint32 mipLevel,
                                       Span<const uint8> pixels) noexcept = 0;

    // 쌓인 업로드 기록을 지금 제출하고 그 완료 지점을 돌려준다(소유 스레드). 쌓인 것이 없으면 null
    // 지점이다. 기록 중이면 Busy다.
    virtual Result<GpuFencePoint> SubmitUploads() noexcept = 0;

    // 업로드 ring 진단이다(소유 스레드).
    virtual UploadStats UploadStatistics() const noexcept = 0;

    // 스트리밍 staging을 받는다(스레드 안전). 프레임 업로드 ring과 따로인 pool이며 IO 목적지로 바로
    // 쓸 수 있는 CPU 가시 메모리다(TextureStreaming 5.4절). 실패: 0·예산 초과 크기 InvalidArgument,
    // 예산이 찼으면 CapacityExceeded.
    virtual Result<StagingBuffer> AcquireStaging(uint64 bytes) noexcept = 0;

    // staging의 offset부터 빈틈 없는 행 픽셀로 텍스처 mip 하나를 올린다. 실패: stale handle
    // StaleHandle, 범위 밖 InvalidArgument. staging은 이 업로드의 fence 뒤까지 재사용되지 않는다.
    virtual Result<void> UploadTextureFromStaging(TextureHandle texture, uint32 mipLevel,
                                                  StagingHandle staging,
                                                  uint64 offset) noexcept = 0;

    // staging 구간을 버퍼 구간으로 올린다.
    virtual Result<void> UploadBufferFromStaging(BufferHandle buffer, uint64 destinationOffset,
                                                 StagingHandle staging, uint64 sourceOffset,
                                                 uint64 size) noexcept = 0;

    // staging을 돌려준다(스레드 안전). 그것을 읽는 업로드의 fence가 끝나면 pool이 다시 쓴다.
    virtual void ReleaseStaging(StagingHandle staging) noexcept = 0;

    // staging pool 진단이다(스레드 안전).
    virtual StagingStats StagingStatistics() const noexcept = 0;

    // RenderTarget 텍스처의 비동기 readback을 요청한다(소유 스레드). 지금까지의 제출 뒤에 복사를
    // 제출하고 완료 지점을 돌려준다. 기다리지 않는다. 실패: stale StaleHandle, RenderTarget이
    // 아니면 InvalidArgument, 기록 중이면 Busy.
    virtual Result<ReadbackRequest> RequestReadback(TextureHandle target) noexcept = 0;

    // readback 결과를 out(width*height*4 byte, 첫 행이 위)에 복사한다(소유 스레드). 완료 지점이
    // 아직 끝나지 않았으면 false이며 기다리지 않는다. 실패: stale StaleHandle, 크기 불일치
    // InvalidArgument, 장치 손실 Destroyed.
    virtual Result<bool> TryReadback(ReadbackHandle readback, Span<uint8> out) noexcept = 0;

    // readback을 놓는다(소유 스레드). 버퍼는 fence 뒤에 회수된다.
    virtual void ReleaseReadback(ReadbackHandle readback) noexcept = 0;

    // --- GPU 계측(GX9) ---

    // GPU가 끝낸 프레임의 timestamp 결과를 쓴 순서대로 out에 옮기고 옮긴 수를 돌려준다(소유
    // 스레드, 비차단). 아직 GPU가 끝내지 않은 프레임의 결과는 나오지 않는다(미완료). out이 작으면
    // 남은 결과는 다음 호출로 간다. 지원하지 않거나 소유 스레드가 아니면 0이다.
    virtual usize CollectTimestamps(Span<TimestampSample> out) noexcept = 0;

    // GPU tick과 CPU 시각(platform::MonotonicNanoseconds 시간축) 한 쌍을 얻는다(소유 스레드).
    // GpuTicksToCpuNanoseconds로 timestamp를 CPU trace와 같은 시간축에 놓는다. 실패: 지원하지
    // 않으면 Unsupported(DeviceInfo::clockCalibration), 장치 손실 Destroyed.
    virtual Result<ClockCalibration> CalibrateClocks() noexcept = 0;

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
    // ERROR·CORRUPTION, GL debug output의 ERROR·UNDEFINED_BEHAVIOR, 명시 전이의 before가 장치가
    // 추적한 상태와 다른 수). 검증이 꺼져 있으면 0이다.
    // 시험이 "검증 오류 0"을 확인할 때 쓴다.
    virtual uint64 ValidationErrorCount() const noexcept = 0;

    // 회수 대기 중인 자원 수다(진단·시험: 해제 누락 0 확인).
    virtual usize PendingRetireCount() const noexcept = 0;
};

} // namespace rocks::graphics
