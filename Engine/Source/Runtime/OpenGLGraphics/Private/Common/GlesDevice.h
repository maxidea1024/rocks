#pragma once

#include "GlesApi.h"
#include "GlesFormats.h"
#include "GlesPlatform.h"

#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Text/String.h>
#include <Graphics/DeviceOwnership.h>
#include <Graphics/FenceTimeline.h>
#include <Graphics/SplitTransitionTracker.h>
#include <Graphics/StagingPool.h>
#include <Graphics/TimestampRing.h>

// OpenGL ES 3.2 장치의 공통 구현이다(설계: Docs/Design/OpenGLESGraphics.ko.md). 플랫폼 층
// (GlesPlatform)이 만든 context 위에서 ES 3.2 core API만 쓴다. 구현은 기능별로 나눈다:
// GlesDevice.cpp(생성·자원·회수), GlesDeviceFrame.cpp(프레임·기록·present), GlesDeviceTransfer.cpp
// (업로드·readback·계측·소유권), GlesDeviceBuffers.cpp(장치 능력·버퍼 view), GlesDeviceCommands.cpp
// (파이프라인 고정 기능 상태·compute·패스 attachment·간접 명령·복사·UAV clear, GX3·GX4).
namespace rocks::graphics::gles {

// 텍스처 유닛 배정이다(ShaderSystem 7.2절 4항): PS·CS 0부터, VS 16부터. 업로드는 그리기 유닛과
// 겹치지 않는 유닛에서 묶는다(ES 3.2의 GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 최소 96 안).
inline constexpr GLuint PixelTextureUnit = 0;
inline constexpr GLuint VertexTextureUnitBase = 16;
inline constexpr GLuint UploadTextureUnit = 32;
// 버퍼 view slot n의 typed view(samplerBuffer)는 PS 유닛 PixelBufferViewUnitBase + n, VS 유닛
// VertexBufferViewUnitBase + n이다(PS 텍스처 0과 VS 16 뒤, PipelineDesc 고정 배치). 구조화·raw
// view는 storage block binding n이다.
inline constexpr GLuint PixelBufferViewUnitBase = PixelTextureUnit + 1;
inline constexpr GLuint VertexBufferViewUnitBase = VertexTextureUnitBase + 1;
// 조회 수단이 없는 버퍼 최대 크기의 보수적 값이다(D3D 보장식의 하한 128 MiB, DeviceLimits).
inline constexpr uint64 ConservativeBufferBytes = uint64{128} << 20;
// raw view로 읽는 storage block 이름의 접두사다(SPIRV-Cross가
// ByteAddressBuffer·RWByteAddressBuffer에 붙이는 이름). 그 밖의 storage block은 구조화 view다.
inline constexpr char RawStorageBlockPrefix[] = "type_ByteAddressBuffer";
inline constexpr char RawWritableStorageBlockPrefix[] = "type_RWByteAddressBuffer";
// compute 쓰기 view slot n의 구조화·raw storage block binding은 WritableStorageBindingBase + n이고
// typed는 image unit n이다(ComputePipelineDesc 고정 배치).
inline constexpr GLuint WritableStorageBindingBase = MaxDrawBufferViews;

// 그리기별 상수를 담는 루트 상수 에뮬레이션 배열(uniform highp vec4 <이름>[N])의 이름과 vec4 한
// 칸의 byte 수다(ShaderSystem 7.2절 4항: `<단계>u_<타입>`, UE `$Globals`의 pu_h 자리). N은 셰이더가
// 정하며 N × 16이 파이프라인의 drawConstantBytes 이하여야 한다.
inline constexpr char VertexRootConstantsName[] = "vu_h";
inline constexpr char PixelRootConstantsName[] = "pu_h";
inline constexpr char ComputeRootConstantsName[] = "cu_h";
inline constexpr uint32 RootConstantVectorBytes = 4 * sizeof(GLfloat);
static_assert(MaxDrawConstantBytes % RootConstantVectorBytes == 0,
              "Draw constants must be a whole number of vec4 slots");

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

// mip 사슬 길이의 상한이다(16384 한 변의 전체 사슬 15단 + 여유, D3D·Vulkan backend와 같다).
inline constexpr uint32 MaxMipLevels = 16;

struct BufferRecord {
    GLuint buffer = 0;
    uint64 size = 0;
    BufferDesc desc;
    // 살아 있는 view 수다. 0이 아닐 때 Retire하면 버퍼 이름은 orphanBuffers_로 옮겨 마지막 view를
    // 놓을 때 지운다(storage view는 그리기 때 버퍼 이름으로 묶는다).
    uint32 viewCount = 0;
};

struct OrphanBuffer {
    GLuint buffer = 0;
    uint32 viewCount = 0;
};

// 버퍼 view(GX2)다. typed는 버퍼 텍스처 객체(glTexBufferRange), 구조화·raw는 glBindBufferRange로
// 묶는 (buffer, offset, size)다.
struct BufferViewRecord {
    GLuint texture = 0;
    GLuint buffer = 0;
    SlotHandle source;
    uint64 offset = 0;
    uint64 size = 0;
    // 구조화 view의 원소 byte다(셰이더 선언 stride와 대조한다).
    uint32 stride = 0;
    PixelFormat format = PixelFormat::Unknown;
    BufferViewKind kind = BufferViewKind::Typed;
    ViewAccess access = ViewAccess::Read;
    PixelShaderType shaderType = PixelShaderType::None;
};

struct TextureRecord {
    GLuint texture = 0;
    // 텍스처마다의 샘플러 객체다(TextureDesc::sampler). 선형 필터를 받지 않는 형식(정수·깊이)은
    // 최근접 필터로 만든다(ES에서 그 조합은 불완전 텍스처다).
    GLuint sampler = 0;
    // RenderTarget이면 색 attachment 하나의 framebuffer다.
    GLuint framebuffer = 0;
    // 차원과 그 GL 대상(TEXTURE_2D·2D_ARRAY·3D·CUBE_MAP·CUBE_MAP_ARRAY)이다.
    TextureDimension dimension = TextureDimension::Texture2D;
    GLenum target = GL_TEXTURE_2D;
    uint32 width = 0;
    uint32 height = 0;
    // 3D 깊이다(다른 차원은 1).
    uint32 depth = 1;
    uint32 mipLevels = 1;
    // subresource slice 수(TextureSliceCount)다. 2D 배열·cube 배열은 GL layer 수, cube는 면 6이다.
    uint32 slices = 1;
    PixelFormat format = PixelFormat::Unknown;
    // 묶을 때 셰이더가 읽는 성분 대응이다(TextureDesc·TextureViewDesc::swizzle). 성분 대응은 GL
    // 텍스처 객체 상태라 view와 원 텍스처가 공유하므로, 묶을 때마다 형식 흉내(A8)·스텐실 plane
    // 대응과 합성해 TEXTURE_SWIZZLE_R/G/B/A로 다시 정한다(BindPassTexture).
    ChannelSwizzle swizzle;
    // TextureUsage::DepthStencil로 만들었다(초기 데이터 없음).
    bool depthTarget = false;
    // TextureUsage::RenderTarget으로 만들었다. framebuffer는 2D만 갖는다(2D 밖 차원·다른 mip은
    // 패스 framebuffer에 붙인다, 텍스처 GX2 C부).
    bool renderTarget = false;
    // 만든 용도다(view는 원 텍스처의 용도, ResolveTextureViewRange).
    TextureUsage usage = TextureUsage::Sampled;
    // MSAA 표본 수다(TextureDesc::sampleCount). 2 이상이면 target이 TEXTURE_2D_MULTISAMPLE이다.
    uint32 sampleCount = 1;
    // view의 mip·slice 범위다(view가 아니면 전체). texture view가 없어 읽기 view는 텍스처 전체를
    // 묶고(셰이더가 TextureViewBaseMip을 쓴다), 쓰기 view는 이 범위의 level을 image로 묶는다.
    ResolvedSubresourceRange viewRange;
    // 쓰기 view(ViewAccess::ReadWrite)다. SetWritableTexture로만 묶는다(glBindImageTexture).
    bool writable = false;
    // 깊이·스텐실 텍스처를 묶을 때 읽는 plane이다(GL_DEPTH_STENCIL_TEXTURE_MODE, 1이면 스텐실).
    // 이 값은 텍스처 객체 상태라 묶을 때마다 다시 정한다.
    uint32 plane = 0;
    // GL 텍스처 객체가 깊이·스텐실 형식이다(묶을 때 plane으로 읽기 대상을 정한다). view도 원
    // 텍스처의 값을 갖는다.
    bool depthStencilTexture = false;
    // 텍스처 생성 때의 샘플러 설명이다(view가 같은 설명으로 자기 샘플러를 만든다).
    SamplerDesc samplerDesc;
    // 다른 텍스처의 GL 텍스처 객체를 공유하는 view다(CreateTextureView). 텍스처 객체는 원 기록이
    // 놓는다. 업로드·readback·그리기 대상이 될 수 없다.
    bool isView = false;
    SlotHandle source;
    // 이 텍스처를 가리키는 살아 있는 view 수다. 0이 아니면 Retire는 기록을 남겨 두고
    // (retirePending) 마지막 view를 놓을 때 함께 놓는다(텍스처 객체 이름을 view보다 먼저 지우지
    // 않는다).
    uint32 viewCount = 0;
    bool retirePending = false;
};

// mip level 하나의 데이터 byte다(모든 slice·깊이 면, TextureMipBytes와 같은 값).
inline uint64 MipBytes(const TextureRecord& record, uint32 level) noexcept {
    const uint32 depth =
        record.dimension == TextureDimension::Texture3D ? MipExtent(record.depth, level) : 1u;
    return PixelFormatImageBytes(record.format, MipExtent(record.width, level),
                                 MipExtent(record.height, level)) *
           depth * record.slices;
}

struct PipelineRecord {
    GLuint program = 0;
    GLuint vertexArray = 0;
    // 루트 상수 에뮬레이션 배열 위치(셰이더가 쓰지 않으면 -1)와 active vec4 수다.
    GLint vertexConstants = -1;
    GLint pixelConstants = -1;
    GLint computeConstants = -1;
    GLsizei vertexConstantVectors = 0;
    GLsizei pixelConstantVectors = 0;
    GLsizei computeConstantVectors = 0;
    // 선언한 그리기별 상수 byte 수다(SetDrawConstants 크기 검사).
    uint32 drawConstantBytes = 0;
    // stream별 stride와 배치가 선언한 stream mask다.
    uint32 strides[MaxVertexStreams] = {};
    uint32 streamMask = 0;
    // 고정 기능 상태다(GX3). SetPipeline이 GL 상태로 건다.
    GLenum topology = GL_TRIANGLES;
    uint32 colorTargetCount = 1;
    ColorTargetBlend blend[MaxColorTargets];
    bool alphaToCoverage = false;
    DepthStencilState depthStencil;
    RasterState raster;
    // 깊이·스텐실을 쓴다(읽기 전용 깊이 attachment에서 그리기를 거른다).
    bool writesDepthStencil = false;
    // compute 프로그램이다(Dispatch만 쓴다).
    bool compute = false;
    // VS·PS(compute면 CS)가 읽는 버퍼 view slot과 CS가 쓰는 slot이다(program introspection).
    DrawBufferSlots bufferSlots;
    DrawBufferSlots writeSlots;
    // compute가 텍스처 image로 쓰는 쓰기 slot 비트다(SetWritableTexture가 묶여야 한다).
    uint32 writeTextures = 0;
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
    // 호출자에게 줄 byte 수다(width × height 이미지 하나, Rocks 배치).
    uint64 bytes = 0;
    // pack buffer의 byte 수다. 표준 읽기 조합으로 읽었으면(convert) Rocks 배치로 옮겨 준다.
    uint64 readBytes = 0;
    PixelFormat format = PixelFormat::Unknown;
    bool convert = false;
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
    const DeviceCaps& Caps() const noexcept override;
    uint32 GetPixelFormatSampleCounts(PixelFormat format) const noexcept override;
    Result<BufferViewHandle> CreateBufferView(BufferHandle buffer,
                                              const BufferViewDesc& desc) noexcept override;
    Result<void> ReadBuffer(BufferHandle buffer, uint64 offset, Span<uint8> out) noexcept override;
    Result<SwapChainHandle> CreateSwapChain(const SwapChainDesc& desc) noexcept override;
    Result<void> ResizeSwapChain(SwapChainHandle handle, uint32 width,
                                 uint32 height) noexcept override;
    Result<BufferHandle> CreateBuffer(const BufferDesc& desc,
                                      Span<const uint8> initial) noexcept override;
    Result<TextureHandle> CreateTexture(const TextureDesc& desc,
                                        Span<const uint8> pixels) noexcept override;
    Result<PipelineHandle> CreatePipeline(const PipelineDesc& desc) noexcept override;
    Result<PipelineHandle> CreateComputePipeline(const ComputePipelineDesc& desc) noexcept override;
    PixelFormatCaps GetPixelFormatCaps(PixelFormat format) const noexcept override;
    Result<TextureHandle> CreateTextureView(TextureHandle texture,
                                            const TextureViewDesc& desc) noexcept override;
    Result<TextureSubresources>
    GetTextureSubresources(TextureHandle texture) const noexcept override;
    Result<void> ReadTexture(TextureHandle texture, uint32 mipLevel, uint32 plane,
                             Span<uint8> out) noexcept override;
    Result<CommandContext*> BeginFrame(SwapChainHandle handle,
                                       const ClearColor& clear) noexcept override;
    Result<CommandContext*> BeginPass(const PassDesc& pass) noexcept override;
    Result<GpuFencePoint> EndFrame() noexcept override;
    Result<void> ReadPixels(TextureHandle handle, Span<uint8> out) noexcept override;
    void Retire(BufferHandle handle) noexcept override;
    void Retire(BufferViewHandle handle) noexcept override;
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
    void SetBufferView(uint32 slot, BufferViewHandle view) noexcept override;
    Result<void> SetDrawConstants(Span<const uint8> bytes) noexcept override;
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
    // --- GX3·GX4(GlesDeviceCommands.cpp) ---
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

    // 그래픽 파이프라인의 고정 기능 상태를 GL 상태로 건다(SetPipeline).
    void ApplyPipelineState(const PipelineRecord& pipeline) noexcept;
    // 스텐실 비교 함수·기준 값을 건다(파이프라인과 SetStencilReference).
    void ApplyStencilFunction(const PipelineRecord& pipeline) noexcept;
    // 패스가 바꾼 GL 상태를 기본값으로 되돌린다(EndFrame, present blit·readback 전).
    void ResetPassState() noexcept;
    // 그리기 직전 조건(파이프라인 종류·attachment·stream·view·읽기 전용 깊이·index)을 보고
    // 상수·view를 건다. 그릴 수 없으면 null이다.
    const PipelineRecord* PrepareDraw(bool indexed) noexcept;
    // dispatch 직전 조건을 보고 상수·읽기·쓰기 view를 건다.
    const PipelineRecord* PrepareDispatch() noexcept;
    // 간접 인자 버퍼(Indirect 용도, offset 4 byte 배수, 범위 안)의 GL 이름이다. 아니면 0이다.
    GLuint IndirectArguments(BufferHandle arguments, uint64 offset, uint64 bytes) const noexcept;

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
    // 패스 끝 MSAA resolve다(EndFrame).
    void ResolveAttachments() noexcept;
    // 묶인 framebuffer(target)의 attachment에 텍스처의 (mip, slice)를 붙인다. 2D·MSAA는
    // FramebufferTexture2D, cube는 면 대상, 2D 배열·cube 배열·3D는 FramebufferTextureLayer다.
    void AttachTextureLevel(GLenum target, GLenum attachment, const TextureRecord& record,
                            uint32 mipLevel, uint32 slice) noexcept;
    // SetDrawConstants 거부(묶인 파이프라인 없음, 크기가 선언과 다름)의 InvalidArgument다.
    Result<void> RejectDrawConstantSize(bool pipelineBound, usize bytes,
                                        uint32 drawConstantBytes) noexcept;

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
    // 텍스처 기록 하나를 회수 대기로 넘기고 표에서 뺀다(view면 샘플러만 놓는다).
    void RetireTextureRecord(SlotHandle slot, const TextureRecord& record) noexcept;

    // 스왑체인의 offscreen 색 텍스처를 width × height로 (다시) 만든다.
    bool CreateBackBuffer(SwapChainRecord& record, uint32 width, uint32 height) noexcept;

    // 색 attachment 하나의 framebuffer를 만든다. 완전하지 않으면 0이다.
    GLuint CreateColorFramebuffer(GLuint texture, GLenum target = GL_TEXTURE_2D) noexcept;

    // 텍스처 하나의 샘플러 객체를 만든다. filterable이 false면(정수·깊이 형식) 최근접 필터다.
    // filterable은 형식이 선형 필터를 받는지다(비교 샘플러의 깊이 형식은 선형 필터를 받는다).
    GLuint CreateSampler(const SamplerDesc& desc, bool filterable) noexcept;

    // texture mip 하나를 올린다(업로드 유닛에서 묶는다). 원본은 Rocks 배치(mip 안 plane 순서)다.
    // 깊이·스텐실은 plane을 GL 외부 배치로 엮는 임시 버퍼가 필요하다(할당 실패면 OutOfMemory).
    // mip 하나(모든 slice·깊이 면, TextureDesc 초기 픽셀 배치)를 올린다.
    Result<void> UploadMip(const TextureRecord& record, uint32 level, const uint8* pixels) noexcept;
    // 2D 이미지 images개(같은 크기)를 GL 외부 배치로 바꿔(깊이 형식 묶기) 대상 하나에 올린다.
    Result<void> UploadImages(const TextureRecord& record, GLenum target, uint32 level,
                              uint32 firstLayer, uint32 images, const uint8* pixels) noexcept;

    // mip 하나를 색 attachment로 읽어 Rocks 배치로 out에 둔다(ReadTexture·ReadPixels·readback의
    // 공통 경로). 읽기 조합은 구현 선택 조합이 업로드 배치와 같으면 그것, 아니면 표준 조합이다.
    // layer는 배열 layer·cube 면(cube 배열은 cube × 6 + 면)·3D 깊이 면이다.
    Result<void> ReadColorLevel(const TextureRecord& record, uint32 mipLevel, uint32 layer,
                                Span<uint8> out) noexcept;
    // 깊이·스텐실 plane 하나를 셰이더로 R32F·R32UI 대상에 옮겨 읽는다(ES는 깊이·스텐실을
    // glReadPixels로 읽지 못한다).
    Result<void> ReadDepthPlane(const TextureRecord& record, uint32 mipLevel, uint32 plane,
                                Span<uint8> out) noexcept;
    // 지금 읽기 framebuffer에서 읽을 조합을 고른다. direct면 업로드 배치 그대로다.
    GlesReadPair ChooseReadPair(const GlesTextureFormat& format, bool& direct) noexcept;
    // framebuffer의 색 attachment 0을 pixel pack buffer로 읽고 map해 out에 옮긴다.
    Result<void> ReadFramebuffer(GLuint framebuffer, uint32 width, uint32 height,
                                 const GlesReadPair& pair, uint8* out, uint64 bytes) noexcept;
    // 깊이·스텐실 읽기용 프로그램·정점 배열·최근접 샘플러를 처음 쓸 때 만든다.
    Result<void> PrepareDepthReads() noexcept;

    Result<void> MatchVertexInputs(GLuint program, const VertexLayout& layout) noexcept;
    // --- 장치 능력·버퍼 view(GlesDeviceBuffers.cpp) ---
    // 장치 한도·기능과 형식별 MSAA 표본 수를 채운다(생성 때 한 번, debug output을 켜기 전).
    void QueryDeviceCaps(bool desktopContext) noexcept;
    // 링크한 프로그램의 sampler·image uniform·storage block을 고정 배치와 대조해 slot 배치를
    // 만든다. writeSlots가 null이면(그래픽) 쓰기 자원(image uniform, binding 4–7 storage block)은
    // 배치 밖이다. 배치 밖(유닛 0이 아닌 텍스처 sampler, 허용 유닛 밖 samplerBuffer·image, binding
    // 밖 storage block)이면 InvalidArgument다. GL introspection은 storage block의 readonly 여부를
    // 알려 주지 않으므로 그래픽 단계의 binding 0–3 storage block 쓰기는 거부하지 못한다(셰이더는
    // readonly로 선언해야 하고, 쿠킹 경로는 SPIR-V 리플렉션이 거른다).
    // compute의 image2D·image2DArray·image3D·imageCube·imageCubeArray(SetWritableTexture slot =
    // image unit)는 writeTextures에 더한다.
    Result<void> ReflectBufferSlots(GLuint program, DrawBufferSlots& slots,
                                    DrawBufferSlots* writeSlots,
                                    uint32* writeTextures = nullptr) noexcept;
    // 링크한 프로그램의 루트 상수 에뮬레이션 배열(vu_h·pu_h·cu_h) 위치와 active vec4 수를 record에
    // 채운다. vec4 배열이 아니거나 active 크기 × 16이 record.drawConstantBytes보다 크면
    // InvalidArgument다.
    Result<void> ReflectRootConstants(GLuint program, PipelineRecord& record) noexcept;
    bool BufferViewsBound(const PipelineRecord& pipeline) const noexcept;
    // 파이프라인이 읽는 slot의 view를 유닛·binding에 묶는다(그리기 직전).
    void ApplyBufferViews(const PipelineRecord& pipeline) noexcept;
    void ReleaseViewSource(const BufferViewRecord& view) noexcept;
    Result<GLuint> CompileShader(GLenum stage, StringView source) noexcept;
    void StoreLog(GLuint object, bool shader) noexcept;

    void InitializeTimestamps() noexcept;
    // 기록을 시작하고 pass_의 framebuffer에 load 동작(clear)을 한다(pass_는 호출자가 채운다).
    void StartPass(const PassDesc& pass) noexcept;
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
    SlotArray<BufferViewRecord> bufferViews_;
    Array<OrphanBuffer> orphanBuffers_;
    // 장치 능력(GX1)과 형식별 MSAA 표본 수다(생성 때 정해진다).
    DeviceCaps caps_;
    uint32 sampleCounts_[MaxPixelFormatNumber + 1] = {};
    uint32 apiVersion_ = 0;
    GlesStagingSource stagingSource_;
    UniquePtr<StagingPool> staging_;
    UniquePtr<GlesPlatform> platform_;
    // platform_->Gl()의 사본이다(호출마다 간접 참조를 줄인다).
    GlesFunctions gl_;
    GlesFeatures features_;
    // 형식 번호마다의 능력이다(장치 생성 때 features_로 한 번 채운다).
    PixelFormatCaps formatCaps_[MaxPixelFormatNumber + 1] = {};
    // 깊이·스텐실 plane 읽기 자원이다(PrepareDepthReads).
    GLuint depthReadProgram_ = 0;
    GLuint stencilReadProgram_ = 0;
    GLint depthReadLevel_ = -1;
    GLint stencilReadLevel_ = -1;
    GLuint readVertexArray_ = 0;
    GLuint readSampler_ = 0;
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
        // BeginPass의 attachment들이다(GX4). 색 1개·깊이 없음이면 텍스처의 framebuffer, 아니면
        // 장치의 패스 framebuffer(passFramebuffer_)에 붙인다.
        uint32 colorCount = 0;
        TextureHandle colors[MaxColorTargets];
        TextureHandle depth;
        bool depthReadOnly = false;
        // 색 attachment마다 패스 끝에 resolve할 대상이다(ColorAttachment::resolveTarget).
        TextureHandle resolves[MaxColorTargets];
        // attachment 표본 수다(1이면 MSAA가 아니다).
        uint32 sampleCount = 1;
        // 색 attachment 0의 mip과 resolve 대상의 mip·slice다(텍스처 GX2 C부).
        uint32 firstColorMip = 0;
        uint32 resolveMips[MaxColorTargets] = {};
        uint32 resolveSlices[MaxColorTargets] = {};
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
    BufferViewHandle boundViews_[MaxDrawBufferViews];
    // compute 쓰기 slot에 묶인 view와 스텐실 기준 값이다(GX4).
    BufferViewHandle boundWriteViews_[MaxDrawBufferViews];
    // compute 쓰기 slot에 묶인 텍스처 쓰기 view다(버퍼 view와 같은 slot 공간, 하나만 남는다).
    TextureHandle boundWriteTextures_[MaxDrawBufferViews];
    uint32 stencilReference_ = 0;
    // MRT·깊이 패스가 attachment를 붙이는 framebuffer다(처음 쓸 때 만든다).
    GLuint passFramebuffer_ = 0;
    // resolve 대상의 mip·slice를 붙이는 framebuffer다(처음 쓸 때 만든다).
    GLuint resolveFramebuffer_ = 0;
    // glPolygonOffsetClampEXT다(GL_EXT_polygon_offset_clamp가 없으면 null).
    void (*polygonOffsetClamp_)(GLfloat, GLfloat, GLfloat) = nullptr;
    // 기록의 그리기별 상수 값이다(기록 시작 때 0, 그리기·dispatch 때 program에 올린다).
    alignas(16) uint8 constants_[MaxDrawConstantBytes] = {};
    uint64 ignoredDraws_ = 0;
    uint64 validationErrors_ = 0;
    // 마지막으로 제출한 fence 값이다.
    uint64 fenceValue_ = 0;
    FenceTimeline timeline_;
    DeviceOwnership ownership_;
    // 기록 중 열린 분리 전이 짝이다.
    SplitTransitionTracker splits_;
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
