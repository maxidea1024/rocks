#pragma once

#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <D3D12Graphics/D3D12Device.h>
#include <Graphics/DeviceOwnership.h>
#include <Graphics/FenceTimeline.h>
#include <Graphics/GpuPageAllocator.h>
#include <Graphics/GraphicsLog.h>
#include <Graphics/SplitTransitionTracker.h>
#include <Graphics/SubresourceStates.h>
#include <Graphics/StagingPool.h>
#include <Graphics/TimestampRing.h>
#include <Graphics/UploadRing.h>
#include <Platform/Fatal.h>
#include <Platform/Win64/ComRef.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

#include <Platform/Atomics.h>

#include <Graphics/RootBudget.h>
#include <Graphics/ShaderBindings.h>

#include "D3D12Caps.h"
#include "D3D12PixelFormats.h"
#include <Platform/Memory.h>

// D3D12 장치 구현의 선언이다. 정의는 기능별 파일에 나눈다.
// - D3D12Device.cpp: 생성·장치 정보·fence·소유권·회수·검증 집계·생성 함수
// - D3D12Common.cpp: 상태·형식·barrier 도우미
// - D3D12Resources.cpp: 스왑체인·버퍼·버퍼 view·텍스처·텍스처 view·샘플러·Retire
// - D3D12Pipeline.cpp: 셰이더 컴파일·리플렉션·root signature·파이프라인
// - D3D12Frame.cpp: 프레임 시작·제출과 기록(CommandContext)·명시 전이
// - D3D12Commands.cpp: 패스 attachment·compute·간접 명령·복사·UAV clear·버퍼 상태 추적(GX3·GX4)
// - D3D12Upload.cpp: 업로드 기록·ring·staging·readback
// - D3D12Timing.cpp: timestamp·calibration·marker
// - D3D12Compute.cpp: async compute queue(RG4): queue·fence·기록 저장소·제출·queue 사이 대기
// - D3D12Transient.cpp: transient heap·placed 자원·aliasing 획득(GX12)
namespace rocks::graphics::d3d12 {

// "stage failed (HRESULT 0x...)" 진단 한 줄의 버퍼 byte 수다.
constexpr usize FailureMessageBytes = 160;

// GetDeviceRemovedReason 값의 이름이다(진단용, 정적 수명).
const char* RemovedReasonName(HRESULT reason) noexcept;

// 기록 저장소(command allocator) 수의 상한이다. 끝난 제출의 저장소를 다시 쓰고, 모두 GPU가 쓰는
// 중이면 이 수까지 새로 만든다. 프레임 수 제한(프레임 slot, RenderBase 5.3절)은 장치 밖에서 하므로
// 보통은 slot 수 + 업로드 정도만 쓴다. 상한에 닿으면 가장 오래된 제출을 기다린다.
constexpr uint32 MaxCommandAllocators = 8;
constexpr uint32 SwapChainBufferCount = 2;
constexpr uint32 RtvCapacity = 128;
constexpr uint32 DsvCapacity = 64;
constexpr uint32 SrvCapacity = 1024;
// 서로 다른 SamplerDesc 조합 수의 상한이다(필터 4 × 주소 9 × 이방성 16을 넘지 않는다).
constexpr uint32 SamplerCapacity = 256;
constexpr uint32 MaxMipLevels = 16;
// 업로드 ring 안 텍스처 footprint 시작 정렬이다(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT).
constexpr uint64 TexturePlacementAlignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
// 업로드 ring 안 버퍼 복사 원본 정렬이다.
constexpr uint64 BufferUploadAlignment = 16;
// 기록 하나가 그리기 때 table을 복사해 넣는 shader-visible descriptor ring의 칸 수다(queue마다,
// RenderBase 9.11절 5항). 같은 원본 목록은 다시 복사하지 않는다.
constexpr uint32 OnlineDescriptorsPerQueue = 131'072;
// 그리기 때 Sampler table을 복사해 넣는 shader-visible sampler ring의 칸 수다(queue마다 — 두 queue
// 합이 D3D12 shader-visible sampler heap 최대 2048, RenderBase 9.13절 4항).
constexpr uint32 OnlineSamplersPerQueue = 1024;
// 단계 Sampler table 원본이 비었음을 나타내는 boundSamplers_ 값이다(텍스처 desc 샘플러를 쓴다).
constexpr uint32 NoBoundSampler = NumericLimits<uint32>::Max();
// table 캐시 종류 번호다(D3D12Device::CopyTable).
constexpr uint32 SrvTableKind = 0;
constexpr uint32 UavTableKind = 1;
constexpr uint32 SamplerTableKind = 2;
constexpr uint32 TableKindCount = 3;
// 스왑체인 하나가 파괴될 때 retire 목록에 넣는 항목 수(스왑체인 + 버퍼마다 하나)다.
constexpr usize SwapChainRetireEntries = 1 + SwapChainBufferCount;
// 소멸 때까지 놓지 않을 retire 항목의 fence 값이다. 정상 동작 중의 완료 값은 이 값에 닿지
// 않는다.
constexpr uint64 RetainUntilDestructionFence = NumericLimits<uint64>::Max();
// dxgidebug.h의 DXGI_DEBUG_DXGI다(dxguid.lib 없이 쓰려고 값을 직접 둔다).
constexpr GUID DxgiDebugDxgi = {
    0x25cddaa4, 0xb1c6, 0x47e1, {0xac, 0x3e, 0x98, 0x87, 0x5b, 0x5a, 0x2e, 0x2a}};
static_assert(MaxDrawConstantBytes % sizeof(uint32) == 0,
              "Draw constants must be a whole number of root constants");
// 루트 상수의 register space다(UE 예약 space 3과 같은 번호 — ShaderSystem SSD32).
constexpr UINT RootConstantsSpace = 3;
// PS 진단 버퍼 root UAV의 register space다(u0 space999, ShaderSystem 7.2절 12항).
constexpr UINT DebugBufferSpace = 999;

// COM 참조는 PlatformBase 공용 도구를 쓴다(Platform/Win64/ComRef.h).
using platform::windows::ComRef;

// 컬 모드의 D3D12 값이다. 앞면 규약(FrontCounterClockwise = TRUE)과 함께 쓴다.
D3D12_CULL_MODE RasterCullMode(CullMode mode) noexcept;

// 정점 원소 형식의 DXGI 형식이다(RenderBase 10.4절 표, UE D3D12VertexDeclaration.cpp:27–41).
DXGI_FORMAT AttributeFormat(VertexFormat format) noexcept;

// HLSL 시맨틱은 대소문자를 가리지 않는다. 정점 입력 시맨틱이 ATTRIBUTE인지 본다.
bool IsAttributeSemantic(const char* name) noexcept;

// 진단 문자열 조립 칸이다(셰이더 입력 대조 실패의 location·형식·셰이더 타입).
class Diagnostic {
public:
    Diagnostic& Text(const char* text) noexcept {
        while (*text != '\0' && length_ + 1 < sizeof(buffer_)) {
            buffer_[length_++] = *text++;
        }
        buffer_[length_] = '\0';
        return *this;
    }

    Diagnostic& Number(uint32 value) noexcept {
        char digits[12] = {};
        usize count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count > 0 && length_ + 1 < sizeof(buffer_)) {
            buffer_[length_++] = digits[--count];
        }
        buffer_[length_] = '\0';
        return *this;
    }

    const char* CStr() const noexcept {
        return buffer_;
    }

private:
    char buffer_[256] = {};
    usize length_ = 0;
};

// 샘플러 필터다(비교 샘플러면 COMPARISON 계열). AnisotropicPoint는
// D3D12_FILTER_MIN_MAG_ANISOTROPIC_MIP_POINT다(장치 기능 AnisoFilterWithPointMipSupported가 있을
// 때만 부른다. 없으면 SamplerFor가 AnisotropicLinear로 바꿔 부른다).
D3D12_FILTER SamplerFilter(Filter filter, bool comparison) noexcept;

D3D12_TEXTURE_ADDRESS_MODE SamplerAddress(AddressMode mode) noexcept;

D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after) noexcept;

// subresource 하나의 전이다.
D3D12_RESOURCE_BARRIER TransitionSubresource(ID3D12Resource* resource, uint32 subresource,
                                             D3D12_RESOURCE_STATES before,
                                             D3D12_RESOURCE_STATES after) noexcept;

// 추상 접근 상태를 D3D12 resource state로 옮긴다(RenderBase GX5, UE D3D12 legacy barrier의 상태
// 대응). 깊이 쓰기를 켠 상태는 DEPTH_WRITE 하나다(D3D12는 깊이 읽기·쓰기를 함께 켜지 못한다).
// Present는 COMMON(0)이다. Unknown·Discard는 호출자가 걸러 낸다.
D3D12_RESOURCE_STATES StatesFor(ResourceAccess access) noexcept;

// 그래픽 셰이더 읽기 상태다(SetTexture의 암묵 전이 대상, ResourceAccess::SrvGraphics와 같다).
constexpr D3D12_RESOURCE_STATES ShaderReadStates =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// UAV barrier다(같은 UAV 상태 사이의 쓰기 순서).
D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource) noexcept;

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type) noexcept;

D3D12_RESOURCE_DESC BufferResource(uint64 size) noexcept;

struct BufferRecord {
    ComRef<ID3D12Resource> resource;
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    uint64 size = 0;
    BufferDesc desc;
    // placed 버퍼(CreatePlacedBuffer)면 그 transient heap이다. 메모리는 heap 소유이고, 회수 뒤
    // heap의 placed 수를 줄인다(GX12).
    SlotHandle heap;
};

// 버퍼 view(GX2)다. 버퍼 자원 참조를 쥐므로 버퍼를 먼저 놓아도 산다. descriptor는 CPU 전용
// SRV heap(CBV_SRV_UAV)의 칸이다(UAV view도 같은 heap, 그리기 때 shader-visible ring으로 복사한다).
struct BufferViewRecord {
    ComRef<ID3D12Resource> resource;
    uint32 descriptor = 0;
    // ReadWrite view의 CPU 전용 UAV 칸이다(ClearUnorderedAccessViewUint가 요구한다). 없으면 -1이다.
    int64 cpuDescriptor = -1;
    PixelFormat format = PixelFormat::Unknown;
    BufferViewKind kind = BufferViewKind::Typed;
    ViewAccess access = ViewAccess::Read;
    PixelShaderType shaderType = PixelShaderType::None;
    // 구조화 view의 원소 byte다(셰이더 선언 stride와 대조한다).
    uint32 stride = 0;
    // placed 버퍼의 view면 그 heap이다. view가 버퍼 자원을 붙드는 동안 heap도 놓지 않는다(GX12).
    SlotHandle heap;
};

struct TextureRecord {
    ComRef<ID3D12Resource> resource;
    TextureDimension dimension = TextureDimension::Texture2D;
    uint32 width = 0;
    uint32 height = 0;
    // 3D 깊이다(다른 차원은 1).
    uint32 depth = 1;
    uint32 mipLevels = 1;
    // subresource slice 수(TextureSliceCount)이며 D3D12 자원의 배열 크기(3D는 1)와 같다.
    uint32 slices = 1;
    // view(CreateTextureView)면 view가 읽는 형식, 아니면 텍스처 형식이다.
    PixelFormat format = PixelFormat::Unknown;
    bool renderTarget = false;
    // 깊이·스텐실 타깃(TextureUsage::DepthStencil)이다.
    bool depthTarget = false;
    // 만든 용도다(view는 원 텍스처의 용도, ResolveTextureViewRange).
    TextureUsage usage = TextureUsage::Sampled;
    // MSAA 표본 수다(TextureDesc::sampleCount, view는 원 텍스처의 값).
    uint32 sampleCount = 1;
    // 다른 텍스처의 자원을 공유하는 view다. 업로드·readback·그리기 대상이 될 수 없다. 자원 상태는
    // 원 텍스처 기록(source)이 추적한다.
    bool view = false;
    SlotHandle source;
    // view가 가리키는 mip·slice 범위다(view가 아니면 전체). 묶기 전이는 이 범위만 바꾼다.
    ResolvedSubresourceRange viewRange;
    // 쓰기 view(ViewAccess::ReadWrite)다. srv 칸에 UAV descriptor가 있고 SetWritableTexture로만
    // 묶는다(hasSrv 거짓).
    bool writable = false;
    // 이 텍스처를 가리키는 살아 있는 view 수다. 0이 아니면 Retire는 기록을 남겨 두고(retirePending)
    // 마지막 view가 놓일 때 함께 놓는다(상태 추적의 주인을 하나로 유지한다).
    uint32 viewCount = 0;
    bool retirePending = false;
    // 장치 자원의 plane 수다(D3D12_FEATURE_FORMAT_INFO, 깊이·스텐실 typeless와 비디오는 2). mip별
    // 상태 전이는 모든 plane의 subresource에 낸다.
    uint32 devicePlanes = 1;
    // SRV가 있다(형식이 Sampled일 때만 만든다). 없으면 SetTexture는 흰 텍스처로 대신한다.
    bool hasSrv = false;
    uint32 srv = 0;
    uint32 rtv = 0;
    uint32 dsv = 0;
    // 샘플러 힙의 칸이다(SamplerFor가 같은 설정끼리 공유한다).
    uint32 sampler = 0;
    // 샘플러가 깊이 비교 샘플러다(view는 깊이 plane만 읽을 수 있다).
    bool comparison = false;
    // subresource(mip × slice)마다 장치가 추적한 상태다(RenderBase GX5). committed는 제출한 프레임
    // 기록 끝의 상태이고, current는 기록 중인 프레임 안의 상태다(기록 중이 아니면 둘이 같다).
    // 업로드 기록은 프레임 기록보다 먼저 실행되므로 committed에서 바꿨다가 committed로 되돌려
    // 놓는다(상태 중립). 장치 plane들은 같은 상태를 따른다. view 기록은 비어 있다(원 기록이
    // 추적한다).
    SubresourceStates<D3D12_RESOURCE_STATES> committed;
    SubresourceStates<D3D12_RESOURCE_STATES> current;
    // 기록 중인 프레임이 current를 바꿨다(touchedTextures_에 있다).
    bool touched = false;
    // placed 텍스처(CreatePlacedTexture)면 그 transient heap이다(BufferRecord::heap, view는 비어
    // 있다).
    SlotHandle heap;
};

// mip level 하나의 데이터 byte다(모든 slice·깊이 면, TextureMipBytes와 같은 값).
inline uint64 MipBytes(const TextureRecord& record, uint32 level) noexcept {
    const uint64 image = PixelFormatImageBytes(record.format, MipExtent(record.width, level),
                                               MipExtent(record.height, level));
    const uint32 depth =
        record.dimension == TextureDimension::Texture3D ? MipExtent(record.depth, level) : 1u;
    return image * depth * record.slices;
}

// 단계 하나의 root table 배치다(RenderBase 9.11절 5항). 매개변수 번호가 -1이면 그 table이 없다.
// srvBindings·samplerBindings·uavBindings는 PipelineSlots::bindings 색인이고 table 안 순서다.
struct RootStageTables {
    ShaderStage stage = ShaderStage::Vertex;
    int32 srvParam = -1;
    uint32 srvCount = 0;
    uint16 srvBindings[MaxStageTextures + MaxDrawBufferViews] = {};
    int32 samplerParam = -1;
    uint32 samplerCount = 0;
    uint16 samplerBindings[MaxStageSamplers] = {};
    int32 uavParam = -1;
    uint32 uavCount = 0;
    uint16 uavBindings[MaxDrawBufferViews] = {};
    // 넘친 CBV table의 range [cbvFirst, cbvFirst + cbvCount)다.
    int32 cbvParam = -1;
    uint32 cbvFirst = 0;
    uint32 cbvCount = 0;
};

// 파이프라인 하나의 root signature 배치다. root CBV는 rootCbvParam부터 plan.rootOrder 순서다.
struct RootLayout {
    RootStageTables stages[MaxRootBudgetStages];
    uint32 stageCount = 0;
    RootBudgetPlan plan;
    uint32 rootCbvParam = 0;
    int32 debugParam = -1;
    int32 constantsParam = -1;
    uint32 constantDwords = 0;
    uint32 parameterCount = 0;
};

struct PipelineRecord {
    ComRef<ID3D12PipelineState> state;
    // 이 파이프라인 layout의 root signature(장치 캐시가 소유를 나눈다)와 그 배치, 고정 slot 표다.
    ComRef<ID3D12RootSignature> rootSignature;
    RootLayout root;
    PipelineSlots slots;
    // stream별 stride와 배치가 선언한 stream mask다(그리기 때 묶인 정점 버퍼를 확인한다).
    uint32 strides[MaxVertexStreams] = {};
    uint32 streamMask = 0;
    // VS·PS(compute면 CS)가 읽는 버퍼 view slot과 CS가 쓰는 slot이다(리플렉션).
    DrawBufferSlots bufferSlots;
    DrawBufferSlots writeSlots;
    // 선언한 그리기별 상수 byte 수다(SetDrawConstants 크기 검사, dispatch 때 올릴 수).
    uint32 drawConstantBytes = 0;
    // compute가 텍스처 UAV로 쓰는 쓰기 slot 비트다(SetWritableTexture가 묶여야 한다).
    uint32 writeTextures = 0;
    // compute 파이프라인이다(Dispatch만 쓴다).
    bool compute = false;
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    // 깊이·스텐실을 쓴다(읽기 전용 깊이 attachment에서 그리기를 거른다).
    bool writesDepthStencil = false;
};

// 상수 버퍼 handle 저장소 page다(upload heap 버퍼 하나를 계속 map, 9.11절 2항).
struct ConstantPage {
    explicit ConstantPage(Allocator& allocator) noexcept : ranges(allocator) {
    }

    ComRef<ID3D12Resource> resource;
    uint8* data = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    GpuPageAllocator ranges;
};

struct ConstantBufferRecord {
    ConstantBufferLifetime lifetime = ConstantBufferLifetime::SingleFrame;
    uint32 bytes = 0;
    uint32 page = 0;
    GpuPageAllocation block;
};

// queue 하나의 기록 명령 상수 ring과 shader-visible descriptor ring이다(9.11절 2·5항). 묶음은 그
// queue의 제출 fence로 닫는다.
struct TransientRings {
    explicit TransientRings(Allocator& allocator) noexcept
        : constants(allocator), descriptors(allocator), samplers(allocator) {
    }

    UploadRingAllocator constants;
    ComRef<ID3D12Resource> buffer;
    uint8* data = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    UploadRingAllocator descriptors;
    uint32 descriptorBase = 0;
    // Sampler table ring(onlineSamplers_ 안 구간)이다.
    UploadRingAllocator samplers;
    uint32 samplerBase = 0;
};

struct SwapChainRecord {
    ComRef<IDXGISwapChain3> swapChain;
    ComRef<ID3D12Resource> buffers[SwapChainBufferCount];
    uint32 rtv[SwapChainBufferCount] = {};
    uint32 width = 0;
    uint32 height = 0;
    bool vsync = true;
    // ResizeBuffers·버퍼 재취득이 실패해 버퍼가 없는 상태다. 다음 Resize가 성공할 때까지
    // BeginFrame은 DependencyFailed다.
    bool broken = false;
    // present하지 않은 기록이 이 백버퍼에 그렸다(그리는 중인 프레임, SwapChainPassDesc). load
    // Load는 이때만 받는다.
    bool frameOpen = false;
};

// fence 완료 뒤에 해제할 자원과 descriptor다.
struct Retired {
    ComRef<IUnknown> object;
    uint64 fenceValue = 0;
    // 아직 제출하지 않은 업로드 기록이 읽는 전용 staging이다. 업로드 묶음 제출이 fence 값을 정한다.
    bool uploadPending = false;
    // 기록 중인(아직 제출하지 않은) 프레임이 쓸 수 있는 자원이다. EndFrame이 그 프레임의 signal
    // 값을 정할 때까지 해제하지 않는다.
    bool pending = false;
    // compute queue의 마지막 사용이다(RG4): 그 queue에 Retire 때까지 제출한 마지막 값, 기록 중인
    // compute 기록이 쓸 수 있으면 그 기록의 값(computePending, 제출이 정한다).
    uint64 computeFenceValue = 0;
    bool computePending = false;
    int64 srv = -1;
    int64 rtv = -1;
    int64 dsv = -1;
    // 상수 버퍼 page 구간이다(Retire·UpdateConstantBuffer의 옛 자리, 9.11절 2항).
    int64 constantPage = -1;
    GpuPageAllocation constantBlock;
    // placed 자원·placed 버퍼 view의 heap이다. 자원을 놓은 뒤 heap의 placed 수를 줄인다
    // (ReleasePlacement, GX12).
    SlotHandle heap;
};

// transient heap 안 placed 자원 하나의 구간과 aliasing 활성 여부다(GX12). 획득이 자원을 활성으로
// 만들고, 같은 heap에서 구간이 겹치는 다른 자원을 비활성으로 돌린다. 자원 종류는 heap 종류로
// 정해진다(Buffers는 버퍼, 그 밖은 텍스처).
struct PlacedRange {
    SlotHandle resource;
    uint64 offset = 0;
    uint64 bytes = 0;
    bool active = false;
};

// transient heap(GX12)이다. ID3D12Heap 하나를 placed 자원이 offset으로 나눠 쓴다. 회수 대기
// (retirePending)여도 placed 자원·view가 남아 있으면 표에 두고, 마지막이 회수될 때 heap을 놓는다.
struct TransientHeapRecord {
    explicit TransientHeapRecord(Allocator& allocator) noexcept : ranges(allocator) {
    }

    ComRef<ID3D12Heap> heap;
    uint64 size = 0;
    TransientHeapKind kind = TransientHeapKind::Buffers;
    // 아직 회수되지 않은 placed 자원과 placed 버퍼 view 수다.
    uint32 placedCount = 0;
    bool retirePending = false;
    // 살아 있는(Retire 전) placed 자원의 구간이다.
    Array<PlacedRange> ranges;
};

// 고정 용량 descriptor 힙의 빈 칸 관리다.
class DescriptorPool {
public:
    explicit DescriptorPool(Allocator& allocator) noexcept : free_(allocator) {
    }

    Result<void> Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32 capacity,
                            bool shaderVisible) noexcept {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = type;
        desc.NumDescriptors = capacity;
        desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                                   : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap_.Out())))) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }
        increment_ = device->GetDescriptorHandleIncrementSize(type);
        capacity_ = capacity;
        auto reserved = free_.TryReserve(capacity);
        if (!reserved) {
            return reserved;
        }
        // 낮은 번호부터 쓰도록 거꾸로 쌓는다.
        for (uint32 index = capacity; index > 0; --index) {
            auto pushed = free_.TryPushBack(index - 1);
            (void)pushed;
        }
        return Result<void>::Success();
    }

    Result<uint32> Allocate() noexcept {
        if (free_.Size() == 0) {
            return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
        }
        return Result<uint32>::Success(free_.TryPopBack().Value());
    }

    void Free(uint32 index) noexcept {
        // 범위 밖 index나 중복 반환은 이후 할당이 잘못된 descriptor를 돌려주게 하므로 즉시 멈춘다.
        if (index >= capacity_ || free_.Size() >= capacity_) {
            platform::Fatal("D3D12 descriptor freed out of range or twice");
        }
        // 용량을 미리 확보했으므로 실패하지 않는다.
        auto pushed = free_.TryPushBack(index);
        (void)pushed;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32 index) const noexcept {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = heap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * increment_;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(uint32 index) const noexcept {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = heap_->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * increment_;
        return handle;
    }

    ID3D12DescriptorHeap* Heap() const noexcept {
        return heap_.Get();
    }

private:
    ComRef<ID3D12DescriptorHeap> heap_;
    Array<uint32> free_;
    uint32 increment_ = 0;
    uint32 capacity_ = 0;
};

// 스트리밍 staging 메모리(upload heap 버퍼, 계속 map)를 만들고 없앤다(StagingPool의 backend).
class D3D12StagingSource final : public StagingSource {
public:
    Result<StagingMemory> CreateStaging(uint64 bytes) noexcept override {
        ComRef<ID3D12Resource> resource;
        const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC desc = BufferResource(bytes);
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(resource.Out())))) {
            return Result<StagingMemory>::Failure(ErrorCode::OutOfMemory);
        }
        void* mapped = nullptr;
        const D3D12_RANGE noRead = {0, 0};
        if (FAILED(resource->Map(0, &noRead, &mapped))) {
            return Result<StagingMemory>::Failure(ErrorCode::DependencyFailed);
        }
        StagingMemory memory;
        memory.resource = resource.Detach();
        memory.data = static_cast<uint8*>(mapped);
        memory.size = bytes;
        return Result<StagingMemory>::Success(memory);
    }

    void DestroyStaging(const StagingMemory& memory) noexcept override {
        ComRef<ID3D12Resource> resource;
        resource.Attach(static_cast<ID3D12Resource*>(memory.resource));
    }

    ID3D12Device* device = nullptr;
};

// timestamp query heap을 resolve한 readback 버퍼에서 결과를 읽는다(GX9). Harvest가 fence 완료
// 뒤에만 부르므로 값은 이미 버퍼에 있다.
class D3D12TimestampSource final : public TimestampSource {
public:
    TimestampReadState ReadTimestamps(uint32 first, uint32 count, uint64* ticks) noexcept override {
        const D3D12_RANGE range = {static_cast<SIZE_T>(first) * sizeof(uint64),
                                   static_cast<SIZE_T>(first + count) * sizeof(uint64)};
        void* mapped = nullptr;
        if (buffer == nullptr || FAILED(buffer->Map(0, &range, &mapped))) {
            return TimestampReadState::Disjoint;
        }

        platform::MemoryCopy(ticks, static_cast<const uint8*>(mapped) + range.Begin,
                             static_cast<usize>(count) * sizeof(uint64));
        const D3D12_RANGE noWrite = {0, 0};
        buffer->Unmap(0, &noWrite);
        return TimestampReadState::Ready;
    }

    ID3D12Resource* buffer = nullptr;
};

// 비동기 readback 하나다. fence가 끝나면 footprint 배치로 버퍼에 있다.
struct ReadbackRecord {
    ComRef<ID3D12Resource> resource;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    uint64 total = 0;
    uint64 fenceValue = 0;
    // 빈틈 없는 행 하나의 바이트와 행 수다(블록 형식은 블록 행).
    uint64 rowBytes = 0;
    uint32 rows = 0;
    // 2D 이미지 수다(3D mip의 깊이 면, footprint 안에서 RowPitch × rows 간격). 다른 차원은 1이다.
    uint32 images = 1;
};

// fence 관찰 스레드가 제출 하나의 완료를 기다리는 경계다. 장치가 제거되면 fence 완료 값이
// UINT64_MAX가 되어 대기가 풀린다(D3D12 device removed 규칙).
class D3D12FenceSource final : public FenceWaitSource {
public:
    bool WaitSubmission(uint64 value, void* token) noexcept override {
        (void)token;
        uint64 completed = fence->GetCompletedValue();
        if (completed == RemovedFenceValue) {
            return false;
        }

        if (completed >= value) {
            return true;
        }

        if (FAILED(fence->SetEventOnCompletion(value, event))) {
            return false;
        }

        WaitForSingleObject(event, INFINITE);
        completed = fence->GetCompletedValue();
        return completed != RemovedFenceValue;
    }

    // 장치 제거 뒤 fence가 돌려주는 완료 값이다.
    static constexpr uint64 RemovedFenceValue = NumericLimits<uint64>::Max();

    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
};

class D3D12Device final : public Device, private CommandContext {
public:
    explicit D3D12Device(Allocator& allocator) noexcept;

    ~D3D12Device() override;

    Result<void> Initialize(const DeviceDesc& desc) noexcept;

    DeviceInfo Info() const noexcept override;

    const DeviceCaps& Caps() const noexcept override;

    uint32 GetPixelFormatSampleCounts(PixelFormat format) const noexcept override;

    Result<SwapChainHandle> CreateSwapChain(const SwapChainDesc& desc) noexcept override;

    Result<void> ResizeSwapChain(SwapChainHandle handle, uint32 width,
                                 uint32 height) noexcept override;

    // 다른 스레드에서도 부를 수 있다(concurrentResourceCreation). 초기 데이터는 업로드 ring에 쓰고
    // 복사를 기록한다. 소유 스레드의 다음 제출이 실행한다.
    Result<BufferHandle> CreateBuffer(const BufferDesc& desc,
                                      Span<const uint8> initial) noexcept override;

    // 다른 스레드에서도 부를 수 있다(concurrentResourceCreation). descriptor는 CPU 전용 SRV
    // heap 칸 하나다(SRV·UAV 모두, GX2 — 그리기 때 shader-visible ring으로 복사한다).
    Result<BufferViewHandle> CreateBufferView(BufferHandle handle,
                                              const BufferViewDesc& desc) noexcept override;

    // 버퍼 구간을 동기로 읽는다(시험·캡처용). 업로드 기록 뒤에 복사를 붙여 앞선 갱신 뒤의 내용을
    // 본다. 버퍼는 COMMON에서 복사 원본으로 암묵 승격된다.
    Result<void> ReadBuffer(BufferHandle handle, uint64 offset, Span<uint8> out) noexcept override;

    // 다른 스레드에서도 부를 수 있다(concurrentResourceCreation). 초기 픽셀은 업로드 기록으로 간다.
    // 형식·용도 지원은 장치 능력(GetPixelFormatCaps)으로 판정한다(Docs/Design/PixelFormat 8.3절).
    Result<TextureHandle> CreateTexture(const TextureDesc& desc,
                                        Span<const uint8> pixels) noexcept override;

    // 기본 SRV(형식이 Sampled일 때, plane 0)·RTV·DSV를 만든다(잠금 안). 실패하면 이미 잡은 칸을
    // 놓는다.
    Result<void> CreateTextureViews(TextureRecord& record, const d3d12::FormatMapping& mapping,
                                    bool sampled, const ChannelSwizzle& swizzle) noexcept;

    // 다 만든 텍스처 기록의 descriptor 칸을 바로 놓는다(잠금 안, 아직 어떤 명령도 참조하지 않을
    // 때).
    void ReleaseTextureViews(const TextureRecord& record) noexcept;

    // 2D texture SRV를 쓴다. plane은 깊이·스텐실·비디오 plane 선택이다.
    // 패스 끝 MSAA resolve다(EndFrame, 잠금 안).
    void ResolveAttachments() noexcept;
    // attachment (mip, slice)의 RTV·DSV를 handle 칸에 쓴다(3D RTV의 slice는 깊이 면 z).
    void WriteRenderTargetView(const TextureRecord& record, uint32 mipLevel, uint32 slice,
                               D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept;
    void WriteDepthStencilView(const TextureRecord& record, uint32 mipLevel, uint32 slice,
                               bool readOnly, D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept;
    Result<TextureHandle> CreateWritableTextureView(TextureHandle texture, TextureRecord& source,
                                                    PixelFormat format,
                                                    const ResolvedSubresourceRange& range,
                                                    TextureDimension dimension) noexcept;
    void WriteShaderResourceView(const TextureRecord& record, DXGI_FORMAT format, uint32 plane,
                                 const ChannelSwizzle& swizzle, uint32 slot) noexcept;

    // 기록을 표에 넣고 handle을 돌려준다(잠금 안). 실패하면 descriptor 칸을 놓는다.
    Result<TextureHandle> AddTextureRecord(TextureRecord&& record) noexcept;

    PixelFormatCaps FormatCaps(PixelFormat format) const noexcept;

    PixelFormatCaps GetPixelFormatCaps(PixelFormat format) const noexcept override;

    // 원 텍스처의 자원을 공유하는 SRV view다(Docs/Design/PixelFormat 9–11절). 다른 스레드에서도
    // 부를 수 있다.
    Result<TextureHandle> CreateTextureView(TextureHandle texture,
                                            const TextureViewDesc& desc) noexcept override;

    // 텍스처 mip·plane 하나를 동기로 읽는다(시험·캡처용). ReadPixels와 같은 경로다.
    Result<TextureSubresources>
    GetTextureSubresources(TextureHandle texture) const noexcept override;

    Result<TextureViewInfo> GetTextureViewInfo(TextureHandle texture) noexcept override;

    Result<BufferViewInfo> GetBufferViewInfo(BufferViewHandle view) noexcept override;
    Result<void> ReadTexture(TextureHandle handle, uint32 mipLevel, uint32 plane,
                             Span<uint8> out) noexcept override;

    // 다른 스레드에서도 부를 수 있다(concurrentResourceCreation, 내부 잠금으로 직렬화한다).
    Result<PipelineHandle> CreatePipeline(const PipelineDesc& desc) noexcept override;
    Result<PipelineHandle> CreateComputePipeline(const ComputePipelineDesc& desc) noexcept override;

    using Device::BeginFrame;
    Result<CommandContext*> BeginFrame(const SwapChainPassDesc& desc) noexcept override;

    Result<CommandContext*> BeginPass(const PassDesc& pass) noexcept override;

    Result<GpuFencePoint> EndFrame() noexcept override;

    Result<void> QueueWait(QueueKind queue, GpuFencePoint point) noexcept override;

    Result<void> ReadPixels(TextureHandle handle, Span<uint8> out) noexcept override;

    // --- 업로드·readback(GX8) ---

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

    void Retire(BufferHandle handle) noexcept override;

    void Retire(BufferViewHandle handle) noexcept override;

    // --- 상수 버퍼(RenderBase 9.11절 GX6, D3D12Constants.cpp) ---

    // 다른 스레드에서도 부를 수 있다(concurrentResourceCreation). 내용은 upload page 구간이다.
    Result<ConstantBufferHandle> CreateConstantBuffer(ConstantBufferLifetime lifetime,
                                                      Span<const uint8> contents) noexcept override;
    Result<void> UpdateConstantBuffer(ConstantBufferHandle buffer,
                                      Span<const uint8> contents) noexcept override;
    void Retire(ConstantBufferHandle buffer) noexcept override;

    void Retire(TextureHandle handle) noexcept override;

    // 텍스처 기록 하나를 회수 대기로 넘기고 표에서 뺀다(잠금 안).
    void RetireTextureRecord(SlotHandle slot, TextureRecord& record) noexcept;

    void Retire(PipelineHandle handle) noexcept override;
    Result<ShaderHandle> CreateShader(const ShaderCodeDesc& desc) noexcept override;
    void Retire(ShaderHandle shader) noexcept override;

    void Retire(SwapChainHandle handle) noexcept override;

    void ReclaimCompleted() noexcept override;

    Result<void> WaitIdle() noexcept override;

    Result<void> WaitIdleLocked() noexcept;

    GpuFencePoint LastSubmittedFence(QueueKind queue = QueueKind::Graphics) const noexcept override;

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

    uint64 PresentCount() const noexcept override;

    StringView LastError() const noexcept override;

    uint64 IgnoredDraws() const noexcept override;
    uint64 NullTextureBindings() const noexcept override;

    usize CollectTimestamps(Span<TimestampSample> out) noexcept override;

    Result<ClockCalibration>
    CalibrateClocks(QueueKind queue = QueueKind::Graphics) noexcept override;

    uint64 ValidationErrorCount() const noexcept override;

    // --- transient heap·placed 자원(GX12, D3D12Transient.cpp) ---

    // 다른 스레드에서도 부를 수 있다(concurrentResourceCreation).
    Result<TransientHeapHandle>
    CreateTransientHeap(const TransientHeapDesc& desc) noexcept override;
    void RetireTransientHeap(TransientHeapHandle heap) noexcept override;
    // 스레드 안전하다(장치 생성 뒤 바뀌지 않는 표와 GetResourceAllocationInfo만 쓴다).
    Result<TransientPlacement> GetTexturePlacement(const TextureDesc& desc) const noexcept override;
    Result<TransientPlacement> GetBufferPlacement(const BufferDesc& desc) const noexcept override;
    Result<TextureHandle> CreatePlacedTexture(TransientHeapHandle heap, uint64 offset,
                                              const TextureDesc& desc) noexcept override;
    Result<BufferHandle> CreatePlacedBuffer(TransientHeapHandle heap, uint64 offset,
                                            const BufferDesc& desc) noexcept override;

private:
    // placed 자원을 둘 heap 구간이다(CreatePlacedTexture·CreatePlacedBuffer).
    struct Placement {
        SlotHandle heap;
        uint64 offset = 0;
    };

    // 텍스처 desc 검사 결과다. code가 None이 아니면 실패이고, unsupportedFormat이면 형식 문구,
    // detail이 있으면 그 문구를 LastError에 남긴다.
    struct TextureDescCheck {
        ErrorCode code = ErrorCode::None;
        bool unsupportedFormat = false;
        const char* detail = nullptr;
    };

    // CreateTexture와 placement 질의가 함께 쓰는 desc 검사다(잠금 밖, 스레드 안전).
    TextureDescCheck CheckTextureDesc(const TextureDesc& desc, usize pixelBytes) const noexcept;

    // 텍스처 desc의 D3D12 자원 설정이다(committed·placed·placement 질의가 같은 값을 쓴다).
    static D3D12_RESOURCE_DESC TextureResourceFor(const TextureDesc& desc) noexcept;

    // 버퍼 desc의 D3D12 자원 설정이다.
    static D3D12_RESOURCE_DESC BufferResourceFor(const BufferDesc& desc) noexcept;

    // 텍스처·버퍼 생성 본체다. placement가 있으면 그 heap 구간에 placed 자원으로 두고(초기 데이터
    // 없음) 없으면 committed 자원이다. view·descriptor·상태 추적은 같다.
    Result<TextureHandle> CreateTextureIn(const TextureDesc& desc, Span<const uint8> pixels,
                                          const Placement* placement) noexcept;
    Result<BufferHandle> CreateBufferIn(const BufferDesc& desc, Span<const uint8> initial,
                                        const Placement* placement) noexcept;

    // placement 구간을 확인하고 heap 기록을 돌려준다(잠금 안). heap이 없거나 회수 대기면
    // StaleHandle, 종류가 다르거나 offset이 정렬·범위 밖이면 InvalidArgument다. 성공하면 bytes에
    // 자원 크기를 두고 구간 목록에 칸 하나를 미리 잡는다(TrackPlaced가 실패하지 않게).
    Result<TransientHeapRecord*> CheckPlacement(const Placement& placement, TransientHeapKind kind,
                                                const D3D12_RESOURCE_DESC& resource,
                                                uint64& bytes) noexcept;

    // 표에 넣은 placed 자원을 heap 구간 목록에 더하고 placed 수를 올린다(잠금 안, 비활성으로
    // 시작한다).
    void TrackPlaced(SlotHandle heap, SlotHandle resource, uint64 offset, uint64 bytes) noexcept;

    // Retire한 placed 자원을 heap 구간 목록에서 뺀다(잠금 안). 메모리는 회수 뒤
    // ReleasePlacement가 놓는다.
    void ForgetPlaced(SlotHandle heap, SlotHandle resource) noexcept;

    // placed 자원·view 하나가 회수됐다(잠금 안). heap이 회수 대기이고 마지막이면 heap을 놓는다.
    void ReleasePlacement(SlotHandle heap) noexcept;

    // placed 자원을 활성으로 만든다(잠금 안, graphics 기록). 이미 활성이면 거짓이다. 아니면
    // aliasing barrier(앞 자원 null)를 내고 같은 heap에서 구간이 겹치는 다른 자원을 비활성으로
    // 돌린 뒤 참이다.
    bool ActivatePlaced(SlotHandle heap, SlotHandle resource, ID3D12Resource* object) noexcept;

    // placed 텍스처의 aliasing 획득이다(ResourceTransition::acquire·attachment acquire, 잠금 안).
    // 새로 활성이 된 렌더 타깃·깊이는 전체를 RENDER_TARGET·DEPTH_WRITE로 바꿔 DiscardResource로
    // 초기화한다(D3D12 규칙). cleared면 패스의 attachment clear가 자원 전체를 초기화하므로
    // Discard를 생략한다. compute 기록에서는 하지 않고 IgnoredDraws로 센다.
    void AcquireTexture(TextureRecord& record, SlotHandle slot, bool cleared = false) noexcept;

    // placed 버퍼의 aliasing 획득이다(AcquireTexture와 같은 규칙, 초기화는 없다).
    void AcquireBuffer(const BufferRecord& record, SlotHandle slot) noexcept;

    // 획득 전이의 before가 Discard가 아니면 검증 오류로 센다(검증 장치, 획득은 그대로 한다).
    void CheckAcquireBefore(const ResourceTransition& transition) noexcept;

    // 기록 저장소 하나와 그것으로 마지막에 제출한 fence 값이다(0이면 쓴 적 없음).
    struct AllocatorRecord {
        ComRef<ID3D12CommandAllocator> allocator;
        uint64 fenceValue = 0;
        // 지금 프레임·업로드 기록이 쓰는 중이다(고르지 않는다).
        bool inUse = false;
    };

    // 패스 대상이다. 색 0(resource·rtv·format·texture)은 CopyPassTarget이 쓴다.
    struct Target {
        ID3D12Resource* resource = nullptr;
        D3D12_RESOURCE_STATES restState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
        uint32 width = 0;
        uint32 height = 0;
        // 패스 대상의 형식이다(스왑체인은 RGBA8UnormSrgb view).
        PixelFormat format = PixelFormat::Unknown;
        // offscreen 패스의 대상 텍스처다(스왑체인 패스는 null).
        TextureHandle texture;
        // BeginPass의 attachment들이다(스왑체인 패스는 색 1개).
        uint32 colorCount = 0;
        TextureHandle colors[MaxColorTargets];
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[MaxColorTargets] = {};
        TextureHandle depth;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
        bool depthReadOnly = false;
        // 색 attachment마다 패스 끝에 resolve할 대상이다(ColorAttachment::resolveTarget).
        TextureHandle resolves[MaxColorTargets];
        // attachment 표본 수다(1이면 MSAA가 아니다).
        uint32 sampleCount = 1;
        // attachment마다 붙인 mip·slice와 resolve 대상의 mip·slice다(텍스처 GX2 C부).
        uint32 colorMips[MaxColorTargets] = {};
        uint32 colorSlices[MaxColorTargets] = {};
        uint32 resolveMips[MaxColorTargets] = {};
        uint32 resolveSlices[MaxColorTargets] = {};
        uint32 depthMip = 0;
        uint32 depthSlice = 0;
    };

    // 기록 중 버퍼 자원의 상태다. 버퍼는 제출 끝에 COMMON으로 돌아가므로 기록마다 비우고, 처음 쓰면
    // 암묵 승격으로 그 상태가 된다(RequireBufferState).
    struct BufferState {
        ID3D12Resource* resource = nullptr;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
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
        uint32 slot = 0;
    };

    // 진단 문자열을 남기고 실패를 돌려준다. 장치가 손실되었으면 Destroyed다. 실패는 호출자가 결과로
    // 받아 결정하는 쪽이 기록한다(LoggingSystem 13절). 장치는 손실 전이만 직접 남긴다.
    Result<void> Fail(const char* message) noexcept;

    // API 호출 stage가 HRESULT로 실패했다. 원본 코드를 보존하고(DeviceCreateReport::nativeCode)
    // LastError에 "stage failed (HRESULT 0x...)"로 덧붙인다. 장치 제거면 손실을 게시한다.
    Result<void> Fail(const char* stage, HRESULT result) noexcept;

    // 실패한 stage 이름을 report용 칸에 복사한다. 잘렸으면 참이다.
    bool CopyStage(const char* stage) noexcept;

    // API 실패가 장치 제거 때문이면 손실을 게시한다(기다리는 쪽을 Destroyed로 깨운다). 손실 전이 때
    // GetDeviceRemovedReason과 함께 Error를 한 번 남긴다.
    void DetectLost(HRESULT failure) noexcept;

public:
    // DeviceCreateReport를 채운다(생성 실패, CreateD3D12Device가 부른다).
    void FillReport(ErrorCode code, DeviceCreateReport& report) const noexcept;

private:
    // graphics queue에 다음 fence 값을 신호하고 타임라인에 알린다. 실패하면 값을 쓰지 않는다.
    Result<uint64> SignalQueue() noexcept;

    // 소유 스레드가 직접 본 완료 값을 타임라인에 게시한다(관찰 스레드보다 먼저 알 수 있다).
    void PublishObserved() noexcept;

    Result<uint32> AddCommandAllocator() noexcept;

    // GPU가 다 쓴 기록 저장소를 고른다. 없으면 상한까지 새로 만들고, 상한이면 가장 오래된 제출을
    // 기다린다(프레임 slot이 앞서 기다리므로 보통은 기다리지 않는다).
    Result<uint32> PickCommandAllocator() noexcept;

    template <class T>
    Result<T> FailWith(const char* message) noexcept {
        auto failed = Fail(message);
        (void)failed;
        return Result<T>::Failure(ErrorCode::DependencyFailed);
    }

    template <class T>
    Result<T> FailWith(const char* stage, HRESULT result) noexcept {
        auto failed = Fail(stage, result);
        return Result<T>::Failure(failed.Error());
    }

    // 인자·능력 실패를 code로 돌려주고 LastError에 message를 남긴다(API 실패가 아니다).
    template <class T>
    Result<T> FailCode(ErrorCode code, const char* message) noexcept {
        auto assigned = lastError_.TryAssign(StringView(message, platform::CStringLength(message)));
        (void)assigned;
        return Result<T>::Failure(code);
    }

    // 이 backend가 대응하지 않는 픽셀 형식이다. 다른 형식으로 바꾸지 않고 Unsupported로
    // 실패한다(Docs/Design/PixelFormat 8.3절). 호출자는 gate_를 잡고 있어야 한다.
    template <class T>
    Result<T> UnsupportedFormat(PixelFormat format) noexcept {
        constexpr const char* Prefix = "pixel format ";
        constexpr const char* Suffix = " is not supported for this use by the D3D12 device";
        const char* name = PixelFormatName(format);
        auto assigned = lastError_.TryAssign(StringView(Prefix, platform::CStringLength(Prefix)));
        auto named = lastError_.TryAppend(StringView(name, platform::CStringLength(name)));
        auto ended = lastError_.TryAppend(StringView(Suffix, platform::CStringLength(Suffix)));
        (void)assigned;
        (void)named;
        (void)ended;
        return Result<T>::Failure(ErrorCode::Unsupported);
    }

    // desc와 같은 샘플러 칸을 돌려주고 없으면 만든다. 칸은 장치 수명 동안 유지한다(조합 수가
    // 작다).
    Result<uint32> SamplerFor(const SamplerDesc& desc) noexcept;

    // 파이프라인 slot 표의 root signature다(ShaderSystem 7.2절 4항 — 파이프라인 layout마다 하나를
    // 캐시한다). 같은 (단계, 범위 종류, register) 목록이면 같은 객체를 돌려준다. 호출자는 gate_를
    // 잡고 있어야 한다.
    // slot 표의 root 배치(9.11절 5항)와 그 root signature다(캐시). 실패: 예산 계획 실패
    // InvalidArgument, 직렬화·생성 실패 DependencyFailed.
    Result<ComRef<ID3D12RootSignature>> RootSignatureFor(const PipelineSlots& slots, bool compute,
                                                         uint32 drawConstantBytes,
                                                         RootLayout& layout) noexcept;

    // --- 상수 버퍼 내부(D3D12Constants.cpp, 잠금 안) ---

    Result<void> InitializeConstants() noexcept;
    // page에서 bytes(256 올림)를 잡는다. 자리가 없으면 page를 하나 더 만든다.
    Result<void> AllocateConstantBlock(uint64 bytes, uint32& page,
                                       GpuPageAllocation& block) noexcept;
    // 옛 구간을 마지막 사용 fence 뒤 돌려주도록 회수 목록에 넣는다.
    void RetireConstantBlock(uint32 page, const GpuPageAllocation& block) noexcept;
    TransientRings& ActiveRings() noexcept;
    // 지금 기록 queue의 ring에서 자리를 잡는다(꽉 차면 끝난 제출을 회수하고 가장 오래된 묶음을
    // 기다린다). 이 기록만으로 찼으면 CapacityExceeded다.
    Result<uint64> AllocateTransient(UploadRingAllocator& ring, uint64 size,
                                     uint64 alignment) noexcept;
    // shader-visible ring에서 count칸을 잡아 첫 칸의 CPU·GPU handle을 준다.
    bool AllocateOnline(uint32 count, D3D12_CPU_DESCRIPTOR_HANDLE& cpu,
                        D3D12_GPU_DESCRIPTOR_HANDLE& gpu) noexcept;
    // 기록이 끝났다: 그 queue ring의 열린 묶음을 fence로 닫는다.
    void CloseTransientRings(bool compute, uint64 fence) noexcept;
    // 묶인 (단계, slot)의 GPU 주소와 byte 수다(없으면 0).
    D3D12_GPU_VIRTUAL_ADDRESS BoundConstantAddress(uint32 stage, uint32 slot,
                                                   uint32& bytes) const noexcept;
    // 파이프라인이 읽는 상수 버퍼가 모두 묶였는지다(ConstantBuffersSatisfied).
    bool ConstantsBound(const PipelineRecord& pipeline) const noexcept;
    // table 하나를 원본 목록(SRV·UAV는 srvs_ 칸, Sampler는 samplers_ 칸)으로 채운다. 바로 앞 같은
    // 목록이면 그 구간을 다시 쓴다.
    bool CopyTable(uint32 cacheStage, uint32 cacheKind, const uint32* sources, uint32 count,
                   D3D12_GPU_DESCRIPTOR_HANDLE& gpu) noexcept;
    // shader-visible sampler ring에서 count칸을 잡는다.
    bool AllocateOnlineSamplers(uint32 count, D3D12_CPU_DESCRIPTOR_HANDLE& cpu,
                                D3D12_GPU_DESCRIPTOR_HANDLE& gpu) noexcept;
    // 단계·slot에 묶인 텍스처 기록이다(없거나 SRV가 없으면 흰 텍스처).
    const TextureRecord* BoundTexture(uint32 stage, uint32 slot) const noexcept;

    // 묶인 파이프라인의 root signature·루트 상수·slot table을 지금 기록에 건다(그리기·dispatch
    // 직전).
    bool ApplyRootBindings(const PipelineRecord& pipeline) noexcept;

    // SetDrawConstants 거부(묶인 파이프라인 없음, 크기가 선언과 다름)의 InvalidArgument다.
    Result<void> RejectDrawConstantSize(bool pipelineBound, usize bytes,
                                        uint32 drawConstantBytes) noexcept;

    Result<void> Reject(const Diagnostic& message) noexcept;

    // 묶인 정점 버퍼 하나를 지금 파이프라인의 stride로 IA에 건다.
    void ApplyVertexBuffer(uint32 stream) noexcept;

    // 묶인 파이프라인이 선언한 stream에 모두 유효한 버퍼가 있는지다.
    bool StreamsBound() const noexcept;

    // 간접 명령 signature와 compute 쓰기 기본 칸을 만든다(D3D12Commands.cpp).
    Result<void> CreateComputeObjects() noexcept;

    Result<void> CreateSwapChainViews(SwapChainRecord& record) noexcept;

    // 소유 스레드가 value 완료를 직접 기다린다(업로드·readback·WaitIdle·저장소 재사용).
    Result<void> WaitFenceOwner(uint64 value) noexcept;

    // timestamp query heap과 resolve 대상 readback 버퍼를 만든다(GX9). queue가 timestamp 주파수를
    // 주지 않거나 만들 수 없으면 timestamp를 "없음"으로 공개한다(장치 생성은 실패하지 않는다).
    void InitializeTimestamps() noexcept;

    // GPU가 끝낸 프레임의 timestamp를 회수한다(잠금 안). 장치가 제거되었으면 값이 없으므로 읽지
    // 않는다.
    void HarvestTimestamps() noexcept;

    // 프레임 끝에 열린 marker 구간을 닫는다(잠금 안).
    void CloseMarkers() noexcept;

    Result<void> BeginRecording() noexcept;

    // 기록 시작 때 묶기·상수·버퍼 상태를 비운다(graphics·compute 기록 공용).
    void ResetBindings() noexcept;

    // --- async compute queue(RG4, D3D12Compute.cpp) ---

    // compute queue·fence·관찰 스레드·list·timestamp를 만든다. 실패하면 compute queue 없이
    // 계속한다(DeviceFeatures::asyncCompute 거짓).
    void InitializeComputeQueue() noexcept;

    // compute queue 관찰 스레드를 멈추고 timeline을 닫는다(소멸, WaitIdle 뒤).
    void ShutdownComputeQueue() noexcept;

    // compute 기록 저장소를 고른다(graphics의 PickCommandAllocator와 같은 규칙).
    Result<uint32> PickComputeAllocator() noexcept;

    // 소유 스레드가 compute fence value 완료를 직접 기다린다.
    Result<void> WaitComputeOwner(uint64 value) noexcept;

    // compute 기록을 연다(BeginPass의 queue = AsyncCompute, 잠금 안).
    Result<CommandContext*> BeginComputeRecording() noexcept;

    // compute 기록을 compute queue에 제출한다(EndFrame, 잠금 안).
    Result<GpuFencePoint> EndComputeRecording() noexcept;

    void AssignPendingComputeRetires(uint64 value) noexcept;

    // compute queue의 완료 값이다(없으면 graphics 완료와 무관하게 늘 끝난 것으로 본다).
    uint64 ComputeCompletedValue() const noexcept;

    // 지점의 queue timeline이다.
    const FenceTimeline& TimelineOf(GpuFencePoint point) const noexcept;

    // 지금 compute 기록이면 상태에서 픽셀 셰이더 읽기 비트를 빼고, compute queue가 받지 않는
    // 상태(렌더 타깃·깊이·resolve·index 등)가 남으면 false다. graphics 기록이면 그대로 true다.
    bool AdaptForQueue(D3D12_RESOURCE_STATES& states) const noexcept;

    void StartPass(const PassDesc& pass) noexcept;

    // 업로드 기록 list를 연다(잠금 안). 이미 열려 있으면 그대로 쓴다.
    Result<void> OpenUploadList() noexcept;

    // 업로드 원본 공간이다. ring 구간이거나 전용 staging이다.
    struct UploadSpace {
        ID3D12Resource* resource = nullptr;
        uint64 offset = 0;
        uint8* data = nullptr;
    };

    // size byte 업로드 공간을 잡는다(잠금 안). ring에 자리가 없으면 소유 스레드는 쌓인 업로드를
    // 제출하고 가장 오래된 묶음의 fence를 기다려 비운다. 다른 스레드이거나 그래도 없으면 전용
    // staging을 만들고 그 업로드 묶음의 fence 뒤에 놓는다.
    Result<UploadSpace> StageUpload(uint64 size, uint64 alignment) noexcept;

    // 버퍼 구간 복사를 업로드 기록에 쌓는다(잠금 안).
    Result<void> RecordBufferUpload(ID3D12Resource* target, uint64 offset,
                                    Span<const uint8> data) noexcept;

    // footprint 배치의 원본에서 mip 하나의 plane 하나(subresource)로 복사를 기록한다. restore면
    // 제출한 상태(committed)에서 복사 대상으로 바꿨다가 되돌린다(업로드 기록은 프레임 기록보다 먼저
    // 실행되므로 상태 중립이어야 한다). 처음 생성(COPY_DEST)이면 복사 뒤 셰이더 읽기 상태로 바꾸고
    // 그것을 추적 상태로 삼는다. 상태는 mip 단위로 추적하고 그 mip의 plane들이 함께 따른다.
    void CopyTextureFrom(TextureRecord& record, uint32 mipLevel, uint32 slice, uint32 subresource,
                         ID3D12Resource* source,
                         const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                         bool restore) noexcept;

    // 빈틈 없는 행의 픽셀(mip firstMip부터 count개, mip 안은 slice·깊이 면 순서, 이미지 안은 plane
    // 순서 — TextureDesc 초기 픽셀 배치)을 장치 행 정렬로 ring에
    // 옮기고 복사를 기록한다. 블록 압축 형식의 행은 블록 행이다.
    Result<void> RecordTextureUpload(TextureRecord& record, uint32 firstMip, uint32 count,
                                     Span<const uint8> pixels, bool restore) noexcept;

    // 쌓인 업로드 기록을 제출하고 그 fence 값을 돌려준다(잠금 안, 소유 스레드). 없으면 0이다.
    Result<uint64> FlushUploadsLocked() noexcept;

    void AssignPendingUploads(uint64 value) noexcept;

    // 텍스처 mip 하나·slice 하나의 plane 하나를 readback 버퍼로 복사하는 기록을 쌓고 바로
    // 제출한다(잠금 안). 3D면 그 mip의 모든 깊이 면이다. requireRenderTarget이면 RenderTarget
    // 텍스처만 받는다(ReadPixels·RequestReadback 계약).
    Result<ReadbackRequest> RequestReadbackLocked(TextureHandle target, uint32 mipLevel = 0,
                                                  uint32 plane = 0, bool requireRenderTarget = true,
                                                  uint32 slice = 0) noexcept;

    Result<bool> TryReadbackLocked(ReadbackHandle handle, Span<uint8> out) noexcept;

    void ReleaseReadbackLocked(ReadbackHandle handle) noexcept;

    void KeepUntilDestruction(ComRef<ID3D12Resource>&& object) noexcept;

    void ConfigureValidationQueues() noexcept;

    // 저장된 검증 메시지 중 오류를 세어 누적하고 비운다. 메시지 길이에 상한을 두지 않는다.
    void DrainValidationMessages() const noexcept;

    // 메시지 구조체를 담을 16 byte 정렬 버퍼다. 할당 실패면 null이다.
    void* Scratch(SIZE_T length) const noexcept;

    // 모든 D3D 인터페이스는 IUnknown을 단일 상속하므로 포인터 변환이 안전하다.
    // heap은 placed 자원·view의 transient heap이다(회수 뒤 ReleasePlacement).
    template <class T>
    void Defer(ComRef<T>&& object, int64 srv, int64 rtv, int64 dsv = -1,
               SlotHandle heap = {}) noexcept {
        ComRef<IUnknown> unknown;
        unknown.Attach(object.Detach());
        RetireObject(Move(unknown), srv, rtv, dsv, heap);
    }

    void RetireObject(ComRef<IUnknown>&& object, int64 srv, int64 rtv, int64 dsv = -1,
                      SlotHandle heap = {}) noexcept;

    // 살아 있는 자원이 모두 retire되어도 목록이 자라지 않도록 용량을 확보한다. Retire가 할당 실패로
    // GPU 사용 중인 자원을 놓는 경로를 없앤다.
    Result<void> ReserveRetireCapacity(usize extra) noexcept;

    void AssignPendingRetires(uint64 value) noexcept;

    void ProcessRetired() noexcept;

    // --- CommandContext ---

    void SetPipeline(PipelineHandle handle) noexcept override;

    void SetVertexBuffers(uint32 firstStream,
                          Span<const VertexBufferBinding> buffers) noexcept override;

    void SetIndexBuffer(BufferHandle buffer, IndexFormat format, uint64 offset) noexcept override;

    void SetTexture(ShaderStage stage, uint32 slot, TextureHandle handle) noexcept override;

    Result<void> SetSampler(ShaderStage stage, uint32 slot,
                            const SamplerDesc* sampler) noexcept override;

    Result<void> SetDrawConstants(Span<const uint8> bytes) noexcept override;

    void SetBufferView(uint32 slot, BufferViewHandle handle) noexcept override;

    void SetConstantBuffer(ShaderStage stage, uint32 slot,
                           ConstantBufferHandle buffer) noexcept override;

    Result<void> SetConstantData(ShaderStage stage, uint32 slot,
                                 Span<const uint8> bytes) noexcept override;

    // --- GX3·GX4(D3D12Commands.cpp) ---

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

    // 기록 중 버퍼의 상태를 needed로 맞춘다(COMMON이면 암묵 승격, 읽기끼리는 누적, 그 밖은
    // barrier, UAV → UAV는 UAV barrier).
    void RequireBufferState(ID3D12Resource* resource, D3D12_RESOURCE_STATES needed) noexcept;

    // 그리기 직전 조건을 보고 묶인 정점·index 버퍼·view의 상태를 맞춘다. 그릴 수 없으면 거짓이다.
    bool PrepareDraw(bool indexed) noexcept;

    // dispatch 직전 조건을 보고 compute root 인자와 view 상태를 맞춘다.
    bool PrepareDispatch() noexcept;

    // 간접 인자 버퍼를 확인하고 INDIRECT_ARGUMENT로 맞춘다.
    ID3D12Resource* IndirectArguments(BufferHandle arguments, uint64 offset, uint64 bytes) noexcept;

    // 파이프라인이 읽는 버퍼 view slot이 모두 맞는 view로 채워졌는지다(DrawBufferSlotsSatisfied).
    bool BufferViewsBound() const noexcept;

    void Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept override;

    void DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex) noexcept override;

    Result<void> CopyPassTarget(TextureHandle destination) noexcept override;

    // 명시 전이를 resource barrier로 낮춘다(GX5, Device.h Transition 주석).
    void Transition(Span<const ResourceTransition> transitions) noexcept override;

    // 분리 전이다. UE D3D12와 같이 native split barrier(BEGIN_ONLY·END_ONLY)를 쓰지 않고 end에서
    // Transition과 같이 한 번에 낮춘다(D3D12LegacyBarriers.cpp:1111–1127). begin은 짝만 연다.
    // before 대조와 mip별 current 갱신은 end 때다(사이 접근 금지 계약이므로 begin 때와 같다).
    SplitTransition BeginTransitions(Span<const ResourceTransition> transitions) noexcept override;
    void EndTransitions(SplitTransition split,
                        Span<const ResourceTransition> transitions) noexcept override;

    // Transition의 잠금 안 본체다.
    void TransitionLocked(Span<const ResourceTransition> transitions) noexcept;

    // 버퍼 전이다. 기록 중 추적 상태(RequireBufferState)로 COMMON 암묵 승격·읽기 누적·barrier를
    // 고르고, UAV → UAV는 UAV barrier다(GX5).
    void LowerBufferTransition(const ResourceTransition& transition) noexcept;

    // 기록 중 프레임에서 텍스처 mip [first, first + count)를 after로 바꾼다(잠금 안). 바뀌는 mip만
    // barrier를 내고, 모든 mip이 같은 상태에서 바뀌면 한 barrier로 낸다. uavOrder면 이미 같은 UAV
    // 상태인 mip에 UAV barrier를 낸다.
    void TrackTransition(TextureRecord& record, SlotHandle slot,
                         const ResolvedSubresourceRange& range, D3D12_RESOURCE_STATES after,
                         bool uavOrder = false) noexcept;

    // 프레임 기록을 끝낸다(잠금 안). 제출했으면 바뀐 상태를 committed로 삼고, 제출하지 못했으면
    // current를 committed로 되돌린다.
    void SettleTracking(bool executed) noexcept;

    Result<uint64> WriteTimestamp() noexcept override;

    // PIX가 읽는 이벤트 형식(metadata 0 = UTF-16 문자열)으로 command list에 구간을 기록한다.
    void PushMarker(StringView name) noexcept override;

    void PopMarker() noexcept override;

    String adapterName_;
    String lastError_;
    SlotArray<BufferRecord> buffers_;
    SlotArray<TextureRecord> textures_;
    SlotArray<PipelineRecord> pipelines_;
    SlotArray<SwapChainRecord> swapChains_;
    // transient heap(GX12)이다.
    SlotArray<TransientHeapRecord> heaps_;
    Array<Retired> retired_;
    DescriptorPool rtvs_;
    DescriptorPool dsvs_;
    DescriptorPool srvs_;
    DescriptorPool samplers_;
    SamplerEntry samplerDescs_[SamplerCapacity];
    // 형식 번호별 능력이다(장치 생성 때 채우고 바꾸지 않는다).
    PixelFormatCaps formatCaps_[MaxPixelFormatNumber + 1] = {};
    // 형식 번호별 MSAA 표본 수 집합이다(GetPixelFormatSampleCounts).
    uint32 sampleCounts_[MaxPixelFormatNumber + 1] = {};
    // 장치 능력(GX1)과 최고 feature level이다(생성 때 정해진다).
    DeviceCaps caps_;
    uint32 featureLevel_ = 0;
    uint32 samplerCount_ = 0;
    // AnisotropicPoint를 AnisotropicLinear로 만든 일을 로그에 남겼다(한 번만).
    bool anisotropicPointReported_ = false;
    ComRef<IDXGIFactory4> factory_;
    ComRef<ID3D12Device> device_;
    ComRef<ID3D12InfoQueue> infoQueue_;
    ComRef<ID3D12CommandQueue> queue_;
    // graphics 프레임 기록 list다. list_는 지금 기록의 list(graphics 또는 compute)를 가리킨다.
    ComRef<ID3D12GraphicsCommandList> graphicsList_;
    // 지금 기록의 list다(graphicsList_ 또는 computeList_가 소유한다, RG4).
    ID3D12GraphicsCommandList* list_ = nullptr;
    ComRef<ID3D12GraphicsCommandList> uploadList_;
    // 간접 명령 signature다.
    ComRef<ID3D12CommandSignature> drawSignature_;
    ComRef<ID3D12CommandSignature> drawIndexedSignature_;
    ComRef<ID3D12CommandSignature> dispatchSignature_;
    ComRef<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    // fence 관찰 스레드 전용 이벤트다(소유 스레드의 fenceEvent_와 나눈다).
    HANDLE watcherEvent_ = nullptr;
    Array<AllocatorRecord> allocators_;
    uint32 recordingAllocator_ = 0;
    // 마지막으로 신호한 graphics queue fence 값이다.
    uint64 fenceValue_ = 0;
    Target target_;
    // 지금 기록의 스왑체인(BeginFrame)과 EndFrame이 present할지다.
    SwapChainHandle presentTarget_;
    bool presentOnEnd_ = false;
    // EndFrame이 낸 present 수다(PresentCount).
    uint64 presents_ = 0;
    TextureHandle white_;
    SlotHandle boundPipeline_;
    BoundVertexBuffer boundVertex_[MaxVertexStreams];
    BoundIndexBuffer boundIndex_;
    uint64 ignoredDraws_ = 0;
    // SetTexture가 흰 텍스처로 대신 묶은 수다(NullTextureBindings).
    uint64 nullTextureBindings_ = 0;
    ComRef<IDXGIInfoQueue> dxgiInfoQueue_;
    // 검증 메시지는 읽을 때마다 비우고 여기에 누적한다(저장 한도로 오래된 오류가 밀려나지 않게).
    mutable uint64 validationErrors_ = 0;
    // 검증 메시지 원문을 ID마다 한 번만 남긴다(DrainValidationMessages가 쓴다).
    mutable ValidationMessageFilter validationFilter_;
    // 마지막 실패의 API 호출 이름·원본 HRESULT다(DeviceCreateReport).
    char failureStage_[sizeof(DeviceCreateReport{}.stage)]{};
    HRESULT failureResult_ = S_OK;
    bool failureTruncated_ = false;
    // 장치 제거를 이미 남겼는지다(손실 전이 때 한 번, 원자 변수).
    uint32 lossLogged_ = 0;
    mutable Array<uint8> messageScratch_;
    D3D12FenceSource fenceSource_;
    FenceTimeline timeline_;
    // async compute queue(RG4)의 timeline·기록 저장소·timestamp 장부다(아래 compute 멤버와 함께).
    FenceTimeline computeTimeline_;
    Array<AllocatorRecord> computeAllocators_;
    TimestampRing computeTimestamps_;
    DeviceOwnership ownership_;
    Allocator& allocator_;
    // 장치 내부 잠금이다. 자원 생성·갱신이 다른 스레드에서 올 수
    // 있으므로(concurrentResourceCreation) 자원 표·descriptor·업로드 기록·회수 목록을 만지는 모든
    // 호출이 잡는다. fence API는 잡지 않는다.
    mutable platform::Mutex gate_;
    SlotArray<ReadbackRecord> readbacks_;
    UploadRingAllocator ring_;
    ComRef<ID3D12Resource> ringBuffer_;
    uint8* ringData_ = nullptr;
    // 열린 업로드 기록 list와 그 저장소다. 소유 스레드의 다음 제출이 실행한다.
    uint32 uploadAllocator_ = 0;
    bool uploadOpen_ = false;
    uint64 dedicatedUploads_ = 0;
    D3D12StagingSource stagingSource_;
    UniquePtr<StagingPool> staging_;
    // GPU 계측(GX9): timestamp query heap, resolve 대상 readback 버퍼와 프레임 slot 장부다.
    // 지원하지 않으면 heap이 없다.
    ComRef<ID3D12QueryHeap> timestampHeap_;
    ComRef<ID3D12Resource> timestampReadback_;
    D3D12TimestampSource timestampSource_;
    TimestampRing timestamps_;
    uint64 timestampFrequency_ = 0;
    // 지금 프레임에서 열린 marker 구간 수다.
    uint32 markerDepth_ = 0;
    // 기록 중인 프레임이 추적 상태를 바꾼 텍스처다(GX5). 목록에 넣지 못했으면 overflow가 켜지고
    // 모든 텍스처를 훑는다.
    Array<SlotHandle> touchedTextures_;
    bool touchedOverflow_ = false;
    // 버퍼 view(GX2)와 그리기 slot에 묶인 view, 빈 slot에 묶는 null SRV 칸이다.
    SlotArray<BufferViewRecord> bufferViews_;
    BufferViewHandle boundViews_[MaxDrawBufferViews];
    uint32 nullBufferSrv_ = 0;
    // compute 쓰기 slot에 묶인 view와 빈 slot의 null UAV, ReadWrite view의 CPU UAV 칸 pool,
    // 읽기 전용 DSV를 그때그때 쓰는 칸이다(DSV는 OMSetRenderTargets가 기록 때 읽는다).
    BufferViewHandle boundWriteViews_[MaxDrawBufferViews];
    // compute 쓰기 slot에 묶인 텍스처 쓰기 view다(버퍼 view와 같은 slot 공간, 하나만 남는다).
    TextureHandle boundWriteTextures_[MaxDrawBufferViews];
    // 단계·텍스처 slot마다 SetTexture로 묶은 텍스처 기록(null이면 흰 텍스처)과 샘플러 slot마다
    // SetSampler로 준 값의 샘플러 원본 칸(NoBoundSampler면 없음)이다(RenderBase 9.13절).
    SlotHandle boundTextures_[ConstantBufferStageCount][MaxResourceSlots] = {};
    uint32 boundSamplers_[ConstantBufferStageCount][MaxResourceSlots] = {};
    uint32 nullBufferUav_ = 0;
    DescriptorPool cpuUavs_;
    uint32 scratchDsv_ = 0;
    // 패스마다 attachment RTV를 그 mip·slice로 다시 쓰는 칸이다(RTV는 CPU descriptor라
    // OMSetRenderTargets가 기록 때 읽는다, 텍스처 GX2 C부).
    uint32 scratchRtvs_[MaxColorTargets] = {};
    // 기록의 그리기별 상수 값이다(기록 시작 때 0, compute root 상수로도 올린다).
    alignas(16) uint8 constants_[MaxDrawConstantBytes] = {};
    uint32 stencilReference_ = 0;
    Array<BufferState> bufferStates_;
    // 기록 중 열린 분리 전이 짝이다.
    SplitTransitionTracker splits_;
    // root signature 캐시다(키는 slot 표의 (단계, 종류, register) 목록과 compute 여부).
    struct RootSignatureEntry {
        explicit RootSignatureEntry(Allocator& allocator) noexcept : key(allocator) {
        }

        Array<uint8> key;
        ComRef<ID3D12RootSignature> signature;
    };

    Array<RootSignatureEntry> rootSignatures_;
    // 상수 버퍼(9.11절): handle 표, upload page, queue별 기록 명령 ring, shader-visible descriptor
    // ring(srvs_는 CPU 전용 원본 heap이다), 넘친 CBV table의 빈 칸이 가리킬 0 구간, PS 진단 버퍼.
    SlotArray<ConstantBufferRecord> constantBuffers_;
    Array<UniquePtr<ConstantPage>> constantPages_;
    TransientRings graphicsRings_;
    TransientRings computeRings_;
    ComRef<ID3D12DescriptorHeap> online_;
    uint32 onlineIncrement_ = 0;
    // Sampler table ring(queue마다 OnlineSamplersPerQueue칸)이다. samplers_는 CPU 전용 원본이다.
    ComRef<ID3D12DescriptorHeap> onlineSamplers_;
    uint32 onlineSamplerIncrement_ = 0;
    D3D12_GPU_VIRTUAL_ADDRESS zeroConstants_ = 0;
    ComRef<ID3D12Resource> debugBuffer_;
    struct BoundConstant {
        SlotHandle handle;
        // SetConstantData 자리(handle이 null일 때)다.
        D3D12_GPU_VIRTUAL_ADDRESS data = 0;
        uint32 bytes = 0;
    };
    BoundConstant boundConstants_[ConstantBufferStageCount][MaxConstantBufferSlots] = {};
    // 단계(VS·PS·CS)와 종류(SRV·UAV·Sampler — TableKind)마다 바로 앞에 복사한 table 원본 목록과 그
    // 구간이다(기록마다 비운다).
    struct TableCache {
        bool valid = false;
        uint32 count = 0;
        uint32 sources[MaxStageTextures + MaxDrawBufferViews] = {};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
    };
    TableCache tableCache_[ConstantBufferStageCount][TableKindCount] = {};
    // 단계마다 바로 앞에 만든 넘친 CBV table(range 첫 slot·칸 수·주소 목록)과 그 구간이다.
    struct CbvTableCache {
        bool valid = false;
        uint32 first = 0;
        uint32 count = 0;
        D3D12_GPU_VIRTUAL_ADDRESS addresses[MaxConstantBufferSlots] = {};
        UINT sizes[MaxConstantBufferSlots] = {};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
    };
    CbvTableCache cbvTableCache_[ConstantBufferStageCount] = {};
    bool onlineExhaustedLogged_ = false;
    // 지금 기록에 건 root signature다(기록·패스마다 비운다).
    ID3D12RootSignature* appliedGraphicsRoot_ = nullptr;
    ID3D12RootSignature* appliedComputeRoot_ = nullptr;
    // 장치 셰이더(CreateShader — 코드와 layout의 CPU 사본)다.
    DeviceShaderTable shaders_;
    bool recording_ = false;
    bool validation_ = false;
    // GPU 기반 검증이 켜졌다(DeviceDesc::enableGpuValidation). 검증 queue가 경고도 저장한다.
    bool gpuValidation_ = false;
    bool software_ = false;
    // D3D12_HEAP_FLAG_CREATE_NOT_ZEROED를 받는 런타임이다(ID3D12Device8, UE D3D12Adapter.cpp와
    // 같다).
    bool heapNotZeroed_ = false;
    // 기록 중인 프레임이 placed 자원을 활성으로 만들었다. 제출하지 못하면 모든 구간을 비활성으로
    // 돌린다(다음 획득이 aliasing barrier를 다시 낸다).
    bool placedActivated_ = false;
    // --- async compute queue(RG4) ---
    // compute queue와 그 fence·list다. 만들지 못했으면 queue가 없다.
    ComRef<ID3D12CommandQueue> computeQueue_;
    ComRef<ID3D12Fence> computeFence_;
    ComRef<ID3D12GraphicsCommandList> computeList_;
    HANDLE computeEvent_ = nullptr;
    HANDLE computeWatcherEvent_ = nullptr;
    D3D12FenceSource computeFenceSource_;
    uint32 computeRecordingAllocator_ = 0;
    // 마지막으로 신호한 compute queue fence 값이다.
    uint64 computeFenceValue_ = 0;
    ComRef<ID3D12QueryHeap> computeTimestampHeap_;
    ComRef<ID3D12Resource> computeTimestampReadback_;
    D3D12TimestampSource computeTimestampSource_;
    uint64 computeTimestampFrequency_ = 0;
    // 지금 기록이 compute queue 기록이다.
    bool computeRecording_ = false;
};

} // namespace rocks::graphics::d3d12
