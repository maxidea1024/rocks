#pragma once

#include <Graphics/ShaderBindings.h>

#include "D3D11Common.h"

#include <Core/Containers/Array.h>
#include <Core/Containers/SlotArray.h>
#include <Core/Text/String.h>
#include <D3D11Graphics/D3D11Device.h>
#include <Graphics/DeviceOwnership.h>
#include <Graphics/FenceTimeline.h>
#include <Graphics/GraphicsLog.h>
#include <Graphics/SplitTransitionTracker.h>
#include <Graphics/StagingPool.h>
#include <Graphics/TimestampRing.h>
#include <Graphics/UploadRing.h>
#include <Platform/Sync.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <dxgidebug.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

// D3D11 장치 구현의 선언이다. 정의는 기능별 파일에 나눈다.
// - D3D11Device.cpp: 생성·장치 정보·fence·소유권·회수·검증 집계
// - D3D11Resources.cpp: 스왑체인·버퍼·텍스처·파이프라인·샘플러·Retire
// - D3D11Frame.cpp: 프레임 시작·제출과 기록(CommandContext)
// - D3D11Commands.cpp: 파이프라인 상태·compute·패스 attachment·간접 명령·복사·UAV clear(GX3·GX4)
// - D3D11Upload.cpp: 업로드 기록·ring·staging·readback·WaitIdle
// - D3D11Timing.cpp: timestamp·calibration·marker
namespace rocks::graphics::d3d11 {

struct BufferRecord {
    ComRef<ID3D11Buffer> buffer;
    uint64 size = 0;
    BufferDesc desc;
};

// 버퍼 view(GX2)다. D3D11 view가 버퍼 참조를 쥐므로 버퍼를 먼저 놓아도 산다. Read view는 srv,
// ReadWrite view는 uav만 있다.
struct BufferViewRecord {
    ComRef<ID3D11ShaderResourceView> srv;
    ComRef<ID3D11UnorderedAccessView> uav;
    PixelFormat format = PixelFormat::Unknown;
    BufferViewKind kind = BufferViewKind::Typed;
    ViewAccess access = ViewAccess::Read;
    PixelShaderType shaderType = PixelShaderType::None;
    // 구조화 view의 원소 byte다(셰이더 선언 stride와 대조한다).
    uint32 stride = 0;
};

struct TextureRecord {
    // ID3D11Texture2D(2D·배열·cube) 또는 ID3D11Texture3D다.
    ComRef<ID3D11Resource> texture;
    // 형식이 Sampled일 때만 있다. 없으면 SetTexture는 흰 텍스처로 대신한다.
    ComRef<ID3D11ShaderResourceView> srv;
    // RenderTarget만 있다.
    ComRef<ID3D11RenderTargetView> rtv;
    // DepthStencil만 있다. readOnlyDsv는 읽기 전용 깊이
    // attachment(DepthAttachment::readOnly)용이다.
    ComRef<ID3D11DepthStencilView> dsv;
    ComRef<ID3D11DepthStencilView> readOnlyDsv;
    // 쓰기 view(ViewAccess::ReadWrite)의 UAV다. 있으면 SetWritableTexture로만 묶는다.
    ComRef<ID3D11UnorderedAccessView> uav;
    TextureDimension dimension = TextureDimension::Texture2D;
    uint32 width = 0;
    uint32 height = 0;
    // 3D 깊이다(다른 차원은 1).
    uint32 depth = 1;
    uint32 mipLevels = 1;
    // subresource slice 수(TextureSliceCount)이며 2D 자원의 ArraySize(3D는 1)와 같다.
    uint32 slices = 1;
    // view(CreateTextureView)면 view가 읽는 형식, 아니면 텍스처 형식이다.
    PixelFormat format = PixelFormat::Unknown;
    bool renderTarget = false;
    bool depthTarget = false;
    // 만든 용도다(view는 원 텍스처의 용도, ResolveTextureViewRange).
    TextureUsage usage = TextureUsage::Sampled;
    // MSAA 표본 수다(TextureDesc::sampleCount, view는 원 텍스처의 값).
    uint32 sampleCount = 1;
    // 다른 텍스처의 자원을 공유하는 view다(SRV와 texture 참조가 자원을 붙든다). 업로드·readback·
    // 그리기 대상이 될 수 없다.
    bool view = false;
    // 샘플러 캐시의 객체다(장치 수명 동안 산다).
    ID3D11SamplerState* sampler = nullptr;
    // 샘플러가 깊이 비교 샘플러다(view는 깊이 plane만 읽을 수 있다).
    bool comparison = false;
};

// mip level의 깊이 면 수다(3D만 1보다 크다).
inline uint32 MipDepth(const TextureRecord& record, uint32 level) noexcept {
    return record.dimension == TextureDimension::Texture3D ? MipExtent(record.depth, level) : 1u;
}

// mip level 하나의 데이터 byte다(모든 slice·깊이 면, TextureMipBytes와 같은 값).
inline uint64 MipBytes(const TextureRecord& record, uint32 level) noexcept {
    return PixelFormatImageBytes(record.format, MipExtent(record.width, level),
                                 MipExtent(record.height, level)) *
           MipDepth(record, level) * record.slices;
}

struct PipelineRecord {
    // 고정 slot 표(셰이더 layout의 단계별 slot)다.
    PipelineSlots slots;
    ComRef<ID3D11VertexShader> vertexShader;
    ComRef<ID3D11PixelShader> pixelShader;
    // 정점 입력이 없는 배치면 비어 있다.
    ComRef<ID3D11InputLayout> inputLayout;
    // 고정 기능 상태다(GX3, 파이프라인마다 만든다).
    ComRef<ID3D11BlendState> blend;
    ComRef<ID3D11RasterizerState> raster;
    ComRef<ID3D11DepthStencilState> depthStencil;
    // compute 파이프라인이면 이것만 있다.
    ComRef<ID3D11ComputeShader> computeShader;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    // stream별 stride와 배치가 선언한 stream mask다(그리기 때 묶인 정점 버퍼를 확인한다).
    uint32 strides[MaxVertexStreams] = {};
    uint32 streamMask = 0;
    // VS·PS(compute면 CS)가 읽는 버퍼 view slot과 CS가 쓰는 slot이다(리플렉션).
    DrawBufferSlots bufferSlots;
    DrawBufferSlots writeSlots;
    // 선언한 그리기별 상수 byte 수다(SetDrawConstants 크기 검사).
    uint32 drawConstantBytes = 0;
    // compute가 텍스처 UAV로 쓰는 쓰기 slot 비트다(SetWritableTexture가 묶여야 한다).
    uint32 writeTextures = 0;
    bool compute = false;
    // 깊이·스텐실을 쓴다(읽기 전용 깊이 attachment에서 그리기를 거른다).
    bool writesDepthStencil = false;
};

// flip model 스왑체인이다. D3D11 runtime이 버퍼를 돌리므로 버퍼 0과 그 RTV만 쥔다.
struct SwapChainRecord {
    ComRef<IDXGISwapChain1> swapChain;
    ComRef<ID3D11Texture2D> buffer;
    ComRef<ID3D11RenderTargetView> rtv;
    uint32 width = 0;
    uint32 height = 0;
    bool vsync = true;
    // ResizeBuffers·버퍼 재취득이 실패해 버퍼가 없는 상태다. 다음 Resize가 성공할 때까지
    // BeginFrame은 DependencyFailed다.
    bool broken = false;
};

// 상수 버퍼 handle 하나다(RenderBase 9.11절 — SingleFrame IMMUTABLE, MultiFrame DEFAULT).
struct ConstantBufferRecord {
    ComRef<ID3D11Buffer> buffer;
    ConstantBufferLifetime lifetime = ConstantBufferLifetime::SingleFrame;
    uint32 bytes = 0;
};

// 비동기 readback 하나다. fence가 끝나면 staging 텍스처에 있다.
struct ReadbackRecord {
    ComRef<ID3D11Texture2D> staging;
    uint64 fenceValue = 0;
    uint32 width = 0;
    uint32 height = 0;
    uint32 bytesPerPixel = 0;
};

// fence 완료 뒤에 놓을 COM 객체들이다.
struct Retired {
    ComRef<IUnknown> objects[MaxRetiredObjects];
    uint64 fenceValue = 0;
    // 아직 실행하지 않은 업로드 기록이 이 자원을 쓸 수 있다. 업로드 묶음 제출이 fence 값을 정한다.
    bool uploadPending = false;
    // 기록 중인(아직 제출하지 않은) 프레임이 쓸 수 있다. EndFrame이 그 프레임의 signal 값을 정할
    // 때까지 놓지 않는다.
    bool pending = false;
};

// ring에 자리가 없어 따로 잡은 업로드 원본 메모리다. 그 업로드 묶음의 fence 뒤에 놓는다.
struct DedicatedUpload {
    void* memory = nullptr;
    uint64 fenceValue = 0;
    bool uploadPending = true;
};

// 소유 스레드의 다음 제출이 immediate context에서 실행할 업로드 명령 하나다.
struct UploadOp {
    enum class Kind : uint8 {
        // UpdateSubresource(destination, subresource, box, source, rowPitch, depthPitch)
        Update,
        // CopyResource(destination, copySource) — readback
        Copy,
        // CopySubresourceRegion(destination, subresource, copySource, sourceSubresource, box) —
        // 깊이· 비디오 staging 업로드와 mip·plane readback, 버퍼 구간 readback(hasBox면 원본 구간)
        CopyRegion
    };

    Kind kind = Kind::Update;
    // 대상은 자원 표나 회수 항목(uploadPending)이 이 명령이 실행될 때까지 살려 둔다.
    ID3D11Resource* destination = nullptr;
    ID3D11Resource* copySource = nullptr;
    uint32 subresource = 0;
    uint32 sourceSubresource = 0;
    bool hasBox = false;
    D3D11_BOX box = {};
    const uint8* source = nullptr;
    uint32 rowPitch = 0;
    uint32 depthPitch = 0;
};

// 스트리밍 staging 메모리(시스템 메모리)를 만들고 없앤다(StagingPool의 backend). D3D11에는 계속
// map해 둘 수 있는 업로드 heap이 없으므로 업로드는 제출 때 이 메모리에서 UpdateSubresource로
// 올린다.
class SystemStagingSource final : public StagingSource {
public:
    Result<StagingMemory> CreateStaging(uint64 bytes) noexcept override;
    void DestroyStaging(const StagingMemory& memory) noexcept override;

    Allocator* allocator = nullptr;
};

// timestamp 결과를 query에서 읽는다(GX9). Harvest가 fence 완료 뒤에만 부른다. 프레임 slot의
// TIMESTAMP_DISJOINT 결과가 끊김을 표시하거나 주파수가 공개값과 다르면 그 slot은 Disjoint다.
// immediate context를 쓰므로 소유 스레드(장치 잠금 안)에서만 부른다.
class QueryTimestampSource final : public TimestampSource {
public:
    TimestampReadState ReadTimestamps(uint32 first, uint32 count, uint64* ticks) noexcept override;

    ID3D11DeviceContext* context = nullptr;
    ComRef<ID3D11Query>* disjoint = nullptr;
    ComRef<ID3D11Query>* stamps = nullptr;
    uint64 frequency = 0;
};

// fence 관찰 스레드가 제출 하나의 완료를 기다리는 경계다. 장치가 제거되면 fence 완료 값이
// UINT64_MAX가 되어 대기가 풀린다.
class FenceEventSource final : public FenceWaitSource {
public:
    bool WaitSubmission(uint64 value, void* token) noexcept override;

    ID3D11Fence* fence = nullptr;
    HANDLE event = nullptr;
};

class D3D11Device final : public Device, private CommandContext {
public:
    explicit D3D11Device(Allocator& allocator) noexcept;
    ~D3D11Device() override;

    // 장치를 만든다. 실패하면 CreateFailure가 원인을 담는다.
    Result<void> Initialize(const DeviceDesc& desc) noexcept;

    // 생성 실패의 원인을 report에 옮긴다.
    void FillReport(ErrorCode code, DeviceCreateReport& report) const noexcept;

    // --- Device ---

    DeviceInfo Info() const noexcept override;
    const DeviceCaps& Caps() const noexcept override;
    uint32 GetPixelFormatSampleCounts(PixelFormat format) const noexcept override;
    Result<BufferViewHandle> CreateBufferView(BufferHandle buffer,
                                              const BufferViewDesc& desc) noexcept override;
    Result<void> ReadBuffer(BufferHandle buffer, uint64 offset, Span<uint8> out) noexcept override;
    Result<SwapChainHandle> CreateSwapChain(const SwapChainDesc& desc) noexcept override;
    Result<void> ResizeSwapChain(SwapChainHandle swapChain, uint32 width,
                                 uint32 height) noexcept override;
    Result<BufferHandle> CreateBuffer(const BufferDesc& desc,
                                      Span<const uint8> initial) noexcept override;
    Result<TextureHandle> CreateTexture(const TextureDesc& desc,
                                        Span<const uint8> pixels) noexcept override;
    Result<PipelineHandle> CreatePipeline(const PipelineDesc& desc) noexcept override;
    Result<PipelineHandle> CreateComputePipeline(const ComputePipelineDesc& desc) noexcept override;
    // 장치 생성 때 CheckFormatSupport로 채운 능력이다(PixelFormat.ko.md PF4).
    PixelFormatCaps GetPixelFormatCaps(PixelFormat format) const noexcept override;
    Result<TextureHandle> CreateTextureView(TextureHandle texture,
                                            const TextureViewDesc& desc) noexcept override;
    Result<TextureSubresources>
    GetTextureSubresources(TextureHandle texture) const noexcept override;
    Result<void> ReadTexture(TextureHandle texture, uint32 mipLevel, uint32 plane,
                             Span<uint8> out) noexcept override;
    Result<CommandContext*> BeginFrame(SwapChainHandle swapChain,
                                       const ClearColor& clear) noexcept override;
    Result<CommandContext*> BeginPass(const PassDesc& pass) noexcept override;
    Result<GpuFencePoint> EndFrame() noexcept override;
    Result<void> ReadPixels(TextureHandle target, Span<uint8> out) noexcept override;
    void Retire(BufferHandle buffer) noexcept override;
    void Retire(BufferViewHandle view) noexcept override;
    // 상수 버퍼(D3D11Constants.cpp, RenderBase 9.11절)다. 다른 스레드에서도 부를 수 있다
    // (concurrentResourceCreation — 기록 중 갱신은 소유 스레드만 기록 순서를 지킨다).
    Result<ConstantBufferHandle> CreateConstantBuffer(ConstantBufferLifetime lifetime,
                                                      Span<const uint8> contents) noexcept override;
    Result<void> UpdateConstantBuffer(ConstantBufferHandle buffer,
                                      Span<const uint8> contents) noexcept override;
    void Retire(ConstantBufferHandle buffer) noexcept override;
    void Retire(TextureHandle texture) noexcept override;
    void Retire(PipelineHandle pipeline) noexcept override;
    Result<ShaderHandle> CreateShader(const ShaderCodeDesc& desc) noexcept override;
    void Retire(ShaderHandle shader) noexcept override;

    void Retire(SwapChainHandle swapChain) noexcept override;
    void ReclaimCompleted() noexcept override;
    Result<void> WaitIdle() noexcept override;
    Result<void> UpdateBuffer(BufferHandle buffer, uint64 offset,
                              Span<const uint8> data) noexcept override;
    Result<void> UpdateTexture(TextureHandle texture, uint32 mipLevel,
                               Span<const uint8> pixels) noexcept override;
    Result<GpuFencePoint> SubmitUploads() noexcept override;
    UploadStats UploadStatistics() const noexcept override;
    Result<StagingBuffer> AcquireStaging(uint64 bytes) noexcept override;
    Result<void> UploadTextureFromStaging(TextureHandle texture, uint32 mipLevel,
                                          StagingHandle staging, uint64 offset) noexcept override;
    Result<void> UploadBufferFromStaging(BufferHandle buffer, uint64 destinationOffset,
                                         StagingHandle staging, uint64 sourceOffset,
                                         uint64 size) noexcept override;
    void ReleaseStaging(StagingHandle staging) noexcept override;
    StagingStats StagingStatistics() const noexcept override;
    Result<ReadbackRequest> RequestReadback(TextureHandle target) noexcept override;
    Result<bool> TryReadback(ReadbackHandle readback, Span<uint8> out) noexcept override;
    void ReleaseReadback(ReadbackHandle readback) noexcept override;
    usize CollectTimestamps(Span<TimestampSample> out) noexcept override;
    Result<ClockCalibration>
    CalibrateClocks(QueueKind queue = QueueKind::Graphics) noexcept override;
    GpuFencePoint LastSubmittedFence(QueueKind queue = QueueKind::Graphics) const noexcept override;
    Result<void> QueueWait(QueueKind queue, GpuFencePoint point) noexcept override;
    Result<bool> IsFenceComplete(GpuFencePoint point) const noexcept override;
    Result<platform::WaitResult> WaitFence(GpuFencePoint point, uint32 timeoutMs) noexcept override;
    Result<void> NotifyFence(GpuFencePoint point, FenceCallback callback,
                             void* context) noexcept override;
    bool IsLost() const noexcept override;
    bool IsOwnerThread() const noexcept override;
    Result<void> ReleaseOwnership() noexcept override;
    Result<void> AcquireOwnership() noexcept override;
    uint64 RejectedCalls() const noexcept override;
    StringView LastError() const noexcept override;
    uint64 IgnoredDraws() const noexcept override;
    uint64 NullTextureBindings() const noexcept override;
    uint64 ValidationErrorCount() const noexcept override;
    usize PendingRetireCount() const noexcept override;

private:
    // 기록 대상(백버퍼 또는 BeginPass attachment)이다. 색 0(rtv·resource·format·texture)은
    // CopyPassTarget이 쓴다.
    struct Target {
        ID3D11RenderTargetView* rtv = nullptr;
        // 패스 대상 자원이다(CopyPassTarget의 원본).
        ID3D11Resource* resource = nullptr;
        uint32 width = 0;
        uint32 height = 0;
        // 패스 대상의 형식이다(스왑체인은 RGBA8UnormSrgb view).
        PixelFormat format = PixelFormat::Unknown;
        // offscreen 패스의 대상 텍스처다(스왑체인 패스는 null).
        TextureHandle texture;
        // 모든 attachment다(스왑체인 패스는 색 1개).
        uint32 colorCount = 0;
        ID3D11RenderTargetView* rtvs[MaxColorTargets] = {};
        TextureHandle colors[MaxColorTargets];
        ID3D11DepthStencilView* dsv = nullptr;
        TextureHandle depth;
        bool depthReadOnly = false;
        // 색 attachment마다 패스 끝에 resolve할 대상이다(ColorAttachment::resolveTarget).
        TextureHandle resolves[MaxColorTargets];
        // attachment 표본 수다(1이면 MSAA가 아니다).
        uint32 sampleCount = 1;
        // resolve 대상의 mip·slice다(텍스처 GX2 C부).
        uint32 resolveMips[MaxColorTargets] = {};
        uint32 resolveSlices[MaxColorTargets] = {};
        // 색 attachment의 층이다(MSAA 배열 resolve 원본 subresource).
        uint32 colorSlices[MaxColorTargets] = {};
    };

    // 기록 중 묶인 정점·index 버퍼다(파이프라인을 바꾸면 그 stride로 다시 묶는다).
    struct BoundVertexBuffer {
        BufferHandle buffer;
        uint64 offset = 0;
    };

    struct BoundIndexBuffer {
        BufferHandle buffer;
        IndexFormat format = IndexFormat::UInt16;
        uint64 offset = 0;
    };

    struct SamplerEntry {
        SamplerDesc desc;
        ComRef<ID3D11SamplerState> state;
    };

    // 업로드 원본 공간이다(ring 구간이거나 전용 메모리).
    struct UploadSpace {
        uint8* data = nullptr;
    };

    // --- CommandContext(D3D11Frame.cpp·D3D11Timing.cpp) ---

    void SetPipeline(PipelineHandle pipeline) noexcept override;
    void SetVertexBuffers(uint32 firstStream,
                          Span<const VertexBufferBinding> buffers) noexcept override;
    void SetIndexBuffer(BufferHandle buffer, IndexFormat format, uint64 offset) noexcept override;
    void SetTexture(TextureHandle texture) noexcept override;
    void SetBufferView(uint32 slot, BufferViewHandle view) noexcept override;
    Result<void> SetDrawConstants(Span<const uint8> bytes) noexcept override;
    void SetConstantBuffer(ShaderStage stage, uint32 slot,
                           ConstantBufferHandle buffer) noexcept override;
    Result<void> SetConstantData(ShaderStage stage, uint32 slot,
                                 Span<const uint8> bytes) noexcept override;
    void Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept override;
    void DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex) noexcept override;
    Result<void> CopyPassTarget(TextureHandle destination) noexcept override;
    void Transition(Span<const ResourceTransition> transitions) noexcept override;
    // 분리 전이다. 드라이버가 암묵 전이하므로 begin은 짝만 열고 end는 Transition과 같다.
    SplitTransition BeginTransitions(Span<const ResourceTransition> transitions) noexcept override;
    void EndTransitions(SplitTransition split,
                        Span<const ResourceTransition> transitions) noexcept override;
    Result<uint64> WriteTimestamp() noexcept override;
    void PushMarker(StringView name) noexcept override;
    void PopMarker() noexcept override;

    // --- GX3·GX4(D3D11Commands.cpp) ---

    void SetWritableBufferView(uint32 slot, BufferViewHandle view) noexcept override;
    void SetWritableTexture(uint32 slot, TextureHandle view) noexcept override;
    void DrawIndirect(BufferHandle arguments, uint64 offset) noexcept override;
    void DrawIndexedIndirect(BufferHandle arguments, uint64 offset) noexcept override;
    void Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) noexcept override;
    void DispatchIndirect(BufferHandle arguments, uint64 offset) noexcept override;
    void SetViewport(const Viewport& viewport) noexcept override;
    void SetScissor(const ScissorRect& scissor) noexcept override;
    void SetStencilReference(uint32 reference) noexcept override;
    Result<void> CopyBuffer(BufferHandle destination, uint64 destinationOffset, BufferHandle source,
                            uint64 sourceOffset, uint64 size) noexcept override;
    Result<void> CopyTexture(TextureHandle destination, TextureHandle source) noexcept override;
    Result<void> ClearWritableBufferView(BufferViewHandle view, uint32 value) noexcept override;
    // 파이프라인 고정 기능 상태 객체(blend·raster·depth-stencil)를 만든다(잠금 안).
    Result<void> CreatePipelineStates(const PipelineDesc& desc, PipelineRecord& record) noexcept;
    // 그리기 직전 조건을 보고 VS·PS 버퍼 view와(dispatch 뒤면) 정점·index 버퍼를 다시 묶는다.
    // 그릴 수 없으면 거짓이다.
    bool PrepareDraw(bool indexed) noexcept;
    // dispatch 직전 조건을 보고 CS 상수·읽기 view·쓰기 view를 묶는다.
    bool PrepareDispatch() noexcept;
    // 파이프라인이 읽는 상수 버퍼(D3D11Constants.cpp)를 묶는다. 하나라도 없거나 작으면 거짓이다.
    bool ApplyConstants(const PipelineRecord& pipeline) noexcept;
    // 묶인 (단계, slot)의 버퍼와 byte 수다(없으면 null·0).
    ID3D11Buffer* BoundConstantBuffer(uint32 stage, uint32 slot, uint32& bytes) const noexcept;
    // dispatch 뒤 CS view를 풀어 다음 그리기가 같은 버퍼를 읽을 수 있게 한다.
    void FinishDispatch() noexcept;
    // 간접 인자 버퍼(Indirect 용도, offset 4 byte 배수, 범위 안)다. 아니면 null이다.
    ID3D11Buffer* IndirectArguments(BufferHandle arguments, uint64 offset, uint64 bytes) noexcept;

    // --- 생성(D3D11Device.cpp) ---

    // 어댑터 하나로 장치·immediate context·fence를 만든다. 실패하면 단계와 HRESULT를 남긴다.
    Result<void> CreateOnAdapter(IDXGIAdapter1* adapter, bool requestDebug) noexcept;
    Result<void> CreateSharedStates() noexcept;
    void ConfigureValidationQueues() noexcept;
    void DrainValidationMessages() const noexcept;
    void* Scratch(SIZE_T length) const noexcept;

    // --- 진단 ---

    // 진단 문자열에 단계와 HRESULT를 남기고 실패를 돌려준다. 장치가 손실되었으면 Destroyed다.
    Result<void> Fail(const char* stage, HRESULT result) noexcept;

    // HRESULT가 없는 실패다.
    Result<void> Fail(const char* message) noexcept;

    template <class T>
    Result<T> FailWith(const char* stage, HRESULT result) noexcept {
        auto failed = Fail(stage, result);
        return Result<T>::Failure(failed.Error());
    }

    template <class T>
    Result<T> FailWith(const char* message) noexcept {
        auto failed = Fail(message);
        return Result<T>::Failure(failed.Error());
    }

    // 정점 셰이더 입력 대조 실패를 남기고 InvalidArgument를 돌려준다.
    Result<void> Reject(const Diagnostic& message) noexcept;

    // SetDrawConstants 거부(묶인 파이프라인 없음, 크기가 선언과 다름)의 InvalidArgument다.
    Result<void> RejectDrawConstantSize(bool pipelineBound, usize bytes,
                                        uint32 drawConstantBytes) noexcept;

    // 이 장치가 형식·용도를 지원하지 않는다는 진단을 남기고 Unsupported를 돌려준다(잠금 안).
    template <class T>
    Result<T> UnsupportedFormat(PixelFormat format) noexcept {
        Diagnostic message;
        message.Text("pixel format ")
            .Text(PixelFormatName(format))
            .Text(" is not supported for this use by the D3D11 device");
        auto assigned = lastError_.TryAssign(StringView(message.CStr(), message.Length()));
        (void)assigned;
        return Result<T>::Failure(ErrorCode::Unsupported);
    }

    PixelFormatCaps FormatCaps(PixelFormat format) const noexcept;

    // API 실패가 장치 제거 때문이면 손실을 게시한다(기다리는 쪽을 Destroyed로 깨운다).
    void DetectLost(HRESULT failure) noexcept;

    // --- fence(D3D11Device.cpp) ---

    // immediate context에 다음 fence 값을 신호하고 Flush한 뒤 타임라인에 알린다.
    Result<uint64> SignalQueue() noexcept;
    // 소유 스레드가 value 완료를 직접 기다린다.
    Result<void> WaitFenceOwner(uint64 value) noexcept;
    // GPU 완료 값이다. 장치가 제거되었으면 손실을 게시하고 RemovedFenceValue를 돌려준다.
    uint64 CompletedFence() noexcept;

    // --- 회수(D3D11Device.cpp) ---

    // 객체들을 회수 대기로 넘긴다(잠금 안).
    void RetireObjects(ComRef<IUnknown> (&objects)[MaxRetiredObjects]) noexcept;

    template <class... T>
    void Defer(ComRef<T>&&... objects) noexcept {
        static_assert(sizeof...(T) <= MaxRetiredObjects, "too many objects for one retire entry");
        // 모든 D3D 인터페이스는 IUnknown을 단일 상속하므로 포인터 변환이 안전하다.
        ComRef<IUnknown> list[MaxRetiredObjects];
        usize index = 0;
        ((list[index++].Attach(objects.Detach())), ...);
        RetireObjects(list);
    }

    // 소멸 때까지 놓지 않는다(완료를 알 수 없는 제출이 쓰던 객체).
    void KeepUntilDestruction(ComRef<IUnknown>&& object) noexcept;
    // 살아 있는 자원이 모두 retire되어도 목록이 자라지 않도록 용량을 확보한다.
    Result<void> ReserveRetireCapacity(usize extra) noexcept;
    void AssignPendingRetires(uint64 value) noexcept;
    void AssignPendingUploads(uint64 value) noexcept;
    void ProcessRetired() noexcept;

    // --- 자원(D3D11Resources.cpp) ---

    Result<ID3D11SamplerState*> SamplerFor(const SamplerDesc& desc) noexcept;
    // 파이프라인 slot 표의 자원을 지금 기록의 단계 slot에 건다(그리기·dispatch 직전).
    void ApplySlots(const PipelineRecord& pipeline) noexcept;
    // --- 버퍼 view(D3D11Buffers.cpp) ---
    Result<TextureHandle> CreateWritableTextureView(const TextureRecord& source, PixelFormat format,
                                                    const ResolvedSubresourceRange& range,
                                                    TextureDimension dimension) noexcept;
    // 지금 파이프라인이 읽는 버퍼 view slot이 모두 맞는 view로 채워졌는지다(잠금 안).
    bool BufferViewsBound() const noexcept;
    Result<void> CheckVertexFormats(const VertexLayout& layout) noexcept;
    Result<void> CreateSwapChainView(SwapChainRecord& record) noexcept;
    // 2D SRV를 만든다. 비디오 형식은 PlaneSlice로, 깊이·스텐실은 view 형식으로 plane을 고른다.
    HRESULT CreatePlaneView(const TextureRecord& record, DXGI_FORMAT format, uint32 plane,
                            const ResolvedSubresourceRange& range, bool planar,
                            ComRef<ID3D11ShaderResourceView>& out) noexcept;

    // --- 기록(D3D11Frame.cpp) ---

    void BeginRecording(const Target& target, const PassDesc& pass) noexcept;
    // CPU 사본 constantValues_ 전체를 상수 버퍼에 WRITE_DISCARD로 쓴다.
    void WriteConstants() noexcept;
    // attachment (mip, slice)의 RTV·DSV를 만든다(배열·cube는 2D 배열 view의 층 하나, 3D RTV는 깊이
    // 면 하나, 텍스처 GX2 C부).
    HRESULT CreateAttachmentView(const TextureRecord& record, uint32 mipLevel, uint32 slice,
                                 ComRef<ID3D11RenderTargetView>& out) noexcept;
    HRESULT CreateAttachmentView(const TextureRecord& record, uint32 mipLevel, uint32 slice,
                                 bool readOnly, ComRef<ID3D11DepthStencilView>& out) noexcept;
    void ApplyVertexBuffer(uint32 stream) noexcept;
    bool StreamsBound() const noexcept;

    // --- 업로드(D3D11Upload.cpp) ---

    Result<UploadSpace> StageUpload(uint64 size) noexcept;
    Result<void> PushUpload(const UploadOp& op) noexcept;
    Result<void> RecordBufferUpload(ID3D11Buffer* target, uint64 offset,
                                    Span<const uint8> data) noexcept;
    // 상수 버퍼의 부분 갱신을 거부한다(D3D11은 상수 버퍼 대상 구간을 받지 않는다).
    Result<void> CheckConstantUpdate(const BufferRecord& record, uint64 offset,
                                     uint64 size) noexcept;
    // mip 하나(모든 slice·깊이 면, TextureDesc 초기 픽셀 배치)를 subresource마다
    // UpdateSubresource로 올리는 기록을 쌓는다(잠금 안).
    Result<void> RecordTextureUpload(const TextureRecord& record, uint32 mipLevel,
                                     const uint8* pixels) noexcept;
    // 깊이·스텐실·비디오 형식의 mip 하나(모든 slice)를 slice마다 D3D11 배치로 바꾼 staging
    // 텍스처에서 복사한다(잠금 안). UpdateSubresource는 깊이·스텐실 자원에 쓸 수 없다.
    Result<void> RecordStagedUpload(const TextureRecord& record, uint32 mipLevel,
                                    const uint8* pixels) noexcept;
    // RecordStagedUpload의 slice 하나다.
    Result<void> RecordStagedSlice(const TextureRecord& record, uint32 mipLevel, uint32 slice,
                                   const uint8* pixels) noexcept;
    // subresource(mip, slice) 하나의 plane 하나를 staging 복사로 읽는다(잠금 안, 소유 스레드,
    // 3D는 그 mip의 모든 깊이 면).
    Result<void> ReadSubresource(const TextureRecord& record, uint32 mipLevel, uint32 slice,
                                 uint32 plane, Span<uint8> out) noexcept;
    // 형식 종류에 맞는 업로드 경로를 고른다(잠금 안).
    Result<void> RecordAnyUpload(const TextureRecord& record, uint32 mipLevel,
                                 const uint8* pixels) noexcept;
    Result<uint64> FlushUploadsLocked() noexcept;
    Result<void> WaitIdleLocked() noexcept;
    Result<ReadbackRequest> RequestReadbackLocked(TextureHandle target) noexcept;
    Result<bool> TryReadbackLocked(ReadbackHandle handle, Span<uint8> out) noexcept;
    void ReleaseReadbackLocked(ReadbackHandle handle) noexcept;

    // --- 계측(D3D11Timing.cpp) ---

    void InitializeTimestamps() noexcept;
    void HarvestTimestamps() noexcept;
    void CloseMarkers() noexcept;
    // query 결과가 나올 때까지 돈다(GPU가 이미 끝냈거나 곧 끝나는 query에만 쓴다).
    HRESULT PollQuery(ID3D11Query* query, void* data, UINT size) noexcept;

    Allocator& allocator_;
    String adapterName_;
    String lastError_;
    SlotArray<BufferRecord> buffers_;
    SlotArray<TextureRecord> textures_;
    SlotArray<PipelineRecord> pipelines_;
    SlotArray<SwapChainRecord> swapChains_;
    SlotArray<ReadbackRecord> readbacks_;
    SlotArray<BufferViewRecord> bufferViews_;
    Array<Retired> retired_;
    Array<DedicatedUpload> dedicated_;
    Array<UploadOp> uploadOps_;
    SamplerEntry samplers_[SamplerCapacity];
    // 형식 번호별 능력이다(장치 생성 때 채우고 바꾸지 않는다).
    PixelFormatCaps formatCaps_[MaxPixelFormatNumber + 1] = {};
    // 형식 번호별 MSAA 표본 수 집합과 장치 능력(GX1)이다(생성 때 정해진다).
    uint32 sampleCounts_[MaxPixelFormatNumber + 1] = {};
    DeviceCaps caps_;
    uint32 samplerCount_ = 0;
    // AnisotropicPoint를 AnisotropicLinear로 만든 일을 로그에 남겼다(한 번만).
    bool anisotropicPointReported_ = false;
    ComRef<IDXGIFactory4> factory_;
    ComRef<ID3D11Device5> device_;
    ComRef<ID3D11DeviceContext4> immediate_;
    // 프레임 기록 context다(소유 스레드 전용).
    ComRef<ID3D11DeviceContext> deferred_;
    ComRef<ID3DUserDefinedAnnotation> annotation_;
    ComRef<ID3D11InfoQueue> infoQueue_;
    ComRef<IDXGIInfoQueue> dxgiInfoQueue_;
    ComRef<ID3D11Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    // fence 관찰 스레드 전용 이벤트다(소유 스레드의 fenceEvent_와 나눈다).
    HANDLE watcherEvent_ = nullptr;
    // 마지막으로 신호한 fence 값이다.
    uint64 fenceValue_ = 0;
    // 그리기별 상수(b0, VS·PS·CS) 동적 상수 버퍼(MaxDrawConstantBytes)와 그 CPU 사본이다. 사본은
    // 기록 시작 때 0이고 SetDrawConstants가 앞부분을 바꾼다.
    ComRef<ID3D11Buffer> constants_;
    alignas(16) uint8 constantValues_[MaxDrawConstantBytes] = {};
    // 상수 버퍼(9.11절): handle 표, 기록 명령의 (단계, slot)별 DYNAMIC 버퍼와 그 크기, 묶음이다.
    SlotArray<ConstantBufferRecord> constantBuffers_;
    ComRef<ID3D11Buffer> transientConstants_[ConstantBufferStageCount][MaxConstantBufferSlots];
    uint32 transientBytes_[ConstantBufferStageCount][MaxConstantBufferSlots] = {};
    struct BoundConstant {
        SlotHandle handle;
        // 참이면 기록 명령 버퍼(transientConstants_)다.
        bool data = false;
        uint32 bytes = 0;
    };
    BoundConstant boundConstants_[ConstantBufferStageCount][MaxConstantBufferSlots] = {};
    Target target_;
    SwapChainHandle presentTarget_;
    TextureHandle white_;
    SlotHandle boundPipeline_;
    BoundVertexBuffer boundVertex_[MaxVertexStreams];
    BoundIndexBuffer boundIndex_;
    // 그리기 slot에 묶인 버퍼 view다(GX2).
    BufferViewHandle boundViews_[MaxDrawBufferViews];
    // compute 쓰기 slot에 묶인 view(GX4)와 스텐실 기준 값이다.
    BufferViewHandle boundWriteViews_[MaxDrawBufferViews];
    // 지금 패스의 attachment view다(패스마다 그 mip·slice로 만들고 다음 패스까지 붙든다. deferred
    // context와 command list가 기록 동안 참조를 잡는다).
    ComRef<ID3D11RenderTargetView> passRtvs_[MaxColorTargets];
    ComRef<ID3D11DepthStencilView> passDsv_;
    // compute 쓰기 slot에 묶인 텍스처 쓰기 view(버퍼 view와 같은 slot 공간)와 SetTexture로 묶은
    // 텍스처 기록이다(dispatch가 CS t0·s0에 걸고, 그리기가 PS t0을 다시 건다).
    TextureHandle boundWriteTextures_[MaxDrawBufferViews];
    SlotHandle boundTexture_;
    uint32 stencilReference_ = 0;
    // dispatch가 UAV를 묶어 런타임이 입력 binding(정점·index·SRV)을 풀었을 수 있다. 다음 그리기가
    // 정점·index 버퍼를 다시 묶는다.
    bool inputsDirty_ = false;
    uint64 ignoredDraws_ = 0;
    // SetTexture가 흰 텍스처로 대신 묶은 수다(NullTextureBindings).
    uint64 nullTextureBindings_ = 0;
    // 검증 메시지는 읽을 때마다 비우고 여기에 누적한다(저장 한도로 오래된 오류가 밀려나지 않게).
    mutable uint64 validationErrors_ = 0;
    // 검증 메시지 원문을 ID마다 한 번만 남긴다(DrainValidationMessages가 쓴다).
    mutable ValidationMessageFilter validationFilter_;
    // 장치 제거를 이미 남겼는지다(손실 전이 때 한 번, 원자 변수).
    uint32 lossLogged_ = 0;
    mutable Array<uint8> messageScratch_;
    FenceEventSource fenceSource_;
    FenceTimeline timeline_;
    DeviceOwnership ownership_;
    // 장치 내부 잠금이다. 자원 생성·갱신이 다른 스레드에서 올 수 있으므로
    // (concurrentResourceCreation) 자원 표·업로드 기록·회수 목록을 만지는 모든 호출이 잡는다. fence
    // API는 잡지 않는다.
    mutable platform::Mutex gate_;
    // 업로드 ring(GX8): 시스템 메모리 하나를 UploadRingAllocator가 나눠 준다.
    UploadRingAllocator ring_;
    uint8* ringData_ = nullptr;
    uint64 dedicatedUploads_ = 0;
    SystemStagingSource stagingSource_;
    UniquePtr<StagingPool> staging_;
    // GPU 계측(GX9): 프레임 slot별 disjoint query, timestamp query, calibration용 query다. 지원하지
    // 않으면 timestampFrequency_가 0이다.
    ComRef<ID3D11Query> disjointQueries_[TimestampFrameSlots];
    ComRef<ID3D11Query> timestampQueries_[TimestampQueryCount];
    ComRef<ID3D11Query> calibrationDisjoint_;
    ComRef<ID3D11Query> calibrationTimestamp_;
    ComRef<ID3D11Query> calibrationEvent_;
    QueryTimestampSource timestampSource_;
    TimestampRing timestamps_;
    uint64 timestampFrequency_ = 0;
    // 지금 프레임에서 열린 marker 구간 수다.
    uint32 markerDepth_ = 0;
    // 기록 중 열린 분리 전이 짝이다.
    SplitTransitionTracker splits_;
    // 장치 셰이더(CreateShader — 코드와 layout의 CPU 사본)다.
    DeviceShaderTable shaders_;
    bool recording_ = false;
    bool validation_ = false;
    bool software_ = false;
    // 생성 실패의 단계와 원본 HRESULT다(DeviceCreateReport).
    char failureStage_[48] = {};
    HRESULT failureResult_ = S_OK;
    bool failureTruncated_ = false;
    // 마지막으로 시도한 어댑터에 필수 인터페이스(ID3D11Device5·Context4·Fence)가 없었다.
    bool featureMissing_ = false;
};

} // namespace rocks::graphics::d3d11
