#pragma once

#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <D3D12Graphics/D3D12Device.h>
#include <Graphics/DeviceOwnership.h>
#include <Graphics/FenceTimeline.h>
#include <Graphics/GraphicsLog.h>
#include <Graphics/ShaderSourceCompiler.h>
#include <Graphics/StagingPool.h>
#include <Graphics/TimestampRing.h>
#include <Graphics/UploadRing.h>
#include <Platform/Fatal.h>
#include <Platform/Win64/ComRef.h>

#include <Windows.h>
#include <d3d12.h>
#include <d3d12shader.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <Platform/Atomics.h>

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
constexpr uint32 ConstantCount = sizeof(DrawConstants) / sizeof(uint32);
// 스왑체인 하나가 파괴될 때 retire 목록에 넣는 항목 수(스왑체인 + 버퍼마다 하나)다.
constexpr usize SwapChainRetireEntries = 1 + SwapChainBufferCount;
// 소멸 때까지 놓지 않을 retire 항목의 fence 값이다. 정상 동작 중의 완료 값은 이 값에 닿지
// 않는다.
constexpr uint64 RetainUntilDestructionFence = NumericLimits<uint64>::Max();
// dxgidebug.h의 DXGI_DEBUG_DXGI다(dxguid.lib 없이 쓰려고 값을 직접 둔다).
constexpr GUID DxgiDebugDxgi = {
    0x25cddaa4, 0xb1c6, 0x47e1, {0xac, 0x3e, 0x98, 0x87, 0x5b, 0x5a, 0x2e, 0x2a}};
static_assert(ConstantCount == 20, "DrawConstants must fit in 20 root constants");
// root signature 매개변수 번호다: 그리기 상수(b0), PS 텍스처(t0)·샘플러(s0) table, 버퍼 view slot
// table(t1–t4, 단계 모두, PipelineDesc 고정 배치).
constexpr UINT RootConstants = 0;
constexpr UINT RootTexture = 1;
constexpr UINT RootSampler = 2;
constexpr UINT RootBufferViews = 3;

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

D3D12_FILTER SamplerFilter(Filter filter) noexcept;

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
};

// 버퍼 view(GX2)다. 버퍼 자원 참조를 쥐므로 버퍼를 먼저 놓아도 산다. descriptor는 shader-visible
// SRV heap(CBV_SRV_UAV)의 칸이다(UAV view도 같은 heap, compute 묶기는 GX3·GX4).
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
};

struct TextureRecord {
    ComRef<ID3D12Resource> resource;
    uint32 width = 0;
    uint32 height = 0;
    uint32 mipLevels = 1;
    // view(CreateTextureView)면 view가 읽는 형식, 아니면 텍스처 형식이다.
    PixelFormat format = PixelFormat::Unknown;
    bool renderTarget = false;
    // 깊이·스텐실 타깃(TextureUsage::DepthStencil)이다.
    bool depthTarget = false;
    // 다른 텍스처의 자원을 공유하는 view다. 업로드·readback·그리기 대상이 될 수 없다. 자원 상태는
    // 원 텍스처 기록(source)이 추적한다.
    bool view = false;
    SlotHandle source;
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
    // mip마다 장치가 추적한 상태다(RenderBase GX5). committed는 제출한 프레임 기록 끝의 상태이고,
    // current는 기록 중인 프레임 안의 상태다(기록 중이 아니면 둘이 같다). 업로드 기록은 프레임
    // 기록보다 먼저 실행되므로 committed에서 바꿨다가 committed로 되돌려 놓는다(상태 중립).
    D3D12_RESOURCE_STATES committed[MaxMipLevels] = {};
    D3D12_RESOURCE_STATES current[MaxMipLevels] = {};
    // 기록 중인 프레임이 current를 바꿨다(touchedTextures_에 있다).
    bool touched = false;
};

struct PipelineRecord {
    ComRef<ID3D12PipelineState> state;
    // stream별 stride와 배치가 선언한 stream mask다(그리기 때 묶인 정점 버퍼를 확인한다).
    uint32 strides[MaxVertexStreams] = {};
    uint32 streamMask = 0;
    // VS·PS(compute면 CS)가 읽는 버퍼 view slot과 CS가 쓰는 slot이다(리플렉션).
    DrawBufferSlots bufferSlots;
    DrawBufferSlots writeSlots;
    // compute 파이프라인이다(Dispatch만 쓴다).
    bool compute = false;
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    // 깊이·스텐실을 쓴다(읽기 전용 깊이 attachment에서 그리기를 거른다).
    bool writesDepthStencil = false;
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
    int64 srv = -1;
    int64 rtv = -1;
    int64 dsv = -1;
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

    // 다른 스레드에서도 부를 수 있다(concurrentResourceCreation). descriptor는 shader-visible SRV
    // heap 칸 하나다(SRV·UAV 모두, GX2).
    Result<BufferViewHandle> CreateBufferView(BufferHandle handle,
                                              const BufferViewDesc& desc) noexcept override;

    // 버퍼 구간을 동기로 읽는다(시험·캡처용). 업로드 기록 뒤에 복사를 붙여 앞선 갱신 뒤의 내용을
    // 본다. 버퍼는 COMMON에서 복사 원본으로 암묵 승격된다.
    Result<void> ReadBuffer(BufferHandle handle, uint64 offset, Span<uint8> out) noexcept override;

    // 다른 스레드에서도 부를 수 있다(concurrentResourceCreation). 초기 픽셀은 업로드 기록으로 간다.
    // 형식·용도 지원은 장치 능력(GetPixelFormatCaps)으로 판정한다(PixelFormat.ko.md 8.3절).
    Result<TextureHandle> CreateTexture(const TextureDesc& desc,
                                        Span<const uint8> pixels) noexcept override;

    // 기본 SRV(형식이 Sampled일 때, plane 0)·RTV·DSV를 만든다(잠금 안). 실패하면 이미 잡은 칸을
    // 놓는다.
    Result<void> CreateTextureViews(TextureRecord& record, const d3d12::FormatMapping& mapping,
                                    bool sampled) noexcept;

    // 다 만든 텍스처 기록의 descriptor 칸을 바로 놓는다(잠금 안, 아직 어떤 명령도 참조하지 않을
    // 때).
    void ReleaseTextureViews(const TextureRecord& record) noexcept;

    // 2D texture SRV를 쓴다. plane은 깊이·스텐실·비디오 plane 선택이다.
    void WriteShaderResourceView(ID3D12Resource* resource, DXGI_FORMAT format, uint32 plane,
                                 uint32 mipLevels, uint32 slot) noexcept;

    // 기록을 표에 넣고 handle을 돌려준다(잠금 안). 실패하면 descriptor 칸을 놓는다.
    Result<TextureHandle> AddTextureRecord(TextureRecord&& record) noexcept;

    PixelFormatCaps FormatCaps(PixelFormat format) const noexcept;

    PixelFormatCaps GetPixelFormatCaps(PixelFormat format) const noexcept override;

    // 원 텍스처의 자원을 공유하는 SRV view다(PixelFormat.ko.md 9–11절). 다른 스레드에서도 부를 수
    // 있다.
    Result<TextureHandle> CreateTextureView(TextureHandle texture,
                                            const TextureViewDesc& desc) noexcept override;

    // 텍스처 mip·plane 하나를 동기로 읽는다(시험·캡처용). ReadPixels와 같은 경로다.
    Result<void> ReadTexture(TextureHandle handle, uint32 mipLevel, uint32 plane,
                             Span<uint8> out) noexcept override;

    // 다른 스레드에서도 부를 수 있다(concurrentResourceCreation, 내부 잠금으로 직렬화한다).
    Result<PipelineHandle> CreatePipeline(const PipelineDesc& desc) noexcept override;
    Result<PipelineHandle> CreateComputePipeline(const ComputePipelineDesc& desc) noexcept override;

    Result<CommandContext*> BeginFrame(SwapChainHandle handle,
                                       const ClearColor& clear) noexcept override;

    Result<CommandContext*> BeginPass(const PassDesc& pass) noexcept override;

    Result<GpuFencePoint> EndFrame() noexcept override;

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

    void Retire(TextureHandle handle) noexcept override;

    // 텍스처 기록 하나를 회수 대기로 넘기고 표에서 뺀다(잠금 안).
    void RetireTextureRecord(SlotHandle slot, TextureRecord& record) noexcept;

    void Retire(PipelineHandle handle) noexcept override;

    void Retire(SwapChainHandle handle) noexcept override;

    void ReclaimCompleted() noexcept override;

    Result<void> WaitIdle() noexcept override;

    Result<void> WaitIdleLocked() noexcept;

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

    usize CollectTimestamps(Span<TimestampSample> out) noexcept override;

    Result<ClockCalibration> CalibrateClocks() noexcept override;

    uint64 ValidationErrorCount() const noexcept override;

private:
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
    // 실패한다(PixelFormat.ko.md 8.3절). 호출자는 gate_를 잡고 있어야 한다.
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

    // 컴파일한 VS 입력 signature를 배치와 대조한다(RenderBase 10.4절 셰이더 입력 대조). 사용자
    // 입력은 시맨틱 ATTRIBUTEn만 받고, n이 배치에 있어야 하며 성분 타입 계열이 형식의 셰이더 타입과
    // 같아야 한다. 셰이더가 읽지 않는 속성은 허용한다.
    Result<void> MatchVertexInputs(ID3DBlob* code, const VertexLayout& layout) noexcept;

    // 셰이더가 선언한 자원이 고정 배치(PipelineDesc) 안인지 보고 버퍼 view slot 배치를 slots에
    // 더한다. b0 상수, PS t0 텍스처·s0 샘플러, t1–t4 버퍼 SRV만 받는다. 두 단계가 같은 slot을 다른
    // 종류로 읽거나 배치 밖 자원(UAV 포함)이면 InvalidArgument다.
    Result<void> ReflectBindings(ID3DBlob* code, ShaderStage stage, DrawBufferSlots& slots,
                                 DrawBufferSlots* writeSlots) noexcept;

    Result<void> Reject(const Diagnostic& message) noexcept;

    // 묶인 정점 버퍼 하나를 지금 파이프라인의 stride로 IA에 건다.
    void ApplyVertexBuffer(uint32 stream) noexcept;

    // 묶인 파이프라인이 선언한 stream에 모두 유효한 버퍼가 있는지다.
    bool StreamsBound() const noexcept;

    Result<void> CreateRootSignature() noexcept;
    // compute root signature와 간접 명령 signature를 만든다(D3D12Commands.cpp).
    Result<void> CreateComputeObjects() noexcept;

    Result<void> Compile(StringView source, const char* entry, const char* target,
                         ComRef<ID3DBlob>& code) noexcept;

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
    void CopyTextureFrom(TextureRecord& record, uint32 mipLevel, uint32 subresource,
                         ID3D12Resource* source,
                         const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                         bool restore) noexcept;

    // 빈틈 없는 행의 픽셀(mip firstMip부터 count개, mip 안은 plane 순서)을 장치 행 정렬로 ring에
    // 옮기고 복사를 기록한다. 블록 압축 형식의 행은 블록 행이다.
    Result<void> RecordTextureUpload(TextureRecord& record, uint32 firstMip, uint32 count,
                                     Span<const uint8> pixels, bool restore) noexcept;

    // 쌓인 업로드 기록을 제출하고 그 fence 값을 돌려준다(잠금 안, 소유 스레드). 없으면 0이다.
    Result<uint64> FlushUploadsLocked() noexcept;

    void AssignPendingUploads(uint64 value) noexcept;

    // 텍스처 mip 하나의 plane 하나를 readback 버퍼로 복사하는 기록을 쌓고 바로 제출한다(잠금 안).
    // requireRenderTarget이면 RenderTarget 텍스처만 받는다(ReadPixels·RequestReadback 계약).
    Result<ReadbackRequest> RequestReadbackLocked(TextureHandle target, uint32 mipLevel = 0,
                                                  uint32 plane = 0,
                                                  bool requireRenderTarget = true) noexcept;

    Result<bool> TryReadbackLocked(ReadbackHandle handle, Span<uint8> out) noexcept;

    void ReleaseReadbackLocked(ReadbackHandle handle) noexcept;

    void KeepUntilDestruction(ComRef<ID3D12Resource>&& object) noexcept;

    void ConfigureValidationQueues() noexcept;

    // 저장된 검증 메시지 중 오류를 세어 누적하고 비운다. 메시지 길이에 상한을 두지 않는다.
    void DrainValidationMessages() const noexcept;

    // 메시지 구조체를 담을 16 byte 정렬 버퍼다. 할당 실패면 null이다.
    void* Scratch(SIZE_T length) const noexcept;

    // 모든 D3D 인터페이스는 IUnknown을 단일 상속하므로 포인터 변환이 안전하다.
    template <class T>
    void Defer(ComRef<T>&& object, int64 srv, int64 rtv, int64 dsv = -1) noexcept {
        ComRef<IUnknown> unknown;
        unknown.Attach(object.Detach());
        RetireObject(Move(unknown), srv, rtv, dsv);
    }

    void RetireObject(ComRef<IUnknown>&& object, int64 srv, int64 rtv, int64 dsv = -1) noexcept;

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

    void SetTexture(TextureHandle handle) noexcept override;

    void SetConstants(const DrawConstants& constants) noexcept override;

    void SetBufferView(uint32 slot, BufferViewHandle handle) noexcept override;

    // --- GX3·GX4(D3D12Commands.cpp) ---

    void SetWritableBufferView(uint32 slot, BufferViewHandle view) noexcept override;

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

    // 버퍼 전이다. 기록 중 추적 상태(RequireBufferState)로 COMMON 암묵 승격·읽기 누적·barrier를
    // 고르고, UAV → UAV는 UAV barrier다(GX5).
    void LowerBufferTransition(const ResourceTransition& transition) noexcept;

    // 기록 중 프레임에서 텍스처 mip [first, first + count)를 after로 바꾼다(잠금 안). 바뀌는 mip만
    // barrier를 내고, 모든 mip이 같은 상태에서 바뀌면 한 barrier로 낸다. uavOrder면 이미 같은 UAV
    // 상태인 mip에 UAV barrier를 낸다.
    void TrackTransition(TextureRecord& record, SlotHandle slot, uint32 first, uint32 count,
                         D3D12_RESOURCE_STATES after, bool uavOrder = false) noexcept;

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
    ComRef<IDXGIFactory4> factory_;
    ComRef<ID3D12Device> device_;
    ComRef<ID3D12InfoQueue> infoQueue_;
    ComRef<ID3D12CommandQueue> queue_;
    ComRef<ID3D12GraphicsCommandList> list_;
    ComRef<ID3D12GraphicsCommandList> uploadList_;
    ComRef<ID3D12RootSignature> rootSignature_;
    // compute root signature(b0 상수, t1–t4 SRV table, u0–u3 UAV table)와 간접 명령 signature다.
    ComRef<ID3D12RootSignature> computeRootSignature_;
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
    SwapChainHandle presentTarget_;
    TextureHandle white_;
    SlotHandle boundPipeline_;
    BoundVertexBuffer boundVertex_[MaxVertexStreams];
    BoundIndexBuffer boundIndex_;
    uint64 ignoredDraws_ = 0;
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
    uint32 nullBufferUav_ = 0;
    DescriptorPool cpuUavs_;
    uint32 scratchDsv_ = 0;
    // 마지막 SetConstants 값이다(compute root 상수로도 올린다).
    DrawConstants constants_;
    uint32 stencilReference_ = 0;
    Array<BufferState> bufferStates_;
    bool recording_ = false;
    bool validation_ = false;
    bool software_ = false;
};

} // namespace rocks::graphics::d3d12
