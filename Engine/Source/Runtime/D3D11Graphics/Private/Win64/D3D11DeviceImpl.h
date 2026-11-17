#pragma once

#include "D3D11Common.h"

#include <Core/Containers/Array.h>
#include <Core/Containers/SlotArray.h>
#include <Core/Text/String.h>
#include <D3D11Graphics/D3D11Device.h>
#include <Graphics/DeviceOwnership.h>
#include <Graphics/FenceTimeline.h>
#include <Graphics/StagingPool.h>
#include <Graphics/TimestampRing.h>
#include <Graphics/UploadRing.h>
#include <Platform/Sync.h>

#include <d3dcompiler.h>
#include <dxgidebug.h>

// D3D11 장치 구현의 선언이다. 정의는 기능별 파일에 나눈다.
// - D3D11Device.cpp: 생성·장치 정보·fence·소유권·회수·검증 집계
// - D3D11Resources.cpp: 스왑체인·버퍼·텍스처·파이프라인·샘플러·Retire
// - D3D11Frame.cpp: 프레임 시작·제출과 기록(CommandContext)
// - D3D11Upload.cpp: 업로드 기록·ring·staging·readback·WaitIdle
// - D3D11Timing.cpp: timestamp·calibration·marker
namespace rocks::graphics::d3d11 {

struct BufferRecord {
    ComRef<ID3D11Buffer> buffer;
    uint64 size = 0;
};

struct TextureRecord {
    ComRef<ID3D11Texture2D> texture;
    ComRef<ID3D11ShaderResourceView> srv;
    // RenderTarget만 있다.
    ComRef<ID3D11RenderTargetView> rtv;
    uint32 width = 0;
    uint32 height = 0;
    uint32 mipLevels = 1;
    Format format = Format::Unknown;
    bool renderTarget = false;
    // 샘플러 캐시의 객체다(장치 수명 동안 산다).
    ID3D11SamplerState* sampler = nullptr;
};

struct PipelineRecord {
    ComRef<ID3D11VertexShader> vertexShader;
    ComRef<ID3D11PixelShader> pixelShader;
    // 정점 입력이 없는 배치면 비어 있다.
    ComRef<ID3D11InputLayout> inputLayout;
    bool alphaBlend = false;
    // stream별 stride와 배치가 선언한 stream mask다(그리기 때 묶인 정점 버퍼를 확인한다).
    uint32 strides[MaxVertexStreams] = {};
    uint32 streamMask = 0;
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
        Copy
    };

    Kind kind = Kind::Update;
    // 대상은 자원 표나 회수 항목(uploadPending)이 이 명령이 실행될 때까지 살려 둔다.
    ID3D11Resource* destination = nullptr;
    ID3D11Resource* copySource = nullptr;
    uint32 subresource = 0;
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
    Result<SwapChainHandle> CreateSwapChain(const SwapChainDesc& desc) noexcept override;
    Result<void> ResizeSwapChain(SwapChainHandle swapChain, uint32 width,
                                 uint32 height) noexcept override;
    Result<BufferHandle> CreateBuffer(const BufferDesc& desc,
                                      Span<const uint8> initial) noexcept override;
    Result<TextureHandle> CreateTexture(const TextureDesc& desc,
                                        Span<const uint8> pixels) noexcept override;
    Result<PipelineHandle> CreatePipeline(const PipelineDesc& desc) noexcept override;
    Result<CommandContext*> BeginFrame(SwapChainHandle swapChain,
                                       const ClearColor& clear) noexcept override;
    Result<CommandContext*> BeginOffscreen(TextureHandle target,
                                           const ClearColor& clear) noexcept override;
    Result<GpuFencePoint> EndFrame() noexcept override;
    Result<void> ReadPixels(TextureHandle target, Span<uint8> out) noexcept override;
    void Retire(BufferHandle buffer) noexcept override;
    void Retire(TextureHandle texture) noexcept override;
    void Retire(PipelineHandle pipeline) noexcept override;
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
    Result<ClockCalibration> CalibrateClocks() noexcept override;
    GpuFencePoint LastSubmittedFence() const noexcept override;
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
    uint64 ValidationErrorCount() const noexcept override;
    usize PendingRetireCount() const noexcept override;

private:
    // 기록 대상(백버퍼 또는 RenderTarget 텍스처)이다.
    struct Target {
        ID3D11RenderTargetView* rtv = nullptr;
        uint32 width = 0;
        uint32 height = 0;
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
    void SetConstants(const DrawConstants& constants) noexcept override;
    void Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept override;
    void DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex) noexcept override;
    Result<uint64> WriteTimestamp() noexcept override;
    void PushMarker(StringView name) noexcept override;
    void PopMarker() noexcept override;

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

    template <class A, class B = IUnknown, class C = IUnknown>
    void Defer(ComRef<A>&& first, ComRef<B>&& second = ComRef<B>(),
               ComRef<C>&& third = ComRef<C>()) noexcept {
        // 모든 D3D 인터페이스는 IUnknown을 단일 상속하므로 포인터 변환이 안전하다.
        ComRef<IUnknown> objects[MaxRetiredObjects];
        objects[0].Attach(first.Detach());
        objects[1].Attach(second.Detach());
        objects[2].Attach(third.Detach());
        RetireObjects(objects);
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
    Result<void> Compile(StringView source, const char* entry, const char* target,
                         ComRef<ID3DBlob>& code) noexcept;
    Result<void> MatchVertexInputs(ID3DBlob* code, const VertexLayout& layout) noexcept;
    Result<void> CheckVertexFormats(const VertexLayout& layout) noexcept;
    Result<void> CreateSwapChainView(SwapChainRecord& record) noexcept;

    // --- 기록(D3D11Frame.cpp) ---

    void BeginRecording(const Target& target, const ClearColor& clear) noexcept;
    void WriteConstants(const DrawConstants& constants) noexcept;
    void ApplyVertexBuffer(uint32 stream) noexcept;
    bool StreamsBound() const noexcept;

    // --- 업로드(D3D11Upload.cpp) ---

    Result<UploadSpace> StageUpload(uint64 size) noexcept;
    Result<void> PushUpload(const UploadOp& op) noexcept;
    Result<void> RecordBufferUpload(ID3D11Buffer* target, uint64 offset,
                                    Span<const uint8> data) noexcept;
    Result<void> RecordTextureUpload(const TextureRecord& record, uint32 mipLevel,
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
    Array<Retired> retired_;
    Array<DedicatedUpload> dedicated_;
    Array<UploadOp> uploadOps_;
    SamplerEntry samplers_[SamplerCapacity];
    uint32 samplerCount_ = 0;
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
    // 파이프라인끼리 공유하는 고정 상태다.
    ComRef<ID3D11RasterizerState> rasterizer_;
    ComRef<ID3D11DepthStencilState> depthDisabled_;
    ComRef<ID3D11BlendState> opaqueBlend_;
    ComRef<ID3D11BlendState> alphaBlend_;
    // 그리기 상수(DrawConstants, b0) 동적 상수 버퍼다.
    ComRef<ID3D11Buffer> constants_;
    Target target_;
    SwapChainHandle presentTarget_;
    TextureHandle white_;
    SlotHandle boundPipeline_;
    BoundVertexBuffer boundVertex_[MaxVertexStreams];
    BoundIndexBuffer boundIndex_;
    uint64 ignoredDraws_ = 0;
    // 검증 메시지는 읽을 때마다 비우고 여기에 누적한다(저장 한도로 오래된 오류가 밀려나지 않게).
    mutable uint64 validationErrors_ = 0;
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
