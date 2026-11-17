#pragma once

#include "VulkanApi.h"
#include "VulkanMemory.h"

#include <Core/Containers/Array.h>
#include <Core/Containers/SlotArray.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Text/String.h>
#include <Graphics/Device.h>
#include <Graphics/DeviceOwnership.h>
#include <Graphics/FenceTimeline.h>
#include <Graphics/ShaderSourceCompiler.h>
#include <Graphics/StagingPool.h>
#include <Graphics/TimestampRing.h>
#include <Graphics/UploadRing.h>

// Vulkan 장치 구현의 선언이다(Docs/Design/VulkanGraphics.ko.md). 구현은 기능별 파일에 나눈다.
// - VulkanDevice.cpp: 생성(instance·물리 장치·장치·queue), 정보, fence API, 소유권, 진단
// - VulkanSubmission.cpp: 제출 fence·명령 버퍼 기록·완료 관찰·회수
// - VulkanResources.cpp: 버퍼·텍스처·샘플러·descriptor set·Retire
// - VulkanPipeline.cpp: render pass·파이프라인 layout·파이프라인
// - VulkanCommands.cpp: 프레임 기록(CommandContext)·EndFrame
// - VulkanSwapChain.cpp: surface·swapchain·present
// - VulkanUpload.cpp: 업로드 ring·staging·readback
// - VulkanTiming.cpp: timestamp·calibration·debug label·검증 messenger
namespace rocks::graphics::vulkan {

// 로그 범주다(LoggingSystem 13절). 검증 메시지는 따로 둔다.
inline constexpr LogCategory LogVulkan{"Graphics.Vulkan"};
inline constexpr LogCategory LogVulkanValidation{"Graphics.Vulkan.Validation"};

// 명령 버퍼 기록 수 상한이다. 끝난 제출의 것을 다시 쓰고, 모두 GPU가 쓰는 중이면 이 수까지 새로
// 만든다(D3D12 기록 저장소와 같은 상한). 상한이면 소유 스레드는 가장 오래된 제출을 기다린다.
inline constexpr uint32 MaxCommandBuffers = 8;
// 제출 fence 칸 수다. 비행 중 제출(명령 버퍼 상한 + 빈 신호)과 관찰 스레드가 붙잡은 것을 덮는다.
inline constexpr uint32 MaxSubmissionFences = 64;
inline constexpr uint32 MaxMipLevels = 16;
inline constexpr uint32 MaxTextureExtent = 16384;
// 서로 다른 SamplerDesc 조합 수의 상한이다(D3D12와 같다).
inline constexpr uint32 SamplerCapacity = 256;
inline constexpr uint32 DescriptorSetsPerPool = 256;
inline constexpr uint32 RequestedSwapChainImages = 3;
inline constexpr uint32 MaxSwapChainImages = 8;
// 업로드 ring 안 복사 원본 정렬이다(texel 크기 4와 버퍼 복사 모두 만족).
inline constexpr uint64 UploadAlignment = 16;
// 정점 버퍼 offset 정렬(속성 성분 크기의 최댓값)이다.
inline constexpr uint64 VertexOffsetAlignment = 4;
inline constexpr uint32 ConstantBytes = sizeof(DrawConstants);
// 소멸 때까지 놓지 않을 회수 항목의 fence 값이다.
inline constexpr uint64 RetainUntilDestructionFence = NumericLimits<uint64>::Max();
// 한 업로드 묶음 안에서 겹침을 추적하는 버퍼 쓰기 구간 수다(넘치면 barrier를 두고 비운다).
inline constexpr uint32 MaxTrackedBufferWrites = 64;
// 검증 메시지 ID 중복 제거 표 크기다(넘친 ID는 매번 로그를 남긴다).
inline constexpr uint32 MaxLoggedValidationIds = 128;

struct BufferRecord {
    VkBuffer buffer = VK_NULL_HANDLE;
    MemoryAllocation memory;
    uint64 size = 0;
};

struct TextureRecord {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    MemoryAllocation memory;
    // RenderTarget의 framebuffer(CLEAR·LOAD render pass 모두 호환)다.
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    // set 1(PS): binding 0 이 텍스처, binding 1 샘플러다. pool은 descriptor pool 번호다.
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32 pool = 0;
    uint32 width = 0;
    uint32 height = 0;
    uint32 mipLevels = 1;
    PixelFormat format = PixelFormat::Unknown;
    bool renderTarget = false;
};

struct PipelineRecord {
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32 strides[MaxVertexStreams] = {};
    uint32 streamMask = 0;
};

// 창 출력 하나다. 패스는 중간 이미지(target)에 그리고 EndFrame이 swapchain 이미지로 blit한다.
struct SwapChainRecord {
    NativeSurface native;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkImage images[MaxSwapChainImages] = {};
    // 이미지마다 렌더 완료 semaphore다(present가 기다린다).
    VkSemaphore renderDone[MaxSwapChainImages] = {};
    uint32 imageCount = 0;
    VkExtent2D extent = {};
    VkFormat imageFormat = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    // 중간 이미지(R8G8B8A8_UNORM, MUTABLE_FORMAT)와 그리기 view(SRGB)다.
    VkImage target = VK_NULL_HANDLE;
    VkImageView targetView = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    MemoryAllocation targetMemory;
    VkImageLayout targetLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32 width = 0;
    uint32 height = 0;
    bool vsync = true;
    // 재생성이 실패해 swapchain이 없다. 다음 Resize가 성공할 때까지 BeginFrame은 실패한다.
    bool broken = false;
    // present가 OUT_OF_DATE를 알렸다(다음 EndFrame이 acquire 전에 다시 만든다).
    bool outOfDate = false;
    bool surfaceLost = false;
};

struct ReadbackRecord {
    VkBuffer buffer = VK_NULL_HANDLE;
    MemoryAllocation memory;
    uint64 fenceValue = 0;
    uint32 width = 0;
    uint32 height = 0;
    uint32 bytesPerPixel = 0;
};

// 스트리밍 staging 하나다(StagingMemory::resource가 가리킨다).
struct StagingBlock {
    VkBuffer buffer = VK_NULL_HANDLE;
    MemoryAllocation memory;
};

// fence 뒤에 파괴할 객체 묶음이다. 아무 칸이나 비어 있을 수 있고 파괴 순서는 DestroyRetired가
// 정한다(framebuffer·view → image → 메모리, swapchain → surface).
struct Retired {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32 setPool = 0;
    MemoryAllocation memory;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSemaphore semaphores[MaxSwapChainImages] = {};
    uint32 semaphoreCount = 0;
    uint64 fenceValue = 0;
    // 아직 제출하지 않은 업로드 기록이 쓸 수 있다(묶음 제출이 fence를 정한다).
    bool uploadPending = false;
    // 기록 중인 프레임이 쓸 수 있다(EndFrame이 fence를 정한다).
    bool pending = false;
    // present가 쓰던 객체다. fence 뒤에도 present의 semaphore 대기가 남을 수 있으므로 파괴 전에
    // queue idle을 기다린다.
    bool presentIdle = false;
};

// 제출 fence 칸이다. 관찰 스레드가 token으로 붙잡는 동안(watcherHold) reset하지 않는다.
struct SubmissionFence {
    VkFence fence = VK_NULL_HANDLE;
    uint64 value = 0;
    bool inFlight = false;
    // 소유 스레드가 완료를 보았다.
    bool ownerDone = false;
    // 관찰 스레드가 아직 놓지 않았다(원자 접근).
    uint32 watcherHold = 0;
};

struct CommandRecord {
    VkCommandBuffer buffer = VK_NULL_HANDLE;
    uint64 fenceValue = 0;
    bool inUse = false;
};

// acquire semaphore와 그것을 기다린 제출의 fence다.
struct AcquireSemaphore {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    uint64 fenceValue = 0;
};

// 검증 messenger가 드라이버 스레드에서 쓰는 상태다(원자 접근만).
struct ValidationState {
    uint64 errors = 0;
    int32 loggedIds[MaxLoggedValidationIds] = {};
    uint32 loggedCount = 0;
};

class VulkanDevice;

// 관찰 스레드가 제출 fence를 기다린다.
class FenceWatcher final : public FenceWaitSource {
public:
    bool WaitSubmission(uint64 value, void* token) noexcept override;
    void ReleaseToken(void* token) noexcept override;

    const Functions* functions = nullptr;
    VkDevice device = VK_NULL_HANDLE;
};

// 스트리밍 staging(HOST_VISIBLE 버퍼, 계속 map)을 만든다. StagingPool이 자기 잠금 안에서 부르므로
// 장치 잠금을 잡지 않고 메모리 잠금(leaf)만 잡는다.
class StagingSourceImpl final : public StagingSource {
public:
    Result<StagingMemory> CreateStaging(uint64 bytes) noexcept override;
    void DestroyStaging(const StagingMemory& memory) noexcept override;

    VulkanDevice* owner = nullptr;
};

// fence 완료 뒤 query 결과를 읽는다.
class TimestampSourceImpl final : public TimestampSource {
public:
    TimestampReadState ReadTimestamps(uint32 first, uint32 count, uint64* ticks) noexcept override;

    VulkanDevice* owner = nullptr;
};

class VulkanDevice final : public Device, private CommandContext {
public:
    explicit VulkanDevice(Allocator& allocator) noexcept;
    ~VulkanDevice() override;

    Result<void> Initialize(const DeviceDesc& desc, DeviceCreateReport* report) noexcept;

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

    // --- staging·timestamp 원천이 부른다 ---
    Result<StagingMemory> CreateStagingMemory(uint64 bytes) noexcept;
    void DestroyStagingMemory(const StagingMemory& memory) noexcept;
    TimestampReadState ReadTimestampQueries(uint32 first, uint32 count, uint64* ticks) noexcept;

private:
    // --- CommandContext ---
    void SetPipeline(PipelineHandle pipeline) noexcept override;
    void SetVertexBuffers(uint32 firstStream,
                          Span<const VertexBufferBinding> buffers) noexcept override;
    void SetIndexBuffer(BufferHandle buffer, IndexFormat format, uint64 offset) noexcept override;
    void SetTexture(TextureHandle texture) noexcept override;
    void SetConstants(const DrawConstants& constants) noexcept override;
    void Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept override;
    void DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex) noexcept override;
    Result<void> CopyPassTarget(TextureHandle destination) noexcept override;
    Result<uint64> WriteTimestamp() noexcept override;
    void PushMarker(StringView name) noexcept override;
    void PopMarker() noexcept override;

    // 패스 대상이다(RenderTarget 텍스처 또는 swapchain 중간 이미지).
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        // 그리기 view의 형식(render pass 형식)이다.
        VkFormat viewFormat = VK_FORMAT_UNDEFINED;
        // 패스가 끝난 뒤 돌아갈 layout이다.
        VkImageLayout restLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        uint32 width = 0;
        uint32 height = 0;
        PixelFormat format = PixelFormat::Unknown;
        // offscreen 패스의 대상 텍스처다(창 패스는 null).
        TextureHandle texture;
    };

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
        VkSampler sampler = VK_NULL_HANDLE;
    };

    // render pass 캐시 칸이다(형식 × load op).
    struct RenderPassEntry {
        VkFormat format = VK_FORMAT_UNDEFINED;
        bool load = false;
        VkRenderPass renderPass = VK_NULL_HANDLE;
    };

    // 업로드 원본 공간이다. ring 구간이거나 전용 staging 버퍼다.
    struct UploadSpace {
        VkBuffer buffer = VK_NULL_HANDLE;
        uint64 offset = 0;
        uint8* data = nullptr;
    };

    // 한 업로드 묶음 안의 버퍼 쓰기 구간이다(WAW barrier 판정).
    struct BufferWrite {
        VkBuffer buffer = VK_NULL_HANDLE;
        uint64 begin = 0;
        uint64 end = 0;
    };

    // --- VulkanDevice.cpp ---
    Result<void> CreateInstance(const DeviceDesc& desc) noexcept;
    Result<void> PickPhysicalDevice(const DeviceDesc& desc) noexcept;
    Result<void> CreateLogicalDevice() noexcept;
    Result<void> CreateSharedObjects(const DeviceDesc& desc) noexcept;
    // 실패 진단을 LastError(와 생성 중이면 report)에 남기고 오류를 돌려준다.
    Result<void> Fail(const char* stage, VkResult result) noexcept;
    Result<void> FailCode(const char* stage, ErrorCode code, const char* detail) noexcept;
    template <class T>
    Result<T> FailWith(const char* stage, VkResult result) noexcept {
        auto failed = Fail(stage, result);
        return Result<T>::Failure(failed.Error());
    }

    // 이 backend가 대응하지 않는 픽셀 형식이다. 다른 형식으로 바꾸지 않고 Unsupported로 실패하고
    // LastError에 형식 이름을 남긴다(PixelFormat.ko.md 8.3절). 호출자는 gate_를 잡고 있어야 한다.
    template <class T>
    Result<T> UnsupportedFormat(PixelFormat format) noexcept {
        auto failed = FailCode("pixel format", ErrorCode::Unsupported, PixelFormatName(format));
        auto appended = lastError_.TryAppend(StringView(" is not supported by the Vulkan backend"));
        (void)appended;
        return Result<T>::Failure(failed.Error());
    }

    // 장치 손실이면 게시한다(기다리는 쪽을 Destroyed로 깨운다).
    void DetectLost(VkResult result) noexcept;

    // --- VulkanSubmission.cpp ---
    Result<void> CreateCommandPool() noexcept;
    Result<uint32> PickCommandBuffer() noexcept;
    // 명령 버퍼를 제출하고 fence 값을 돌려준다. buffer가 null이면 빈 제출(지금까지의 완료
    // 신호)이다.
    Result<uint64> Submit(VkCommandBuffer buffer, VkSemaphore wait, VkPipelineStageFlags waitStage,
                          VkSemaphore signal) noexcept;
    Result<uint32> AcquireFenceSlot() noexcept;
    // 비행 중 fence를 보고 완료 값을 올린다(소유 스레드). 손실을 보면 게시한다.
    void PollCompleted() noexcept;
    uint64 CompletedValue() const noexcept;
    Result<void> WaitFenceOwner(uint64 value) noexcept;
    Result<void> WaitIdleLocked() noexcept;
    void DestroySubmissionObjects() noexcept;
    void ProcessRetired() noexcept;
    void DestroyRetired(Retired& entry) noexcept;
    void AssignPendingRetires(uint64 value) noexcept;
    void AssignPendingUploads(uint64 value) noexcept;
    void RetireEntry(Retired&& entry) noexcept;
    Result<void> ReserveRetireCapacity(usize extra) noexcept;
    void KeepUntilDestruction(Retired&& entry) noexcept;

    // --- VulkanResources.cpp ---
    Result<VkSampler> SamplerFor(const SamplerDesc& desc) noexcept;
    Result<void> AllocateTextureSet(VkImageView view, VkSampler sampler, VkDescriptorSet& set,
                                    uint32& pool) noexcept;
    void FreeTextureSet(VkDescriptorSet set, uint32 pool) noexcept;
    Result<void> CreateImageWithMemory(const VkImageCreateInfo& info, VkImage& image,
                                       MemoryAllocation& memory) noexcept;
    Result<void> CreateBufferWithMemory(uint64 size, VkBufferUsageFlags usage, MemoryUsage memory,
                                        VkBuffer& buffer, MemoryAllocation& allocation) noexcept;
    MemoryAllocation AllocateMemoryLocked(const MemoryRequest& request, MemoryUsage usage,
                                          ResourceShape shape, VkResult& result,
                                          ErrorCode& error) noexcept;
    void FreeMemoryLocked(const MemoryAllocation& allocation) noexcept;

    // --- VulkanPipeline.cpp ---
    Result<void> CreatePipelineLayout() noexcept;
    Result<VkRenderPass> RenderPassFor(VkFormat format, bool load) noexcept;
    Result<void> MatchVertexInputs(const CompiledShader& vertex,
                                   const VertexLayout& layout) noexcept;
    Result<void> MatchBindings(const CompiledShader& shader, ShaderStage stage) noexcept;
    Result<void> Reject(const char* message) noexcept;
    Result<VkShaderModule> CreateShaderModule(const CompiledShader& shader) noexcept;

    // --- VulkanCommands.cpp ---
    Result<void> BeginRecording() noexcept;
    void StartPass(const ClearColor& clear) noexcept;
    void BeginRenderPass(bool load, const ClearColor* clear) noexcept;
    bool StreamsBound() const noexcept;
    void Barrier(VkCommandBuffer buffer, VkImage image, VkImageLayout from, VkImageLayout to,
                 uint32 baseMip = 0, uint32 mipCount = VK_REMAINING_MIP_LEVELS,
                 VkPipelineStageFlags extraSourceStages = 0) const noexcept;
    void ResetTimestampSlots() noexcept;

    // --- VulkanSwapChain.cpp ---
    Result<void> BuildSwapChain(SwapChainRecord& record) noexcept;
    Result<void> CreateSwapChainTarget(SwapChainRecord& record) noexcept;
    void DestroySwapChainTarget(SwapChainRecord& record) noexcept;
    void DestroySwapChainImages(SwapChainRecord& record, bool keepSwapchain) noexcept;
    Result<void> RecreateSwapChain(SwapChainRecord& record) noexcept;
    // EndFrame의 present 단계: 이미지 얻기·blit 기록. 얻지 못하면 실패를 돌려주고 기록은 하지
    // 않는다.
    Result<void> RecordPresentBlit(SwapChainRecord& record, uint32& imageIndex,
                                   VkSemaphore& acquired) noexcept;
    Result<VkSemaphore> TakeAcquireSemaphore() noexcept;
    void ReturnAcquireSemaphore(VkSemaphore semaphore, uint64 fenceValue) noexcept;

    // --- VulkanUpload.cpp ---
    Result<void> CreateUploadRing(const DeviceDesc& desc) noexcept;
    Result<void> OpenUploadList() noexcept;
    Result<UploadSpace> StageUpload(uint64 size) noexcept;
    Result<void> RecordBufferUpload(VkBuffer target, uint64 offset,
                                    Span<const uint8> data) noexcept;
    void RecordBufferCopy(VkBuffer source, uint64 sourceOffset, VkBuffer target,
                          uint64 targetOffset, uint64 size) noexcept;
    Result<void> RecordTextureUpload(const TextureRecord& record, uint32 firstMip, uint32 count,
                                     Span<const uint8> pixels, bool initial) noexcept;
    void RecordTextureCopy(const TextureRecord& record, uint32 mipLevel, VkBuffer source,
                           uint64 offset, bool initial) noexcept;
    Result<uint64> FlushUploadsLocked() noexcept;
    Result<ReadbackRequest> RequestReadbackLocked(TextureHandle target) noexcept;
    Result<bool> TryReadbackLocked(ReadbackHandle handle, Span<uint8> out) noexcept;
    void ReleaseReadbackLocked(ReadbackHandle handle) noexcept;

    // --- VulkanTiming.cpp ---
    void InitializeTimestamps(uint32 queueFamily) noexcept;
    void InitializeCalibration() noexcept;
    Result<void> CreateMessenger() noexcept;
    void HarvestTimestamps() noexcept;
    void CloseMarkers() noexcept;
    static VKAPI_ATTR VkBool32 VKAPI_CALL OnValidationMessage(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
        const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);

    friend class StagingSourceImpl;
    friend class TimestampSourceImpl;

    Allocator& allocator_;
    String adapterName_;
    String lastError_;
    // 생성 중 실패를 적을 곳이다(Initialize 동안만 유효).
    DeviceCreateReport* report_ = nullptr;
    Functions functions_;
    Loader loader_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_ = {};
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32 queueFamily_ = 0;
    bool surfaceExtension_ = false;
    bool swapchainExtension_ = false;
    bool debugUtils_ = false;
    bool anisotropy_ = false;
    bool validation_ = false;
    bool software_ = false;
    ShaderSourceCompiler* shaderCompiler_ = nullptr;

    // 메모리 할당기와 그 잠금이다. 장치 잠금·staging pool 잠금 안에서 잡는 가장 안쪽 잠금이다(이
    // 잠금을 쥔 채 다른 잠금을 잡지 않는다).
    mutable platform::Mutex memoryGate_;
    MemoryManager memory_;

    SlotArray<BufferRecord> buffers_;
    SlotArray<TextureRecord> textures_;
    SlotArray<PipelineRecord> pipelines_;
    SlotArray<SwapChainRecord> swapChains_;
    SlotArray<ReadbackRecord> readbacks_;
    Array<Retired> retired_;
    SamplerEntry samplers_[SamplerCapacity];
    uint32 samplerCount_ = 0;
    Array<VkDescriptorPool> descriptorPools_;
    Array<RenderPassEntry> renderPasses_;
    VkDescriptorSetLayout emptySetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout textureSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;

    // 제출·완료(GX7).
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    CommandRecord commands_[MaxCommandBuffers];
    uint32 commandCount_ = 0;
    SubmissionFence fences_[MaxSubmissionFences];
    uint32 fenceCount_ = 0;
    // 마지막으로 제출한 값과 소유 스레드가 본 완료 값이다.
    uint64 fenceValue_ = 0;
    uint64 completedValue_ = 0;
    FenceWatcher watcher_;
    FenceTimeline timeline_;
    DeviceOwnership ownership_;
    // 장치 내부 잠금이다. 자원 생성·갱신이 다른 스레드에서 올 수
    // 있으므로(concurrentResourceCreation) 자원 표·descriptor·명령 버퍼·업로드 기록·회수 목록을
    // 만지는 모든 호출이 잡는다. fence API는 잡지 않는다.
    mutable platform::Mutex gate_;

    // 프레임 기록.
    VkCommandBuffer frame_ = VK_NULL_HANDLE;
    uint32 frameRecord_ = 0;
    bool recording_ = false;
    bool insideRenderPass_ = false;
    Target target_;
    SwapChainHandle presentTarget_;
    TextureHandle white_;
    SlotHandle boundPipeline_;
    BoundVertexBuffer boundVertex_[MaxVertexStreams];
    BoundIndexBuffer boundIndex_;
    uint64 ignoredDraws_ = 0;
    uint32 markerDepth_ = 0;
    Array<AcquireSemaphore> acquireSemaphores_;
    // 신호됐지만 대기자가 없는 acquire semaphore다(제출 실패). queue idle 뒤 파괴한다.
    Array<VkSemaphore> orphanSemaphores_;

    // 업로드(GX8).
    UploadRingAllocator ring_;
    VkBuffer ringBuffer_ = VK_NULL_HANDLE;
    MemoryAllocation ringMemory_;
    VkCommandBuffer upload_ = VK_NULL_HANDLE;
    uint32 uploadRecord_ = 0;
    bool uploadOpen_ = false;
    uint64 dedicatedUploads_ = 0;
    BufferWrite bufferWrites_[MaxTrackedBufferWrites];
    uint32 bufferWriteCount_ = 0;
    StagingSourceImpl stagingSource_;
    UniquePtr<StagingPool> staging_;
    // staging 블록 소유 목록이다(memoryGate_ 안에서 만진다).
    Array<UniquePtr<StagingBlock>> stagingBlocks_;

    // 계측(GX9).
    VkQueryPool queryPool_ = VK_NULL_HANDLE;
    TimestampSourceImpl timestampSource_;
    TimestampRing timestamps_;
    uint64 timestampFrequency_ = 0;
    uint64 timestampMask_ = 0;
    // 초기화가 필요한 slot(bit)과 이번 기록이 초기화한·쓴 slot이다.
    uint32 dirtySlots_ = 0;
    uint32 resetThisFrame_ = 0;
    uint32 usedThisFrame_ = 0;
    PFN_vkGetCalibratedTimestampsKHR getCalibratedTimestamps_ = nullptr;

    // 검증(드라이버 스레드에서도 쓴다).
    ValidationState validationState_;
};

} // namespace rocks::graphics::vulkan
