#pragma once

#include "GlesApi.h"
#include "GlesFormats.h"
#include "GlesPlatform.h"

#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Text/String.h>
#include <Graphics/DeviceOwnership.h>
#include <Graphics/FenceTimeline.h>
#include <Graphics/StagingPool.h>
#include <Graphics/TimestampRing.h>

// OpenGL ES 3.2 장치의 공통 구현이다(설계: Docs/Design/OpenGLESGraphics.ko.md). 플랫폼 층
// (GlesPlatform)이 만든 context 위에서 ES 3.2 core API만 쓴다. 구현은 기능별로 나눈다:
// GlesDevice.cpp(생성·자원·회수), GlesDeviceFrame.cpp(프레임·기록·present), GlesDeviceTransfer.cpp
// (업로드·readback·계측·소유권).
namespace rocks::graphics::gles {

// 텍스처 유닛 배정이다(ShaderSystem 7.2절 4항): PS·CS 0부터, VS 16부터. 업로드는 그리기 유닛과
// 겹치지 않는 유닛에서 묶는다(ES 3.2의 GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 최소 96 안).
inline constexpr GLuint PixelTextureUnit = 0;
inline constexpr GLuint VertexTextureUnitBase = 16;
inline constexpr GLuint UploadTextureUnit = 32;

// 그리기 상수(DrawConstants)를 담는 루트 상수 에뮬레이션 배열의 이름과 vec4 수다(ShaderSystem
// 7.2절 4항: `<단계>u_<타입>`, UE `$Globals`의 pu_h 자리).
inline constexpr char VertexRootConstantsName[] = "vu_h";
inline constexpr char PixelRootConstantsName[] = "pu_h";
inline constexpr GLsizei RootConstantVectors = sizeof(DrawConstants) / (4 * sizeof(float));
static_assert(sizeof(DrawConstants) == RootConstantVectors * 4 * sizeof(float),
              "DrawConstants must be a whole number of vec4 slots");

// 로그 카테고리다(LoggingSystem 13절: 장치 생성·present 실패·장치 손실, 검증 계층 메시지).
inline constexpr LogCategory LogGraphicsOpenGL{"Graphics.OpenGL"};
inline constexpr LogCategory LogGraphicsOpenGLValidation{"Graphics.OpenGL.Validation"};

// 검증 메시지를 ID마다 한 번 남기려고 기억하는 ID 수다. 다 차면 새 ID는 세기만 하고 남기지 않는다
// (할당 없이 반복 로그를 막는다).
inline constexpr uint32 MaxLoggedValidationIds = 64;

// 셰이더·프로그램 info log를 옮길 때 쓰는 고정 버퍼 크기다(넘는 부분은 자른다).
inline constexpr GLsizei InfoLogBytes = 2048;

// 텍스처 한 변의 최대 크기다(ES 3.2 GL_MAX_TEXTURE_SIZE 최소 보장은 2048이고 장치 값으로 다시
// 줄인다. D3D12와 같은 상한 16384를 넘지 않는다).
inline constexpr uint32 MaxTextureExtent = 16384;

struct BufferRecord {
    GLuint buffer = 0;
    uint64 size = 0;
};

struct TextureRecord {
    GLuint texture = 0;
    // 텍스처마다의 샘플러 객체다(TextureDesc::sampler).
    GLuint sampler = 0;
    // RenderTarget이면 색 attachment 하나의 framebuffer다.
    GLuint framebuffer = 0;
    uint32 width = 0;
    uint32 height = 0;
    uint32 mipLevels = 1;
    PixelFormat format = PixelFormat::Unknown;
};

struct PipelineRecord {
    GLuint program = 0;
    GLuint vertexArray = 0;
    // 루트 상수 에뮬레이션 배열 위치다(셰이더가 쓰지 않으면 -1).
    GLint vertexConstants = -1;
    GLint pixelConstants = -1;
    // stream별 stride와 배치가 선언한 stream mask다.
    uint32 strides[MaxVertexStreams] = {};
    uint32 streamMask = 0;
    bool alphaBlend = false;
    CullMode cullMode = CullMode::None;
};

struct SwapChainRecord {
    PlatformSurface surface = nullptr;
    // 프레임을 그리는 offscreen 색 텍스처와 그 framebuffer다(저장소 첫 행 = 이미지 위쪽). present가
    // Y를 뒤집어 창에 blit한다.
    GLuint texture = 0;
    GLuint framebuffer = 0;
    uint32 width = 0;
    uint32 height = 0;
    bool vsync = true;
};

// fence 완료 뒤에 지울 GL 객체다(장치 계약: OpenGL도 fence 뒤 회수).
enum class RetiredKind : uint8 {
    Buffer,
    Texture,
    Pipeline,
    SwapChain
};

struct RetiredObject {
    RetiredKind kind = RetiredKind::Buffer;
    // Buffer: 버퍼. Texture: 텍스처·샘플러·framebuffer. Pipeline: 프로그램·정점 배열.
    // SwapChain: 텍스처·framebuffer와 표면.
    GLuint objects[3] = {};
    PlatformSurface surface = nullptr;
    uint64 fenceValue = 0;
    // 기록 중인(아직 제출하지 않은) 프레임이 쓸 수 있다. EndFrame이 그 프레임의 fence 값을 정한다.
    bool pending = false;
};

// 비동기 readback 하나다(pixel pack buffer). fenceValue 제출이 끝나면 map해서 읽는다.
struct ReadbackRecord {
    GLuint buffer = 0;
    uint64 fenceValue = 0;
    uint32 width = 0;
    uint32 height = 0;
    // 읽을 byte 수다(width × height 이미지 하나).
    uint64 bytes = 0;
};

// OpenGL staging은 CPU 메모리다(TextureStreaming 5.4절). client 메모리 업로드(glTexSubImage2D·
// glBufferSubData)는 호출이 반환되면 원본을 다시 써도 되므로 fence를 기다리지 않는다.
class GlesStagingSource final : public StagingSource {
public:
    Result<StagingMemory> CreateStaging(uint64 bytes) noexcept override;
    void DestroyStaging(const StagingMemory& memory) noexcept override;

    Allocator* allocator = nullptr;
};

// timestamp query 결과를 읽는다(GX9). Harvest가 fence 완료 뒤에만 부르지만 결과가 아직 보이지 않을
// 수 있으므로 가용 여부를 먼저 본다. GL_EXT_disjoint_timer_query가 있으면 끊김 표시를 읽어 그
// 묶음을 무효로 한다.
class GlesTimestampSource final : public TimestampSource {
public:
    TimestampReadState ReadTimestamps(uint32 first, uint32 count, uint64* ticks) noexcept override;

    const GlesFunctions* gl = nullptr;
    const GlesTimerQueryFunctions* timer = nullptr;
    const GLuint* queries = nullptr;
    bool disjointDetection = false;
};

class GlesDevice final : public Device, private CommandContext {
public:
    explicit GlesDevice(Allocator& allocator) noexcept;
    ~GlesDevice() override;

    // context를 만들고 기본 상태·흰색 텍스처·관찰 스레드를 준비한다. 실패하면 failure에 적는다.
    Result<void> Initialize(const DeviceDesc& desc, const OpenGLDeviceOptions& options,
                            GlesCreateFailure& failure) noexcept;

    // --- Device ---
    DeviceInfo Info() const noexcept override;
    Result<SwapChainHandle> CreateSwapChain(const SwapChainDesc& desc) noexcept override;
    Result<void> ResizeSwapChain(SwapChainHandle handle, uint32 width,
                                 uint32 height) noexcept override;
    Result<BufferHandle> CreateBuffer(const BufferDesc& desc,
                                      Span<const uint8> initial) noexcept override;
    Result<TextureHandle> CreateTexture(const TextureDesc& desc,
                                        Span<const uint8> pixels) noexcept override;
    Result<PipelineHandle> CreatePipeline(const PipelineDesc& desc) noexcept override;
    Result<CommandContext*> BeginFrame(SwapChainHandle handle,
                                       const ClearColor& clear) noexcept override;
    Result<CommandContext*> BeginOffscreen(TextureHandle handle,
                                           const ClearColor& clear) noexcept override;
    Result<GpuFencePoint> EndFrame() noexcept override;
    Result<void> ReadPixels(TextureHandle handle, Span<uint8> out) noexcept override;
    void Retire(BufferHandle handle) noexcept override;
    void Retire(TextureHandle handle) noexcept override;
    void Retire(PipelineHandle handle) noexcept override;
    void Retire(SwapChainHandle handle) noexcept override;
    void ReclaimCompleted() noexcept override;
    Result<void> WaitIdle() noexcept override;
    Result<void> UpdateBuffer(BufferHandle handle, uint64 offset,
                              Span<const uint8> data) noexcept override;
    Result<void> UpdateTexture(TextureHandle handle, uint32 mipLevel,
                               Span<const uint8> pixels) noexcept override;
    Result<GpuFencePoint> SubmitUploads() noexcept override;
    UploadStats UploadStatistics() const noexcept override;
    Result<StagingBuffer> AcquireStaging(uint64 bytes) noexcept override;
    Result<void> UploadTextureFromStaging(TextureHandle handle, uint32 mipLevel,
                                          StagingHandle staging, uint64 offset) noexcept override;
    Result<void> UploadBufferFromStaging(BufferHandle handle, uint64 destinationOffset,
                                         StagingHandle staging, uint64 sourceOffset,
                                         uint64 size) noexcept override;
    void ReleaseStaging(StagingHandle staging) noexcept override;
    StagingStats StagingStatistics() const noexcept override;
    Result<ReadbackRequest> RequestReadback(TextureHandle target) noexcept override;
    Result<bool> TryReadback(ReadbackHandle handle, Span<uint8> out) noexcept override;
    void ReleaseReadback(ReadbackHandle handle) noexcept override;
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
    usize PendingRetireCount() const noexcept override;
    StringView LastError() const noexcept override;
    uint64 IgnoredDraws() const noexcept override;
    uint64 ValidationErrorCount() const noexcept override;

private:
    // --- CommandContext ---
    void SetPipeline(PipelineHandle handle) noexcept override;
    void SetVertexBuffers(uint32 firstStream,
                          Span<const VertexBufferBinding> buffers) noexcept override;
    void SetIndexBuffer(BufferHandle buffer, IndexFormat format, uint64 offset) noexcept override;
    void SetTexture(TextureHandle handle) noexcept override;
    void SetConstants(const DrawConstants& constants) noexcept override;
    void Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept override;
    void DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex) noexcept override;
    Result<void> CopyPassTarget(TextureHandle destination) noexcept override;
    Result<uint64> WriteTimestamp() noexcept override;
    void PushMarker(StringView name) noexcept override;
    void PopMarker() noexcept override;

    // 검증 메시지 id를 처음 보면 기억하고 true다(이미 남겼거나 기억 칸이 차면 false).
    bool FirstValidationMessage(GLuint id) noexcept;

    static void DebugMessage(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                             const GLchar* message, const void* user);

    // message를 LastError에 두고 DependencyFailed로 실패한다. 직전 GL 오류가 있으면 그 이름과
    // 값을 덧붙인다(원본 오류 보존).
    Result<void> Fail(const char* message) noexcept;

    template <class T>
    Result<T> FailWith(const char* message) noexcept {
        auto failed = Fail(message);
        return Result<T>::Failure(failed.Error());
    }

    // message를 LastError에 두고 error로 실패한다.
    Result<void> Reject(const Diagnostic& message, ErrorCode error) noexcept;

    // 자원 생성·파괴는 어느 표면에 붙어 있든 이 context면 된다. 붙어 있지 않을 때만 장치 표면에
    // 붙인다.
    bool MakeCurrentForResources() noexcept;

    // graphics queue에 fence를 넣고 다음 값을 돌려준다. fence를 만들지 못하면 glFinish로 끝내고
    // token 없이 알린다(관찰 스레드가 곧바로 완료로 본다). context를 붙일 수 없으면 완료를 지어내지
    // 않고 장치 손실로 게시하며 값을 올리지 않는다(돌려준 값이 직전 값과 같다).
    uint64 Submit() noexcept;

    void Defer(RetiredObject entry) noexcept;
    void AssignPendingRetires(uint64 value) noexcept;
    void DeleteRetired(uint64 completed) noexcept;
    void DeleteObject(const RetiredObject& entry) noexcept;
    void ReleaseTexture(const TextureRecord& record) noexcept;

    // 스왑체인의 offscreen 색 텍스처를 width × height로 (다시) 만든다.
    bool CreateBackBuffer(SwapChainRecord& record, uint32 width, uint32 height) noexcept;

    // 색 attachment 하나의 framebuffer를 만든다. 완전하지 않으면 0이다.
    GLuint CreateColorFramebuffer(GLuint texture) noexcept;

    // 텍스처 하나의 샘플러 객체를 만든다.
    GLuint CreateSampler(const SamplerDesc& desc) noexcept;

    // texture mip 하나를 올린다(업로드 유닛에서 묶는다).
    void UploadMip(const TextureRecord& record, uint32 level, const uint8* pixels) noexcept;

    Result<void> MatchVertexInputs(GLuint program, const VertexLayout& layout) noexcept;
    Result<GLuint> CompileShader(GLenum stage, StringView source) noexcept;
    void StoreLog(GLuint object, bool shader) noexcept;

    void InitializeTimestamps() noexcept;
    void StartPass(GLuint framebuffer, uint32 width, uint32 height,
                   const ClearColor& clear) noexcept;
    void BindPassTexture(const TextureRecord& record) noexcept;
    void ApplyVertexBuffer(uint32 stream) noexcept;
    void ApplyIndexBuffer() noexcept;
    const PipelineRecord* DrawablePipeline() const noexcept;
    void ApplyConstants(const PipelineRecord& pipeline) noexcept;

    // 스왑체인 텍스처를 창 표면의 기본 framebuffer에 Y를 뒤집어 blit한다(장치 context를 그
    // 표면에 붙인다). 끝나면 기록 중인 패스 대상과 viewport를 다시 묶는다. 표면에 붙이지 못하면
    // 장치 표면으로 되돌리고 false다.
    bool PresentBlit(const SwapChainRecord& record) noexcept;

    String adapterName_;
    String lastError_;
    SlotArray<BufferRecord> buffers_;
    SlotArray<TextureRecord> textures_;
    SlotArray<PipelineRecord> pipelines_;
    SlotArray<SwapChainRecord> swapChains_;
    Array<RetiredObject> retired_;
    SlotArray<ReadbackRecord> readbacks_;
    GlesStagingSource stagingSource_;
    UniquePtr<StagingPool> staging_;
    UniquePtr<GlesPlatform> platform_;
    // platform_->Gl()의 사본이다(호출마다 간접 참조를 줄인다).
    GlesFunctions gl_;
    GlesFeatures features_;
    GlesTimerQueryFunctions timer_;
    // GPU 계측(GX9): timestamp query 묶음(slot × 프레임당 한도)과 프레임 slot 장부다. GL_TIMESTAMP
    // 값의 단위는 ns다.
    static constexpr uint32 TimestampQueryCount = TimestampFrameSlots * MaxTimestampsPerFrame;
    static constexpr uint64 TimestampNanosecondFrequency = 1000000000;
    GLuint timestampQueries_[TimestampQueryCount] = {};
    GlesTimestampSource timestampSource_;
    TimestampRing timestamps_;
    bool timestampsReady_ = false;
    // 지금 프레임에서 열린 debug group 수다.
    uint32 markerDepth_ = 0;
    uint32 maxTextureExtent_ = 0;
    // 기록 중인 패스의 대상이다(CopyPassTarget). 스왑체인이면 swapChain이 있고 texture는 null이다.
    struct PassTarget {
        GLuint framebuffer = 0;
        uint32 width = 0;
        uint32 height = 0;
        PixelFormat format = PixelFormat::Unknown;
        TextureHandle texture;
        SwapChainHandle swapChain;
    };
    PassTarget pass_;
    TextureHandle white_;
    SwapChainHandle presentTarget_;
    PipelineHandle boundPipeline_;
    // 기록 중 묶인 정점·index 버퍼다(파이프라인을 바꾸면 그 정점 배열에 다시 묶는다).
    struct BoundVertexBuffer {
        BufferHandle buffer;
        uint64 offset = 0;
    };
    struct BoundIndexBuffer {
        BufferHandle buffer;
        IndexFormat format = IndexFormat::UInt16;
        uint64 offset = 0;
    };
    BoundVertexBuffer boundVertex_[MaxVertexStreams];
    BoundIndexBuffer boundIndex_;
    DrawConstants constants_;
    uint64 ignoredDraws_ = 0;
    uint64 validationErrors_ = 0;
    // 마지막으로 제출한 fence 값이다.
    uint64 fenceValue_ = 0;
    FenceTimeline timeline_;
    DeviceOwnership ownership_;
    bool recording_ = false;
    bool constantsDirty_ = false;
    bool validation_ = false;
    // present·패스 복사 blit에서 sRGB 쓰기 스위치를 끄고 byte를 그대로 옮기는지다(PlatformInfo::
    // rawSrgbBlit이고 스위치가 있을 때).
    bool rawSrgbBlit_ = false;
    // 이미 로그로 남긴 검증 메시지 ID다(ID마다 한 번).
    GLuint loggedValidationIds_[MaxLoggedValidationIds] = {};
    uint32 loggedValidationCount_ = 0;
    // present 실패를 남겼는지다(연속 실패는 한 번만 남기고 성공하면 다시 남긴다).
    bool presentFailureLogged_ = false;
    // 장치 손실 전이를 남겼는지다.
    bool lossLogged_ = false;
};

} // namespace rocks::graphics::gles
