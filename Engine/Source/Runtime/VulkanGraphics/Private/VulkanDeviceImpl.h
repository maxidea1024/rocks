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
#include <Graphics/GraphicsLog.h>
#include <Graphics/ResourceAccess.h>
#include <Graphics/ShaderSourceCompiler.h>
#include <Graphics/SplitTransitionTracker.h>
#include <Graphics/SubresourceStates.h>
#include <Graphics/StagingPool.h>
#include <Graphics/TimestampRing.h>
#include <Graphics/UploadRing.h>

// Vulkan 장치 구현의 선언이다(Docs/Design/VulkanGraphics.ko.md). 구현은 기능별 파일에 나눈다.
// - VulkanDevice.cpp: 생성(instance·물리 장치·장치·queue), 정보, fence API, 소유권, 진단
// - VulkanSubmission.cpp: 제출 fence·명령 버퍼 기록·완료 관찰·회수
// - VulkanResources.cpp: 버퍼·텍스처·샘플러·Retire
// - VulkanBindings.cpp: 장치 능력(GX1), 버퍼 view·ReadBuffer(GX2), 그리기 descriptor set
// - VulkanPipeline.cpp: render pass·파이프라인 layout·파이프라인
// - VulkanCommands.cpp: 프레임 기록(CommandContext)·EndFrame
// - VulkanPasses.cpp: 패스 attachment(BeginPass)·render pass 열쇠·framebuffer 캐시(GX4)
// - VulkanCompute.cpp: compute 파이프라인·dispatch·간접 명령·복사·UAV clear·버퍼 쓰기
// barrier(GX3·GX4)
// - VulkanSwapChain.cpp: surface·swapchain·present
// - VulkanUpload.cpp: 업로드 ring·staging·readback
// - VulkanTiming.cpp: timestamp·calibration·debug label·검증 messenger
namespace rocks::graphics::vulkan {

// 명령 버퍼 기록 수 상한이다. 끝난 제출의 것을 다시 쓰고, 모두 GPU가 쓰는 중이면 이 수까지 새로
// 만든다(D3D12 기록 저장소와 같은 상한). 상한이면 소유 스레드는 가장 오래된 제출을 기다린다.
inline constexpr uint32 MaxCommandBuffers = 8;
// instance가 요청하는 Vulkan 판이다(Docs/Design/VulkanGraphics.ko.md 1절, 기준 1.1).
inline constexpr uint32 RequestedApiVersion = VK_API_VERSION_1_1;
// 제출 fence 칸 수다. 비행 중 제출(명령 버퍼 상한 + 빈 신호)과 관찰 스레드가 붙잡은 것을 덮는다.
inline constexpr uint32 MaxSubmissionFences = 64;
inline constexpr uint32 MaxMipLevels = 16;
inline constexpr uint32 MaxTextureExtent = 16384;
// 서로 다른 SamplerDesc 조합 수의 상한이다(D3D12와 같다).
inline constexpr uint32 SamplerCapacity = 256;
// 그리기 descriptor set 첫 pool의 set 수와 기록(명령 버퍼) 하나가 쓰는 pool 수 상한이다. 다음
// pool은 앞 pool의 두 배라 한 프레임의 상한은 DescriptorSetsPerPool ×
// (2^MaxDescriptorPoolsPerRecord
// - 1)이다. 넘으면 그 뒤 그리기는 무시되고(IgnoredDraws) 처음 한 번 경고 로그를 남긴다.
inline constexpr uint32 DescriptorSetsPerPool = 1024;
inline constexpr uint32 MaxDescriptorPoolsPerRecord = 8;
// 기록 하나가 쓰는 분리 전이 event 수 상한이다(넘으면 그 짝은 end에서 pipeline barrier로 낮춘다).
inline constexpr uint32 MaxSplitEventsPerRecord = 64;
// 파이프라인 하나의 descriptor binding 수 상한이다(PS 텍스처·샘플러 2 + 단계마다 버퍼 view 4,
// compute는 읽기 4 + 쓰기 4).
inline constexpr uint32 MaxPipelineBindings = 2 + 2 * MaxDrawBufferViews;
// framebuffer 하나의 attachment 수 상한이다(색 MaxColorTargets + 깊이 1).
inline constexpr uint32 MaxPassAttachments = MaxColorTargets + 1;
// 그리기별 상수 push constant 범위의 단계다(모든 파이프라인 layout이 같은 범위를 둬 서로 호환된다).
inline constexpr VkShaderStageFlags ConstantStages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
inline constexpr uint32 RequestedSwapChainImages = 3;
inline constexpr uint32 MaxSwapChainImages = 8;
// 업로드 ring 안 복사 원본 정렬이다(texel 크기 4와 버퍼 복사 모두 만족).
inline constexpr uint64 UploadAlignment = 16;
// 정점 버퍼 offset 정렬(속성 성분 크기의 최댓값)이다.
inline constexpr uint64 VertexOffsetAlignment = 4;
// 그리기별 상수 push constant 범위다. 파이프라인 크기와 무관하게 계약 최대를 둬 모든 layout의
// 범위가 같다(파이프라인을 바꿔도 descriptor set·상수가 유지된다, RenderBase 4.4절 GX4 SX2 구현).
inline constexpr uint32 ConstantBytes = MaxDrawConstantBytes;
// 소멸 때까지 놓지 않을 회수 항목의 fence 값이다.
inline constexpr uint64 RetainUntilDestructionFence = NumericLimits<uint64>::Max();
// 한 업로드 묶음 안에서 겹침을 추적하는 버퍼 쓰기 구간 수다(넘치면 barrier를 두고 비운다).
inline constexpr uint32 MaxTrackedBufferWrites = 64;

// 접근 상태의 대표형이다. D3D12 상태 대응에서 같은 상태가 되는 접근을 하나로 모은다:
// SrvGraphics는 compute 읽기도 포함하고(D3D12 PIXEL | NON_PIXEL_SHADER_RESOURCE), VertexOrIndex는
// Constant를, UAV 비트 하나는 두 UAV 비트를 포함한다. 추적 상태는 늘 대표형이고, before 검증도
// 대표형끼리 비교한다(D3D12가 대응한 상태끼리 비교하는 것과 같은 기준).
ResourceAccess CanonicalAccess(ResourceAccess access) noexcept;

struct BufferRecord {
    VkBuffer buffer = VK_NULL_HANDLE;
    MemoryAllocation memory;
    uint64 size = 0;
    BufferDesc desc;
    // 이 버퍼를 가리키는 살아 있는 view 수다. 0이 아닐 때 Retire하면 버퍼·메모리는 orphanBuffers_로
    // 옮겨 마지막 view를 놓을 때 회수한다(view가 버퍼를 붙든다).
    uint32 viewCount = 0;
};

// handle이 Retire됐지만 view가 아직 쓰는 버퍼다.
struct OrphanBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    MemoryAllocation memory;
    uint32 viewCount = 0;
};

// 버퍼 view(GX2)다. typed는 VkBufferView, 구조화·raw는 (buffer, offset, range)를 descriptor에 쓴다.
struct BufferViewRecord {
    VkBufferView view = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    // 원 버퍼 handle이다(Retire 뒤에는 stale이고 그때는 orphanBuffers_에서 buffer로 찾는다).
    SlotHandle source;
    uint64 offset = 0;
    uint64 range = 0;
    PixelFormat format = PixelFormat::Unknown;
    BufferViewKind kind = BufferViewKind::Typed;
    ViewAccess access = ViewAccess::Read;
    PixelShaderType shaderType = PixelShaderType::None;
    // 구조화 view의 원소 byte다(셰이더 선언 stride와 대조한다).
    uint32 stride = 0;
};

struct TextureRecord {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    // attachment용 view(이미지 전체 aspect, 항등 성분 대응)다. 기본 view는 깊이 aspect만 읽거나
    // swizzle이 걸려 있을 수 있으므로 스텐실 plane이 있는 깊이 타깃과 성분 대응이 항등이 아닌
    // 렌더 타깃에만 따로 만든다. 없으면 view를 attachment로 쓴다.
    VkImageView attachmentView = VK_NULL_HANDLE;
    MemoryAllocation memory;
    TextureDimension dimension = TextureDimension::Texture2D;
    uint32 width = 0;
    uint32 height = 0;
    // 3D 깊이다(다른 차원은 1).
    uint32 depth = 1;
    uint32 mipLevels = 1;
    // subresource slice 수(TextureSliceCount)이며 이미지 array layer 수(3D는 1)와 같다.
    uint32 slices = 1;
    // view(CreateTextureView)면 view가 읽는 형식, 아니면 텍스처 형식이다.
    PixelFormat format = PixelFormat::Unknown;
    bool renderTarget = false;
    bool depthTarget = false;
    // 만든 용도다(view는 원 텍스처의 용도, ResolveTextureViewRange).
    TextureUsage usage = TextureUsage::Sampled;
    // MSAA 표본 수다(TextureDesc::sampleCount, view는 원 텍스처의 값).
    uint32 sampleCount = 1;
    // view가 가리키는 mip·slice 범위다(view가 아니면 전체). 묶기 전이는 이 범위만 바꾼다.
    ResolvedSubresourceRange viewRange;
    // 쓰기 view(ViewAccess::ReadWrite)다. SetWritableTexture로만 묶는다(storage image, GENERAL).
    bool writable = false;
    // 이미지 전체의 aspect다(barrier, TextureFormatInfo::aspect).
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    // 묶을 때 쓰는 샘플러다(장치 수명 동안 산다). view도 원 텍스처의 샘플러를 쓴다.
    VkSampler sampler = VK_NULL_HANDLE;
    // 샘플러가 깊이 비교 샘플러다(view는 깊이 plane만 읽을 수 있다).
    bool comparison = false;
    // 다른 텍스처의 이미지를 공유하는 view다. 이미지·메모리는 원 텍스처 기록이 쥐고 상태도 원
    // 기록(source)이 추적한다. 업로드·readback·그리기 대상이 될 수 없다.
    bool isView = false;
    SlotHandle source;
    // 이 텍스처를 가리키는 살아 있는 view 수다. 0이 아니면 Retire는 기록을 남겨 두고(retirePending)
    // 마지막 view를 놓을 때 함께 놓는다(상태 추적의 주인을 하나로 유지한다).
    uint32 viewCount = 0;
    bool retirePending = false;
    // subresource(mip × layer)마다 장치가 추적한 접근 상태다(RenderBase GX5). committed는 제출한
    // 프레임 기록 끝의 상태, current는 기록 중인 프레임 안의 상태다(기록 중이 아니면 같다). 업로드
    // 기록은 프레임 기록보다 먼저 실행되므로 committed에서 바꿨다가 되돌린다(상태 중립). layout은
    // 상태에서 정한다(LayoutFor). view 기록은 비어 있다(원 기록이 추적한다).
    SubresourceStates<ResourceAccess> committed;
    SubresourceStates<ResourceAccess> current;
    // 기록 중인 프레임이 current를 바꿨다(touchedTextures_에 있다).
    bool touched = false;
};

// mip level의 깊이 면 수다(3D만 1보다 크다).
inline uint32 MipDepth(const TextureRecord& record, uint32 level) noexcept {
    return record.dimension == TextureDimension::Texture3D ? MipExtent(record.depth, level) : 1u;
}

// mip level 하나의 데이터 byte다(모든 layer·깊이 면, TextureMipBytes와 같은 값).
inline uint64 MipBytes(const TextureRecord& record, uint32 level) noexcept {
    return PixelFormatImageBytes(record.format, MipExtent(record.width, level),
                                 MipExtent(record.height, level)) *
           MipDepth(record, level) * record.slices;
}

// 파이프라인 descriptor binding 하나가 읽는 것이다(PipelineDesc 고정 배치).
enum class BindingSource : uint8 {
    // SetTexture의 이미지 view(t0)
    Texture,
    // SetTexture 텍스처의 샘플러(s0)
    Sampler,
    // SetBufferView slot(t1–t4)
    BufferView,
    // SetWritableBufferView slot(compute u0–u3)
    WritableBufferView,
    // SetWritableTexture slot(compute u0–u3의 storage image)
    WritableTexture
};

struct PipelineBinding {
    uint32 set = 0;
    uint32 binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    BindingSource source = BindingSource::Texture;
    uint32 slot = 0;
};

// 파이프라인과 그 layout이다. set 0(VS, compute면 CS)·1(PS) layout은 셰이더 리플렉션에서 만들고
// (binding이 없는 단계는 장치 공용 빈 layout), push constant 범위는 모든 파이프라인이 같다(공용
// layout으로 올린 상수가 이 layout과 호환된다).
struct PipelineRecord {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    // 이 파이프라인이 만든 set layout이다(빈 layout을 쓰는 단계는 null).
    VkDescriptorSetLayout setLayouts[2] = {};
    PipelineBinding bindings[MaxPipelineBindings] = {};
    uint32 bindingCount = 0;
    // compute가 텍스처(storage image)로 쓰는 쓰기 slot 비트다(SetWritableTexture가 묶여야 한다).
    uint32 writeTextures = 0;
    // binding이 있는 set 비트다.
    uint32 setMask = 0;
    DrawBufferSlots bufferSlots;
    // compute가 쓰는 slot이다.
    DrawBufferSlots writeSlots;
    uint32 strides[MaxVertexStreams] = {};
    uint32 streamMask = 0;
    // 선언한 그리기별 상수 byte 수다(SetDrawConstants 크기 검사, BuildPipelineLayout 대조).
    uint32 drawConstantBytes = 0;
    bool compute = false;
    // 깊이·스텐실을 쓴다(읽기 전용 깊이 attachment에서 그리기를 거른다).
    bool writesDepthStencil = false;
};

// render pass 하나를 가르는 값이다. 형식·표본 수가 같으면 load·store·layout만 다른 render pass끼리
// 호환이다(사양 "Render Pass Compatibility").
struct RenderPassKey {
    uint32 colorCount = 0;
    VkFormat colors[MaxColorTargets] = {};
    VkAttachmentLoadOp colorLoad[MaxColorTargets] = {};
    VkAttachmentStoreOp colorStore[MaxColorTargets] = {};
    VkFormat depth = VK_FORMAT_UNDEFINED;
    VkAttachmentLoadOp depthLoad = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkAttachmentStoreOp depthStore = VK_ATTACHMENT_STORE_OP_STORE;
    VkAttachmentLoadOp stencilLoad = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkAttachmentStoreOp stencilStore = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    bool depthReadOnly = false;
    // 모든 attachment의 표본 수다(render pass 호환 조건).
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

bool SameRenderPassKey(const RenderPassKey& left, const RenderPassKey& right) noexcept;

// attachment view 묶음 하나의 framebuffer다(BeginPass가 만들고 attachment 텍스처를 놓을 때 회수).
struct FramebufferEntry {
    VkImageView views[MaxPassAttachments] = {};
    uint32 count = 0;
    uint32 width = 0;
    uint32 height = 0;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
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
    VkImageView attachmentView = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayouts[2] = {};
    VkBufferView bufferView = VK_NULL_HANDLE;
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
    // 이 기록의 그리기 descriptor set pool이다. 기록을 다시 쓸 때(앞 제출의 fence가 끝났다) 모두
    // reset한다. poolCursor는 지금 할당하는 pool이다.
    VkDescriptorPool pools[MaxDescriptorPoolsPerRecord] = {};
    uint32 poolCount = 0;
    uint32 poolCursor = 0;
    // 분리 전이 event다(VK_KHR_synchronization2). 기록 끝에 reset하고, 기록을 다시 쓸 때(앞 제출의
    // fence가 끝났다) eventCursor를 0으로 돌린다. eventCount개를 만들어 두었다.
    VkEvent events[MaxSplitEventsPerRecord] = {};
    uint32 eventCount = 0;
    uint32 eventCursor = 0;
};

// begin한 분리 전이 하나다(synchronization2 경로). 저장한 barrier로 end의 vkCmdWaitEvents2가
// begin의 vkCmdSetEvent2와 같은 dependency를 넘긴다(VUID-vkCmdWaitEvents2-pEvents-03838).
struct PendingSplit {
    uint32 id = 0;
    // null이면 event 없이 end에서 vkCmdPipelineBarrier2로 낮춘다(event 상한).
    VkEvent event = VK_NULL_HANDLE;
    uint32 firstImage = 0;
    uint32 imageCount = 0;
    uint32 firstMemory = 0;
    uint32 memoryCount = 0;
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
    PixelFormatCaps GetPixelFormatCaps(PixelFormat format) const noexcept override;
    Result<TextureHandle> CreateTextureView(TextureHandle texture,
                                            const TextureViewDesc& desc) noexcept override;
    Result<TextureSubresources>
    GetTextureSubresources(TextureHandle texture) const noexcept override;
    Result<void> ReadTexture(TextureHandle texture, uint32 mipLevel, uint32 plane,
                             Span<uint8> out) noexcept override;
    Result<PipelineHandle> CreatePipeline(const PipelineDesc& desc) noexcept override;
    Result<PipelineHandle> CreateComputePipeline(const ComputePipelineDesc& desc) noexcept override;
    Result<CommandContext*> BeginFrame(SwapChainHandle swapChain,
                                       const ClearColor& clear) noexcept override;
    Result<CommandContext*> BeginPass(const PassDesc& pass) noexcept override;
    Result<GpuFencePoint> EndFrame() noexcept override;
    Result<void> ReadPixels(TextureHandle target, Span<uint8> out) noexcept override;
    void Retire(BufferHandle buffer) noexcept override;
    void Retire(BufferViewHandle view) noexcept override;
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
    void SetBufferView(uint32 slot, BufferViewHandle view) noexcept override;
    void Transition(Span<const ResourceTransition> transitions) noexcept override;
    // 분리 전이다(Device.h). synchronization2가 있으면 begin은 vkCmdSetEvent2, end는
    // vkCmdWaitEvents2(UE VulkanBarriers.cpp:661–663, 1405–1414), 없으면 end에서 Transition처럼
    // 낮춘다. synchronization2 경로는 before 대조와 추적 상태 갱신을 begin 때 한다(이미지 layout이
    // begin 뒤 바뀌므로 그 사이 render pass를 다시 열지 않는다).
    SplitTransition BeginTransitions(Span<const ResourceTransition> transitions) noexcept override;
    void EndTransitions(SplitTransition split,
                        Span<const ResourceTransition> transitions) noexcept override;
    Result<void> SetDrawConstants(Span<const uint8> bytes) noexcept override;
    void Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept override;
    void DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex) noexcept override;
    Result<void> CopyPassTarget(TextureHandle destination) noexcept override;
    Result<uint64> WriteTimestamp() noexcept override;
    void PushMarker(StringView name) noexcept override;
    void PopMarker() noexcept override;
    // --- GX3·GX4(VulkanCompute.cpp) ---
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

    // 패스 대상이다(BeginPass attachment 또는 swapchain 중간 이미지). 색 0(image·viewFormat·format·
    // texture)은 CopyPassTarget이 쓴다.
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        // 색 0 그리기 view의 형식(render pass 형식)이다.
        VkFormat viewFormat = VK_FORMAT_UNDEFINED;
        // 패스가 끝난 뒤 돌아갈 layout이다.
        VkImageLayout restLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        uint32 width = 0;
        uint32 height = 0;
        PixelFormat format = PixelFormat::Unknown;
        // offscreen 패스의 대상 텍스처다(창 패스는 null).
        TextureHandle texture;
        // 모든 attachment와 패스 시작 render pass·clear 값이다(창 패스는 색 1개).
        uint32 colorCount = 0;
        TextureHandle colors[MaxColorTargets];
        TextureHandle depth;
        bool depthReadOnly = false;
        RenderPassKey key;
        VkClearValue clears[MaxPassAttachments] = {};
        // 색 attachment마다 패스 끝에 resolve할 대상이다(ColorAttachment::resolveTarget).
        TextureHandle resolves[MaxColorTargets];
        // attachment 표본 수다(1이면 MSAA가 아니다).
        uint32 sampleCount = 1;
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

    // render pass 캐시 칸이다.
    struct RenderPassEntry {
        RenderPassKey key;
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

    // --- VulkanResources.cpp(형식 능력) ---
    // 형식 표에 있는 형식마다 optimal tiling 기능을 물어 능력을 정한다(장치 생성 때 한 번).
    void QueryFormatCaps() noexcept;
    // format으로 만드는 이미지의 usage·flags다(QueryFormatCaps와 CreateTexture가 같은 값을 쓴다).
    VkImageUsageFlags ImageUsageFor(PixelFormat format, bool renderTarget,
                                    bool storage = false) const noexcept;
    VkImageCreateFlags ImageFlagsFor(PixelFormat format) const noexcept;
    // 그 usage·flags로 2D 이미지를 만들 수 있는지와 mip 사슬 지원을 묻는다.
    bool ImageSupported(VkFormat format, VkImageUsageFlags usage, VkImageCreateFlags flags,
                        bool& mipmaps) const noexcept;
    // 텍스처 기록 하나를 회수 대기로 넘기고 표에서 뺀다(잠금 안).
    void RetireTextureRecord(SlotHandle slot, TextureRecord& record) noexcept;

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
    Result<VkRenderPass> RenderPassFor(const RenderPassKey& key) noexcept;
    // 색 하나·깊이 없는 render pass다(swapchain 중간 이미지).
    Result<VkRenderPass> RenderPassFor(VkFormat format, bool load) noexcept;
    Result<void> MatchVertexInputs(const CompiledShader& vertex,
                                   const VertexLayout& layout) noexcept;
    // 단계들의 리플렉션을 고정 배치(PipelineDesc·ComputePipelineDesc)와 대조하고 set
    // layout·파이프라인 layout·binding 표를 record에 만든다. 단계의 push constant 블록이
    // record.drawConstantBytes보다 크면 InvalidArgument다. compute면 pixel은 null이다. 실패하면
    // 만든 객체를 파괴한다.
    Result<void> BuildPipelineLayout(const CompiledShader& first, const CompiledShader* pixel,
                                     bool compute, PipelineRecord& record) noexcept;
    void DestroyPipelineLayout(PipelineRecord& record) noexcept;
    Result<void> Reject(const char* message) noexcept;
    // 패스 끝 MSAA resolve다(EndFrame, render pass를 닫은 뒤, 잠금 안).
    void ResolveAttachments() noexcept;
    // SetDrawConstants 거부(묶인 파이프라인 없음, 크기가 선언과 다름)의 InvalidArgument다.
    Result<void> RejectDrawConstantSize(bool pipelineBound, usize bytes,
                                        uint32 drawConstantBytes) noexcept;
    Result<VkShaderModule> CreateShaderModule(const CompiledShader& shader) noexcept;

    // --- VulkanCommands.cpp ---
    Result<void> BeginRecording() noexcept;
    bool StreamsBound() const noexcept;

    // --- VulkanPasses.cpp ---
    // 패스를 연다: 대상 attachment 전이, render pass(시작 load), viewport·scissor·스텐실 기준·상수.
    void StartPass() noexcept;
    // render pass를 연다. resume이면 모든 attachment를 LOAD로 다시 연다(전이·dispatch·복사 뒤).
    void BeginRenderPass(bool resume) noexcept;
    // attachment view 묶음의 framebuffer를 캐시에서 찾고 없으면 만든다.
    Result<VkFramebuffer> FramebufferFor(VkRenderPass renderPass, const VkImageView* views,
                                         uint32 count, uint32 width, uint32 height) noexcept;
    // view를 쓰는 framebuffer를 캐시에서 빼서 회수한다(텍스처를 놓을 때).
    void RetireFramebuffers(VkImageView view) noexcept;
    // 패스 attachment가 모두 attachment 상태인지다(다시 열 수 있는지).
    bool AttachmentsReady() const noexcept;
    // 텍스처가 지금 패스의 attachment인지다.
    bool IsPassAttachment(SlotHandle slot) const noexcept;

    // --- VulkanCompute.cpp ---
    // 그리기 직전 조건(파이프라인 종류·attachment·stream·view·읽기 전용 깊이)을 보고 descriptor를
    // 묶는다.
    bool PrepareDraw(bool indexed) noexcept;
    // dispatch 직전 조건을 보고 render pass를 닫고 descriptor를 묶는다.
    bool PrepareDispatch() noexcept;
    // 버퍼를 쓰는 명령(dispatch·복사·fill) 앞뒤의 전역 메모리 barrier다. 앞은 앞선 읽기와의 실행
    // 의존(WAR), 뒤는 다음 모든 읽기·쓰기가 결과를 보게 한다(RAW·WAW). 렌더 그래프 밖 기록의 보수적
    // 순서다.
    void BeforeBufferWrite(VkPipelineStageFlags writeStage) noexcept;
    void AfterBufferWrite(VkPipelineStageFlags writeStage) noexcept;
    VkBuffer IndirectArguments(BufferHandle arguments, uint64 offset, uint64 bytes) noexcept;
    void Barrier(VkCommandBuffer buffer, VkImage image, VkImageLayout from, VkImageLayout to,
                 uint32 baseMip = 0, uint32 mipCount = VK_REMAINING_MIP_LEVELS,
                 VkPipelineStageFlags extraSourceStages = 0) const noexcept;
    void ResetTimestampSlots() noexcept;

    // --- VulkanAccess.cpp(GX5 명시 접근 상태) ---
    // image mip 구간의 접근 상태 from → to barrier를 buffer에 기록한다. 같은 상태의 읽기는
    // 건너뛰고, uavOrder면 같은 UAV 상태 사이에도 쓰기 순서 barrier를 낸다. 기록했는지를 돌려준다.
    // layer 구간 [firstLayer, firstLayer + layerCount)도 함께 받는다.
    bool AccessBarrier(VkCommandBuffer buffer, VkImage image, uint32 firstMip, uint32 mipCount,
                       ResourceAccess from, ResourceAccess to, bool uavOrder,
                       VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, uint32 firstLayer = 0,
                       uint32 layerCount = 1) noexcept;
    // states의 mip 하나에서 같은 상태가 이어지는 layer 구간마다 from(그 상태) → to barrier를 낸다
    // (업로드·readback 기록이 mip 하나의 모든 layer를 옮길 때). toResting이면 거꾸로 to → 그
    // 상태다.
    void MipLayerBarriers(VkCommandBuffer buffer, const TextureRecord& record,
                          const SubresourceStates<ResourceAccess>& states, uint32 mipLevel,
                          ResourceAccess to, bool toResting) noexcept;
    // 기록 중 프레임에서 텍스처 subresource 범위(mip × layer)를 after로 바꾼다(잠금 안). barrier가
    // 필요하면 열린 render pass를 먼저 닫는다(호출자가 ResumeRenderPass로 다시 연다).
    void TrackTransition(TextureRecord& record, SlotHandle slot,
                         const ResolvedSubresourceRange& range, ResourceAccess after,
                         bool uavOrder) noexcept;
    // 범위를 (같은 상태 열의 layer 구간) × (같은 상태의 mip 구간) 직사각형으로 나눠 emit(firstMip,
    // mipCount, firstLayer, layerCount, from)을 부른다(barrier 수를 줄인다).
    template <class Emit>
    void ForEachStateRun(const TextureRecord& record, const ResolvedSubresourceRange& range,
                         Emit&& emit) noexcept;
    // 프레임 기록을 끝낸다. 제출했으면 current를 committed로, 아니면 committed로 되돌린다.
    void SettleTracking(bool executed) noexcept;
    void SuspendRenderPass() noexcept;
    // Transition의 잠금 안 본체다.
    void TransitionLocked(Span<const ResourceTransition> transitions) noexcept;
    // 텍스처 전이 하나를 해석한다: view면 원 텍스처, 범위, usage 검사, before 대조(검증). 무시할
    // 전이(모르는 handle·범위 밖·usage 밖·Discard)면 null이다(무시 계수는 이 함수가 올린다).
    TextureRecord* ResolveTextureTransition(const ResourceTransition& transition, SlotHandle& slot,
                                            ResolvedSubresourceRange& range) noexcept;
    // 기록 중 프레임이 상태를 바꾼 텍스처로 표시한다.
    void MarkTouched(TextureRecord& record, SlotHandle slot) noexcept;
    // 지금 기록의 분리 전이 event 하나를 준다(없으면 만든다). 상한이면 null이다.
    VkEvent AcquireSplitEvent() noexcept;
    // 기록 끝: 쓴 event를 reset하고, 끝나지 않은 짝을 센다(EndFrame, render pass 밖).
    void CloseSplitTransitions() noexcept;
    // 닫은 render pass를 LOAD로 다시 연다. 패스 대상이 더는 렌더 타깃 상태가 아니면 열지 않고 닫힌
    // 표시를 남긴다(대상이 렌더 타깃으로 돌아오는 다음 전이에서 다시 연다).
    void ResumeRenderPass() noexcept;
    void LowerBufferTransition(const ResourceTransition& transition) noexcept;

    // --- VulkanBindings.cpp ---
    // 장치 능력을 채운다(논리 장치 생성 뒤, 켠 기능 기준).
    void QueryDeviceCaps() noexcept;
    // 그리기 전에 지금 파이프라인의 descriptor set을 기록 pool에서 받아 쓰고 묶는다(바뀐 것이 있을
    // 때만). 묶을 수 없으면(버퍼 view 배치 불일치, pool 한도) 거짓이고 그리기를 무시한다.
    bool PrepareDescriptors(VkPipelineBindPoint bindPoint) noexcept;
    Result<VkDescriptorSet> AllocateFrameSet(VkDescriptorSetLayout layout) noexcept;
    // 기록을 다시 쓸 때 그 기록의 descriptor pool을 비운다.
    void ResetRecordPools(CommandRecord& record) noexcept;
    void DestroyRecordPools() noexcept;
    // view 하나가 놓일 때 원 버퍼(살아 있거나 orphan)의 view 수를 줄인다.
    void ReleaseViewSource(const BufferViewRecord& view) noexcept;

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
                           const uint64* offsets, bool initial) noexcept;
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
    // 형식 번호별 능력이다(장치 생성 때 QueryFormatCaps가 채우고 바뀌지 않는다, PixelFormat.ko.md
    // 7·8절). 형식 표(VulkanFormats.cpp)에 없는 형식은 None이다.
    PixelFormatCaps formatCaps_[MaxPixelFormatNumber + 1] = {};
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32 queueFamily_ = 0;
    bool surfaceExtension_ = false;
    bool swapchainExtension_ = false;
    bool debugUtils_ = false;
    bool anisotropy_ = false;
    // samplerYcbcrConversion 기능을 켰다(multi-planar 비디오 이미지 생성 조건, Vulkan 1.1 core).
    bool ycbcr_ = false;
    // VK_EXT_texture_compression_astc_hdr와 그 기능을 켰다(ASTC HDR 형식 사용 조건).
    bool astcHdr_ = false;
    // shaderImageGatherExtended를 지원한다(PixelFormatCaps::Gather 판정, UE와 같다).
    bool gather_ = false;
    // 켠 장치 기능·확장이다(DeviceCaps 공개, GX1).
    VkPhysicalDeviceFeatures enabledFeatures_ = {};
    bool viewportLayer_ = false;
    bool atomic64_ = false;
    // VK_KHR_synchronization2와 그 기능을 켰고 함수가 있다(분리 전이 event 경로).
    bool sync2_ = false;
    bool rayTracingPipeline_ = false;
    bool rayQuery_ = false;
    // maxMemoryAllocationSize(1.1 maintenance3)다.
    uint64 maxAllocationBytes_ = 0;
    DeviceCaps caps_;
    // 형식 번호별 MSAA 표본 수 집합이다(VkSampleCountFlags와 같은 비트).
    uint32 sampleCounts_[MaxPixelFormatNumber + 1] = {};
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
    SlotArray<BufferViewRecord> bufferViews_;
    Array<OrphanBuffer> orphanBuffers_;
    Array<Retired> retired_;
    SamplerEntry samplers_[SamplerCapacity];
    uint32 samplerCount_ = 0;
    Array<RenderPassEntry> renderPasses_;
    Array<FramebufferEntry> framebuffers_;
    VkDescriptorSetLayout emptySetLayout_ = VK_NULL_HANDLE;
    // 공용 layout(두 set 모두 빈 layout + push constant)이다. 그리기 상수를 올릴 때 쓴다.
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
    // 그리기 descriptor 입력(SetTexture·SetBufferView)과 다시 써야 하는지다.
    TextureHandle boundTexture_;
    BufferViewHandle boundViews_[MaxDrawBufferViews];
    BufferViewHandle boundWriteViews_[MaxDrawBufferViews];
    // compute 쓰기 slot에 묶인 텍스처 쓰기 view다(버퍼 view와 같은 slot 공간, 하나만 남는다).
    TextureHandle boundWriteTextures_[MaxDrawBufferViews];
    bool descriptorsDirty_ = true;
    // 프레임 descriptor pool이 바닥난 횟수다(첫 번째만 경고 로그).
    uint64 descriptorExhaustions_ = 0;
    uint64 ignoredDraws_ = 0;
    uint32 markerDepth_ = 0;
    // GX5: 기록 중 프레임이 상태를 바꾼 텍스처와, 전이를 위해 닫은 render pass다.
    Array<SlotHandle> touchedTextures_;
    bool touchedOverflow_ = false;
    bool renderPassSuspended_ = false;
    // 분리 전이: 짝 표, begin한 짝과 그 barrier(기록 끝에 비운다).
    SplitTransitionTracker splits_;
    Array<PendingSplit> pendingSplits_;
    Array<VkImageMemoryBarrier2> splitImageBarriers_;
    Array<VkMemoryBarrier2> splitMemoryBarriers_;
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
