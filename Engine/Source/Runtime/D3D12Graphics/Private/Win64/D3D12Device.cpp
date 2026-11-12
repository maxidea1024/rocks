#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <D3D12Graphics/D3D12Device.h>
#include <Platform/Fatal.h>

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <Platform/Memory.h>

namespace rocks::graphics {

namespace {

constexpr uint32 FrameCount = 2;
constexpr uint32 SwapChainBufferCount = 2;
constexpr uint32 RtvCapacity = 128;
constexpr uint32 SrvCapacity = 1024;
// 서로 다른 SamplerDesc 조합 수의 상한이다(필터 4 × 주소 9 × 이방성 16을 넘지 않는다).
constexpr uint32 SamplerCapacity = 256;
constexpr uint32 MaxMipLevels = 16;
constexpr uint32 ConstantCount = sizeof(DrawConstants) / sizeof(uint32);
// 스왑체인 하나가 파괴될 때 retire 목록에 넣는 항목 수(스왑체인 + 버퍼마다 하나)다.
constexpr usize SwapChainRetireEntries = 1 + SwapChainBufferCount;
// dxgidebug.h의 DXGI_DEBUG_DXGI다(dxguid.lib 없이 쓰려고 값을 직접 둔다).
constexpr GUID DxgiDebugDxgi = {
    0x25cddaa4, 0xb1c6, 0x47e1, {0xac, 0x3e, 0x98, 0x87, 0x5b, 0x5a, 0x2e, 0x2a}};
static_assert(ConstantCount == 20, "DrawConstants must fit in 20 root constants");

// COM 객체 참조다. 소멸 시 Release한다(예외 없이 쓰는 최소 RAII).
template <class T>
class ComRef {
public:
    ComRef() noexcept = default;

    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;

    ComRef(ComRef&& other) noexcept : pointer_(other.pointer_) {
        other.pointer_ = nullptr;
    }

    ComRef& operator=(ComRef&& other) noexcept {
        if (this != &other) {
            Reset();
            pointer_ = other.pointer_;
            other.pointer_ = nullptr;
        }
        return *this;
    }

    ~ComRef() {
        Reset();
    }

    void Reset() noexcept {
        if (pointer_ != nullptr) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

    T* Get() const noexcept {
        return pointer_;
    }

    T* operator->() const noexcept {
        return pointer_;
    }

    // 출력 인자로 넘길 주소다. 기존 참조는 먼저 해제한다.
    T** Out() noexcept {
        Reset();
        return &pointer_;
    }

    // 소유권을 넘겨준다(참조 수는 그대로다).
    T* Detach() noexcept {
        T* pointer = pointer_;
        pointer_ = nullptr;
        return pointer;
    }

    // 이미 참조를 하나 가진 포인터의 소유를 넘겨받는다.
    void Attach(T* pointer) noexcept {
        Reset();
        pointer_ = pointer;
    }

    explicit operator bool() const noexcept {
        return pointer_ != nullptr;
    }

private:
    T* pointer_ = nullptr;
};

DXGI_FORMAT ResourceFormat(Format format) noexcept {
    switch (format) {
    case Format::RGBA8Unorm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8UnormSrgb:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case Format::Unknown:
        break;
    }
    return DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT AttributeFormat(VertexFormat format) noexcept {
    switch (format) {
    case VertexFormat::Float2:
        return DXGI_FORMAT_R32G32_FLOAT;
    case VertexFormat::Float3:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case VertexFormat::Float4:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

D3D12_FILTER SamplerFilter(Filter filter) noexcept {
    switch (filter) {
    case Filter::Nearest:
        return D3D12_FILTER_MIN_MAG_MIP_POINT;
    case Filter::Bilinear:
        return D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    case Filter::Trilinear:
        return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    case Filter::Anisotropic:
        return D3D12_FILTER_ANISOTROPIC;
    }
    return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
}

D3D12_TEXTURE_ADDRESS_MODE SamplerAddress(AddressMode mode) noexcept {
    switch (mode) {
    case AddressMode::Wrap:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case AddressMode::Clamp:
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case AddressMode::Mirror:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case AddressMode::Border:
        return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
    return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type) noexcept {
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC BufferResource(uint64 size) noexcept {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

struct BufferRecord {
    ComRef<ID3D12Resource> resource;
    D3D12_VERTEX_BUFFER_VIEW view = {};
};

struct TextureRecord {
    ComRef<ID3D12Resource> resource;
    uint32 width = 0;
    uint32 height = 0;
    uint32 mipLevels = 1;
    Format format = Format::Unknown;
    bool renderTarget = false;
    uint32 srv = 0;
    uint32 rtv = 0;
    // 샘플러 힙의 칸이다(SamplerFor가 같은 설정끼리 공유한다).
    uint32 sampler = 0;
};

struct PipelineRecord {
    ComRef<ID3D12PipelineState> state;
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
    // 기록 중인(아직 제출하지 않은) 프레임이 쓸 수 있는 자원이다. EndFrame이 그 프레임의 signal
    // 값을 정할 때까지 해제하지 않는다.
    bool pending = false;
    int64 srv = -1;
    int64 rtv = -1;
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

class D3D12Device final : public Device, private CommandContext {
public:
    explicit D3D12Device(Allocator& allocator) noexcept
        : adapterName_(allocator), lastError_(allocator), buffers_(allocator), textures_(allocator),
          pipelines_(allocator), swapChains_(allocator), retired_(allocator), rtvs_(allocator),
          srvs_(allocator), samplers_(allocator), messageScratch_(allocator) {
    }

    ~D3D12Device() override {
        if (queue_ && fence_ && fenceEvent_ != nullptr) {
            auto idle = WaitIdle();
            (void)idle;
        }
        retired_.Clear();
        swapChains_.Clear();
        textures_.Clear();
        pipelines_.Clear();
        buffers_.Clear();
        if (fenceEvent_ != nullptr) {
            CloseHandle(fenceEvent_);
        }
    }

    Result<void> Initialize(const DeviceDesc& desc) noexcept {
        UINT factoryFlags = 0;
        if (desc.enableValidation) {
            ComRef<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.Out())))) {
                debug->EnableDebugLayer();
                validation_ = true;
                factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            }
        }

        if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(factory_.Out())))) {
            return Fail("CreateDXGIFactory2 failed");
        }

        ComRef<IDXGIAdapter1> adapter;
        if (!desc.preferSoftware) {
            ComRef<IDXGIFactory6> factory6;
            if (SUCCEEDED(factory_->QueryInterface(IID_PPV_ARGS(factory6.Out())))) {
                for (UINT index = 0;; ++index) {
                    ComRef<IDXGIAdapter1> candidate;
                    if (FAILED(factory6->EnumAdapterByGpuPreference(
                            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                            IID_PPV_ARGS(candidate.Out())))) {
                        break;
                    }
                    DXGI_ADAPTER_DESC1 info = {};
                    candidate->GetDesc1(&info);
                    if ((info.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                        continue;
                    }
                    if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                                                    IID_PPV_ARGS(device_.Out())))) {
                        adapter = Move(candidate);
                        break;
                    }
                }
            }
        }

        if (!device_) {
            if (FAILED(factory_->EnumWarpAdapter(IID_PPV_ARGS(adapter.Out()))) ||
                FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                         IID_PPV_ARGS(device_.Out())))) {
                return Fail("D3D12CreateDevice failed for hardware and WARP adapters");
            }
            software_ = true;
        }

        DXGI_ADAPTER_DESC1 info = {};
        adapter->GetDesc1(&info);
        // UTF-16 128자(Description 크기)는 UTF-8로 최대 384 byte다.
        char name[512] = {};
        const int written = WideCharToMultiByte(CP_UTF8, 0, info.Description, -1, name,
                                                static_cast<int>(sizeof(name)), nullptr, nullptr);
        if (written > 1) {
            auto assigned =
                adapterName_.TryAssign(StringView(name, static_cast<usize>(written - 1)));
            (void)assigned;
        }

        if (validation_) {
            ConfigureValidationQueues();
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue_.Out())))) {
            return Fail("CreateCommandQueue failed");
        }

        for (Frame& frame : frames_) {
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(frame.allocator.Out())))) {
                return Fail("CreateCommandAllocator failed");
            }
        }
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(uploadAllocator_.Out()))) ||
            FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              frames_[0].allocator.Get(), nullptr,
                                              IID_PPV_ARGS(list_.Out()))) ||
            FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              uploadAllocator_.Get(), nullptr,
                                              IID_PPV_ARGS(uploadList_.Out())))) {
            return Fail("CreateCommandList failed");
        }
        list_->Close();
        uploadList_->Close();

        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.Out())))) {
            return Fail("CreateFence failed");
        }
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fenceEvent_ == nullptr) {
            return Fail("CreateEvent failed");
        }

        auto rtvs =
            rtvs_.Initialize(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RtvCapacity, false);
        auto srvs = srvs_.Initialize(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                     SrvCapacity, true);
        auto samplers = samplers_.Initialize(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                             SamplerCapacity, true);
        if (!rtvs || !srvs || !samplers) {
            return Fail("descriptor heap creation failed");
        }

        auto signature = CreateRootSignature();
        if (!signature) {
            return signature;
        }

        // 텍스처를 묶지 않은 그리기가 쓸 1x1 흰색 텍스처다.
        const uint8 white[4] = {255, 255, 255, 255};
        TextureDesc whiteDesc;
        whiteDesc.width = 1;
        whiteDesc.height = 1;
        whiteDesc.format = Format::RGBA8Unorm;
        auto whiteTexture = CreateTexture(whiteDesc, Span<const uint8>(white, 4));
        if (!whiteTexture) {
            return Result<void>::Failure(whiteTexture.Error());
        }
        white_ = whiteTexture.Value();
        return Result<void>::Success();
    }

    DeviceInfo Info() const noexcept override {
        DeviceInfo info;
        info.backend = Backend::D3D12;
        info.adapterName = adapterName_.View();
        info.validationEnabled = validation_;
        info.software = software_;
        return info;
    }

    Result<SwapChainHandle> CreateSwapChain(const SwapChainDesc& desc) noexcept override {
        if (desc.surface.window == nullptr || desc.width == 0 || desc.height == 0) {
            return Result<SwapChainHandle>::Failure(ErrorCode::InvalidArgument);
        }

        DXGI_SWAP_CHAIN_DESC1 swapDesc = {};
        swapDesc.Width = desc.width;
        swapDesc.Height = desc.height;
        // flip-model 버퍼는 _SRGB 형식을 쓸 수 없다. RTV를 sRGB로 만들어 쓰기 시 인코딩한다.
        swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = SwapChainBufferCount;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        const HWND window = static_cast<HWND>(desc.surface.window);
        ComRef<IDXGISwapChain1> swapChain1;
        if (FAILED(factory_->CreateSwapChainForHwnd(queue_.Get(), window, &swapDesc, nullptr,
                                                    nullptr, swapChain1.Out()))) {
            return FailWith<SwapChainHandle>("CreateSwapChainForHwnd failed");
        }
        factory_->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

        SwapChainRecord record;
        if (FAILED(swapChain1->QueryInterface(IID_PPV_ARGS(record.swapChain.Out())))) {
            return FailWith<SwapChainHandle>("IDXGISwapChain3 is not available");
        }
        record.width = desc.width;
        record.height = desc.height;
        record.vsync = desc.vsync;
        auto reserved = ReserveRetireCapacity(SwapChainRetireEntries);
        if (!reserved) {
            return Result<SwapChainHandle>::Failure(reserved.Error());
        }
        uint32 allocatedViews = 0;
        for (uint32& rtv : record.rtv) {
            auto allocated = rtvs_.Allocate();
            if (!allocated) {
                for (uint32 index = 0; index < allocatedViews; ++index) {
                    rtvs_.Free(record.rtv[index]);
                }
                return FailWith<SwapChainHandle>("render target descriptor heap is full");
            }
            rtv = allocated.Value();
            ++allocatedViews;
        }
        auto views = CreateSwapChainViews(record);
        if (!views) {
            for (uint32 rtv : record.rtv) {
                rtvs_.Free(rtv);
            }
            return Result<SwapChainHandle>::Failure(views.Error());
        }

        uint32 rtvIndices[SwapChainBufferCount];
        for (uint32 index = 0; index < SwapChainBufferCount; ++index) {
            rtvIndices[index] = record.rtv[index];
        }
        auto added = swapChains_.TryAdd(Move(record));
        if (!added) {
            for (uint32 rtv : rtvIndices) {
                rtvs_.Free(rtv);
            }
            return Result<SwapChainHandle>::Failure(added.Error());
        }
        return Result<SwapChainHandle>::Success(SwapChainHandle{added.Value()});
    }

    Result<void> ResizeSwapChain(SwapChainHandle handle, uint32 width,
                                 uint32 height) noexcept override {
        SwapChainRecord* record = swapChains_.Find(handle.slot);
        if (record == nullptr) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }
        if (width == 0 || height == 0 || recording_) {
            return Result<void>::Failure(recording_ ? ErrorCode::Busy : ErrorCode::InvalidArgument);
        }

        // 버퍼를 참조하는 모든 GPU 작업이 끝난 뒤 버퍼를 놓고 크기를 바꾼다.
        auto idle = WaitIdle();
        if (!idle) {
            return idle;
        }
        for (ComRef<ID3D12Resource>& buffer : record->buffers) {
            buffer.Reset();
        }
        // 이 시점부터 버퍼가 없으므로 성공할 때까지 그리기를 막는다.
        record->broken = true;
        if (FAILED(record->swapChain->ResizeBuffers(SwapChainBufferCount, width, height,
                                                    DXGI_FORMAT_R8G8B8A8_UNORM, 0))) {
            return Fail("ResizeBuffers failed");
        }
        auto views = CreateSwapChainViews(*record);
        if (!views) {
            for (ComRef<ID3D12Resource>& buffer : record->buffers) {
                buffer.Reset();
            }
            return views;
        }
        record->width = width;
        record->height = height;
        record->broken = false;
        return Result<void>::Success();
    }

    Result<BufferHandle> CreateBuffer(const BufferDesc& desc,
                                      Span<const uint8> initial) noexcept override {
        if (desc.size == 0 || initial.Size() != desc.size) {
            return Result<BufferHandle>::Failure(ErrorCode::InvalidArgument);
        }

        BufferRecord record;
        const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC resource = BufferResource(desc.size);
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(record.resource.Out())))) {
            return FailWith<BufferHandle>("vertex buffer creation failed");
        }

        void* mapped = nullptr;
        const D3D12_RANGE noRead = {0, 0};
        if (FAILED(record.resource->Map(0, &noRead, &mapped))) {
            return FailWith<BufferHandle>("vertex buffer map failed");
        }
        platform::MemoryCopy(mapped, initial.Data(), desc.size);
        record.resource->Unmap(0, nullptr);
        record.view.BufferLocation = record.resource->GetGPUVirtualAddress();
        record.view.SizeInBytes = static_cast<UINT>(desc.size);
        record.view.StrideInBytes = 0;

        auto reserved = ReserveRetireCapacity(1);
        if (!reserved) {
            return Result<BufferHandle>::Failure(reserved.Error());
        }
        auto added = buffers_.TryAdd(Move(record));
        if (!added) {
            return Result<BufferHandle>::Failure(added.Error());
        }
        return Result<BufferHandle>::Success(BufferHandle{added.Value()});
    }

    Result<TextureHandle> CreateTexture(const TextureDesc& desc,
                                        Span<const uint8> pixels) noexcept override {
        const bool renderTarget = desc.usage == TextureUsage::RenderTarget;
        const uint64 size = MipChainBytes(desc.width, desc.height, desc.mipLevels, desc.format);
        if (desc.width > 16384 || desc.height > 16384 || size == 0 ||
            desc.mipLevels > MaxMipLevels || !IsValidSamplerDesc(desc.sampler) ||
            (renderTarget ? (pixels.Size() != 0 || desc.mipLevels != 1) : pixels.Size() != size)) {
            return Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
        }
        auto sampler = SamplerFor(desc.sampler);
        if (!sampler) {
            return FailWith<TextureHandle>("sampler descriptor heap is full");
        }

        TextureRecord record;
        record.width = desc.width;
        record.height = desc.height;
        record.mipLevels = desc.mipLevels;
        record.format = desc.format;
        record.renderTarget = renderTarget;
        record.sampler = sampler.Value();
        D3D12_RESOURCE_DESC resource = {};
        resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resource.Width = desc.width;
        resource.Height = desc.height;
        resource.DepthOrArraySize = 1;
        resource.MipLevels = static_cast<UINT16>(desc.mipLevels);
        resource.Format = ResourceFormat(desc.format);
        resource.SampleDesc.Count = 1;
        resource.Flags =
            renderTarget ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
        const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        // 최적화 clear 값은 두지 않는다. 패스마다 clear 색이 다르므로 값을 두면 매번 불일치 경고가
        // 쌓인다.
        const D3D12_RESOURCE_STATES initialState = renderTarget
                                                       ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                                       : D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource,
                                                    initialState, nullptr,
                                                    IID_PPV_ARGS(record.resource.Out())))) {
            return FailWith<TextureHandle>("texture creation failed");
        }

        auto reserved = ReserveRetireCapacity(1);
        if (!reserved) {
            return Result<TextureHandle>::Failure(reserved.Error());
        }
        if (!renderTarget) {
            auto uploaded = UploadTexture(record, pixels);
            if (!uploaded) {
                return Result<TextureHandle>::Failure(uploaded.Error());
            }
        }

        auto srv = srvs_.Allocate();
        if (!srv) {
            return FailWith<TextureHandle>("shader resource descriptor heap is full");
        }
        record.srv = srv.Value();
        device_->CreateShaderResourceView(record.resource.Get(), nullptr, srvs_.Cpu(record.srv));
        if (renderTarget) {
            auto rtv = rtvs_.Allocate();
            if (!rtv) {
                srvs_.Free(record.srv);
                return FailWith<TextureHandle>("render target descriptor heap is full");
            }
            record.rtv = rtv.Value();
            device_->CreateRenderTargetView(record.resource.Get(), nullptr, rtvs_.Cpu(record.rtv));
        }

        const uint32 srvIndex = record.srv;
        const int64 rtvIndex = renderTarget ? static_cast<int64>(record.rtv) : -1;
        auto added = textures_.TryAdd(Move(record));
        if (!added) {
            // 아직 어떤 명령도 이 텍스처를 참조하지 않으므로 바로 놓는다.
            srvs_.Free(srvIndex);
            if (rtvIndex >= 0) {
                rtvs_.Free(static_cast<uint32>(rtvIndex));
            }
            return Result<TextureHandle>::Failure(added.Error());
        }
        return Result<TextureHandle>::Success(TextureHandle{added.Value()});
    }

    Result<PipelineHandle> CreatePipeline(const PipelineDesc& desc) noexcept override {
        if (desc.vertex.hlsl.ByteCount() == 0 || desc.pixel.hlsl.ByteCount() == 0 ||
            desc.vertexStride == 0 || desc.attributes.Size() == 0 || desc.attributes.Size() > 8 ||
            ResourceFormat(desc.colorFormat) == DXGI_FORMAT_UNKNOWN) {
            return Result<PipelineHandle>::Failure(ErrorCode::InvalidArgument);
        }

        ComRef<ID3DBlob> vertexCode;
        ComRef<ID3DBlob> pixelCode;
        auto vertex = Compile(desc.vertex.hlsl, "VSMain", "vs_5_1", vertexCode);
        if (!vertex) {
            return Result<PipelineHandle>::Failure(vertex.Error());
        }
        auto pixel = Compile(desc.pixel.hlsl, "PSMain", "ps_5_1", pixelCode);
        if (!pixel) {
            return Result<PipelineHandle>::Failure(pixel.Error());
        }

        D3D12_INPUT_ELEMENT_DESC elements[8] = {};
        for (usize index = 0; index < desc.attributes.Size(); ++index) {
            const VertexAttribute& attribute = desc.attributes[index];
            elements[index].SemanticName = "TEXCOORD";
            elements[index].SemanticIndex = attribute.location;
            elements[index].Format = AttributeFormat(attribute.format);
            elements[index].InputSlot = 0;
            elements[index].AlignedByteOffset = attribute.offset;
            elements[index].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC state = {};
        state.pRootSignature = rootSignature_.Get();
        state.VS = {vertexCode->GetBufferPointer(), vertexCode->GetBufferSize()};
        state.PS = {pixelCode->GetBufferPointer(), pixelCode->GetBufferSize()};
        D3D12_RENDER_TARGET_BLEND_DESC& blend = state.BlendState.RenderTarget[0];
        blend.BlendEnable = desc.alphaBlend ? TRUE : FALSE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        state.SampleMask = 0xffffffffu;
        state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        state.RasterizerState.DepthClipEnable = TRUE;
        state.DepthStencilState.DepthEnable = FALSE;
        state.DepthStencilState.StencilEnable = FALSE;
        state.InputLayout = {elements, static_cast<UINT>(desc.attributes.Size())};
        state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        state.NumRenderTargets = 1;
        state.RTVFormats[0] = ResourceFormat(desc.colorFormat);
        state.SampleDesc.Count = 1;

        PipelineRecord record;
        if (FAILED(
                device_->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(record.state.Out())))) {
            return FailWith<PipelineHandle>("CreateGraphicsPipelineState failed");
        }
        auto reserved = ReserveRetireCapacity(1);
        if (!reserved) {
            return Result<PipelineHandle>::Failure(reserved.Error());
        }
        auto added = pipelines_.TryAdd(Move(record));
        if (!added) {
            return Result<PipelineHandle>::Failure(added.Error());
        }
        // 정점 간격은 파이프라인마다 다를 수 있으므로 handle과 함께 기억한다.
        auto stride = StrideFor(added.Value(), desc.vertexStride);
        if (!stride) {
            auto removed = pipelines_.TryRemove(added.Value());
            (void)removed;
            return Result<PipelineHandle>::Failure(stride.Error());
        }
        return Result<PipelineHandle>::Success(PipelineHandle{added.Value()});
    }

    Result<CommandContext*> BeginFrame(SwapChainHandle handle,
                                       const ClearColor& clear) noexcept override {
        SwapChainRecord* record = swapChains_.Find(handle.slot);
        if (record == nullptr) {
            return Result<CommandContext*>::Failure(ErrorCode::StaleHandle);
        }
        if (recording_) {
            return Result<CommandContext*>::Failure(ErrorCode::Busy);
        }
        if (record->broken) {
            return FailWith<CommandContext*>("swap chain has no buffers after a failed resize");
        }

        const uint32 backBuffer = record->swapChain->GetCurrentBackBufferIndex();
        auto begun = BeginRecording();
        if (!begun) {
            return Result<CommandContext*>::Failure(begun.Error());
        }
        target_ = {record->buffers[backBuffer].Get(), D3D12_RESOURCE_STATE_PRESENT,
                   rtvs_.Cpu(record->rtv[backBuffer]), record->width, record->height};
        presentTarget_ = handle;
        StartPass(clear);
        return Result<CommandContext*>::Success(this);
    }

    Result<CommandContext*> BeginOffscreen(TextureHandle handle,
                                           const ClearColor& clear) noexcept override {
        TextureRecord* record = textures_.Find(handle.slot);
        if (record == nullptr || !record->renderTarget) {
            return Result<CommandContext*>::Failure(record == nullptr ? ErrorCode::StaleHandle
                                                                      : ErrorCode::InvalidArgument);
        }
        if (recording_) {
            return Result<CommandContext*>::Failure(ErrorCode::Busy);
        }

        auto begun = BeginRecording();
        if (!begun) {
            return Result<CommandContext*>::Failure(begun.Error());
        }
        target_ = {record->resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   rtvs_.Cpu(record->rtv), record->width, record->height};
        presentTarget_ = {};
        StartPass(clear);
        return Result<CommandContext*>::Success(this);
    }

    Result<void> EndFrame() noexcept override {
        if (!recording_) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const D3D12_RESOURCE_BARRIER back =
            Transition(target_.resource, D3D12_RESOURCE_STATE_RENDER_TARGET, target_.restState);
        list_->ResourceBarrier(1, &back);
        recording_ = false;
        if (FAILED(list_->Close())) {
            return Fail("command list close failed");
        }
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);

        Result<void> outcome = Result<void>::Success();
        if (!presentTarget_.IsNull()) {
            SwapChainRecord* record = swapChains_.Find(presentTarget_.slot);
            if (record != nullptr) {
                const HRESULT presented = record->swapChain->Present(record->vsync ? 1 : 0, 0);
                if (FAILED(presented)) {
                    outcome = Fail(presented == DXGI_ERROR_DEVICE_REMOVED
                                       ? "Present failed: device removed"
                                       : "Present failed");
                }
            }
        }

        const uint64 value = ++fenceValue_;
        if (FAILED(queue_->Signal(fence_.Get(), value))) {
            // 장치 제거 등으로 완료를 알 수 없다. 이 프레임이 쓴 자원은 소멸 때까지 놓지 않는다.
            AssignPendingRetires(UINT64_MAX);
            DrainValidationMessages();
            return Fail("queue signal failed");
        }
        frames_[frameSlot_].fenceValue = value;
        frameSlot_ = (frameSlot_ + 1) % FrameCount;
        AssignPendingRetires(value);
        ProcessRetired();
        DrainValidationMessages();
        return outcome;
    }

    Result<void> ReadPixels(TextureHandle handle, Span<uint8> out) noexcept override {
        TextureRecord* record = textures_.Find(handle.slot);
        if (record == nullptr) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }
        const uint32 bytesPerPixel = BytesPerPixel(record->format);
        const usize size = static_cast<usize>(record->width) * record->height * bytesPerPixel;
        if (!record->renderTarget || out.Size() != size || recording_) {
            return Result<void>::Failure(recording_ ? ErrorCode::Busy : ErrorCode::InvalidArgument);
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 total = 0;
        const D3D12_RESOURCE_DESC resourceDesc = record->resource->GetDesc();
        device_->GetCopyableFootprints(&resourceDesc, 0, 1, 0, &footprint, nullptr, nullptr,
                                       &total);
        ComRef<ID3D12Resource> readback;
        const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
        const D3D12_RESOURCE_DESC buffer = BufferResource(total);
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(readback.Out())))) {
            return Fail("readback buffer creation failed");
        }

        auto begun = BeginUpload();
        if (!begun) {
            return begun;
        }
        D3D12_RESOURCE_BARRIER toCopy =
            Transition(record->resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
        uploadList_->ResourceBarrier(1, &toCopy);
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = record->resource.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;
        uploadList_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        D3D12_RESOURCE_BARRIER back =
            Transition(record->resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        uploadList_->ResourceBarrier(1, &back);
        auto submitted = SubmitUploadAndWait();
        if (!submitted) {
            return submitted;
        }

        void* mapped = nullptr;
        const D3D12_RANGE whole = {0, static_cast<SIZE_T>(total)};
        if (FAILED(readback->Map(0, &whole, &mapped))) {
            return Fail("readback map failed");
        }
        const uint8* rows = static_cast<const uint8*>(mapped) + footprint.Offset;
        const usize rowBytes = static_cast<usize>(record->width) * bytesPerPixel;
        for (uint32 row = 0; row < record->height; ++row) {
            platform::MemoryCopy(out.Data() + row * rowBytes,
                                 rows + static_cast<usize>(row) * footprint.Footprint.RowPitch,
                                 rowBytes);
        }
        const D3D12_RANGE noWrite = {0, 0};
        readback->Unmap(0, &noWrite);
        return Result<void>::Success();
    }

    void Destroy(BufferHandle handle) noexcept override {
        BufferRecord* record = buffers_.Find(handle.slot);
        if (record == nullptr) {
            return;
        }
        Retire(Move(record->resource), -1, -1);
        auto removed = buffers_.TryRemove(handle.slot);
        (void)removed;
    }

    void Destroy(TextureHandle handle) noexcept override {
        TextureRecord* record = textures_.Find(handle.slot);
        if (record == nullptr || handle == white_) {
            return;
        }
        Retire(Move(record->resource), record->srv,
               record->renderTarget ? static_cast<int64>(record->rtv) : int64{-1});
        auto removed = textures_.TryRemove(handle.slot);
        (void)removed;
    }

    void Destroy(PipelineHandle handle) noexcept override {
        PipelineRecord* record = pipelines_.Find(handle.slot);
        if (record == nullptr) {
            return;
        }
        Retire(Move(record->state), -1, -1);
        auto removed = pipelines_.TryRemove(handle.slot);
        (void)removed;
    }

    void Destroy(SwapChainHandle handle) noexcept override {
        SwapChainRecord* record = swapChains_.Find(handle.slot);
        if (record == nullptr) {
            return;
        }
        // 백버퍼와 RTV는 마지막으로 쓴 프레임이 끝난 뒤 놓는다. 기록 중인 프레임이 이 스왑체인에
        // 그리고 있어도 retire 항목이 참조를 쥐므로 EndFrame의 barrier까지 유효하다(present는
        // 생략된다).
        for (uint32 index = 0; index < SwapChainBufferCount; ++index) {
            Retire(Move(record->buffers[index]), -1, record->rtv[index]);
        }
        Retire(Move(record->swapChain), -1, -1);
        auto removed = swapChains_.TryRemove(handle.slot);
        (void)removed;
    }

    Result<void> WaitIdle() noexcept override {
        const uint64 value = ++fenceValue_;
        if (FAILED(queue_->Signal(fence_.Get(), value))) {
            return Fail("queue signal failed");
        }
        auto waited = WaitFence(value);
        if (!waited) {
            return waited;
        }
        // 기록 중인 프레임의 보류 항목은 아직 제출되지 않았으므로 남긴다.
        ProcessRetired();
        return Result<void>::Success();
    }

    StringView LastError() const noexcept override {
        return lastError_.View();
    }

    uint64 IgnoredDraws() const noexcept override {
        return ignoredDraws_;
    }

    uint64 ValidationErrorCount() const noexcept override {
        DrainValidationMessages();
        return validationErrors_;
    }

private:
    struct Frame {
        ComRef<ID3D12CommandAllocator> allocator;
        uint64 fenceValue = 0;
    };

    struct Target {
        ID3D12Resource* resource = nullptr;
        D3D12_RESOURCE_STATES restState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
        uint32 width = 0;
        uint32 height = 0;
    };

    struct PipelineStride {
        SlotHandle pipeline;
        uint32 stride = 0;
    };

    struct SamplerEntry {
        SamplerDesc desc;
        uint32 slot = 0;
    };

    Result<void> Fail(const char* message) noexcept {
        auto assigned = lastError_.TryAssign(StringView(message, platform::CStringLength(message)));
        (void)assigned;
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    template <class T>
    Result<T> FailWith(const char* message) noexcept {
        auto failed = Fail(message);
        (void)failed;
        return Result<T>::Failure(ErrorCode::DependencyFailed);
    }

    // desc와 같은 샘플러 칸을 돌려주고 없으면 만든다. 칸은 장치 수명 동안 유지한다(조합 수가
    // 작다).
    Result<uint32> SamplerFor(const SamplerDesc& desc) noexcept {
        for (uint32 index = 0; index < samplerCount_; ++index) {
            if (samplerDescs_[index].desc == desc) {
                return Result<uint32>::Success(samplerDescs_[index].slot);
            }
        }
        if (samplerCount_ >= SamplerCapacity) {
            return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
        }
        auto slot = samplers_.Allocate();
        if (!slot) {
            return Result<uint32>::Failure(slot.Error());
        }
        D3D12_SAMPLER_DESC sampler = {};
        sampler.Filter = SamplerFilter(desc.filter);
        sampler.AddressU = SamplerAddress(desc.addressU);
        sampler.AddressV = SamplerAddress(desc.addressV);
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = desc.filter == Filter::Anisotropic ? desc.maxAnisotropy : 1u;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        BorderColorValue(desc.borderColor, sampler.BorderColor);
        device_->CreateSampler(&sampler, samplers_.Cpu(slot.Value()));
        samplerDescs_[samplerCount_++] = {desc, slot.Value()};
        return Result<uint32>::Success(slot.Value());
    }

    Result<void> StrideFor(SlotHandle pipeline, uint32 stride) noexcept {
        // 사용 중인 칸 중 파괴된 파이프라인의 칸을 재사용하고, 없으면 뒤에 추가한다.
        for (uint32 index = 0; index < pipelineStrideCount_; ++index) {
            if (!pipelines_.IsValid(pipelineStrides_[index].pipeline) ||
                pipelineStrides_[index].pipeline == pipeline) {
                pipelineStrides_[index] = {pipeline, stride};
                return Result<void>::Success();
            }
        }
        if (pipelineStrideCount_ >= MaxPipelines) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }
        pipelineStrides_[pipelineStrideCount_++] = {pipeline, stride};
        return Result<void>::Success();
    }

    uint32 StrideOf(SlotHandle pipeline) const noexcept {
        for (uint32 index = 0; index < pipelineStrideCount_; ++index) {
            if (pipelineStrides_[index].pipeline == pipeline) {
                return pipelineStrides_[index].stride;
            }
        }
        return 0;
    }

    Result<void> CreateRootSignature() noexcept {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        range.OffsetInDescriptorsFromTableStart = 0;
        D3D12_DESCRIPTOR_RANGE samplerRange = {};
        samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        samplerRange.NumDescriptors = 1;
        samplerRange.BaseShaderRegister = 0;
        samplerRange.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER parameters[3] = {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.ShaderRegister = 0;
        parameters[0].Constants.Num32BitValues = ConstantCount;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &range;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[2].DescriptorTable.NumDescriptorRanges = 1;
        parameters[2].DescriptorTable.pDescriptorRanges = &samplerRange;
        parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = 3;
        desc.pParameters = parameters;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComRef<ID3DBlob> blob;
        ComRef<ID3DBlob> errors;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.Out(),
                                               errors.Out())) ||
            FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                IID_PPV_ARGS(rootSignature_.Out())))) {
            return Fail("root signature creation failed");
        }
        return Result<void>::Success();
    }

    Result<void> Compile(StringView source, const char* entry, const char* target,
                         ComRef<ID3DBlob>& code) noexcept {
        ComRef<ID3DBlob> errors;
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
        if (validation_) {
            flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
        } else {
            flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
        }
        const HRESULT compiled =
            D3DCompile(source.Data(), source.ByteCount(), "RocksShader", nullptr, nullptr, entry,
                       target, flags, 0, code.Out(), errors.Out());
        if (FAILED(compiled)) {
            if (errors) {
                auto assigned = lastError_.TryAssign(StringView(
                    static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
                (void)assigned;
            } else {
                auto failed = Fail("shader compilation failed");
                (void)failed;
            }
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }
        return Result<void>::Success();
    }

    Result<void> CreateSwapChainViews(SwapChainRecord& record) noexcept {
        D3D12_RENDER_TARGET_VIEW_DESC view = {};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        for (uint32 index = 0; index < SwapChainBufferCount; ++index) {
            if (FAILED(record.swapChain->GetBuffer(index,
                                                   IID_PPV_ARGS(record.buffers[index].Out())))) {
                return Fail("swap chain GetBuffer failed");
            }
            device_->CreateRenderTargetView(record.buffers[index].Get(), &view,
                                            rtvs_.Cpu(record.rtv[index]));
        }
        return Result<void>::Success();
    }

    Result<void> WaitFence(uint64 value) noexcept {
        if (fence_->GetCompletedValue() >= value) {
            return Result<void>::Success();
        }
        if (FAILED(fence_->SetEventOnCompletion(value, fenceEvent_))) {
            return Fail("fence wait setup failed");
        }
        WaitForSingleObject(fenceEvent_, INFINITE);
        return Result<void>::Success();
    }

    Result<void> BeginRecording() noexcept {
        Frame& frame = frames_[frameSlot_];
        // 이 slot의 이전 제출이 끝나야 allocator를 다시 쓸 수 있다.
        auto waited = WaitFence(frame.fenceValue);
        if (!waited) {
            return waited;
        }
        if (FAILED(frame.allocator->Reset()) ||
            FAILED(list_->Reset(frame.allocator.Get(), nullptr))) {
            return Fail("command list reset failed");
        }
        recording_ = true;
        pipelineBound_ = false;
        bufferBound_ = false;
        boundPipeline_ = {};
        return Result<void>::Success();
    }

    void StartPass(const ClearColor& clear) noexcept {
        const D3D12_RESOURCE_BARRIER toTarget =
            Transition(target_.resource, target_.restState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list_->ResourceBarrier(1, &toTarget);
        list_->OMSetRenderTargets(1, &target_.rtv, FALSE, nullptr);
        const float color[4] = {clear.r, clear.g, clear.b, clear.a};
        list_->ClearRenderTargetView(target_.rtv, color, 0, nullptr);
        D3D12_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(target_.width);
        viewport.Height = static_cast<float>(target_.height);
        viewport.MaxDepth = 1.0f;
        const D3D12_RECT scissor = {0, 0, static_cast<LONG>(target_.width),
                                    static_cast<LONG>(target_.height)};
        list_->RSSetViewports(1, &viewport);
        list_->RSSetScissorRects(1, &scissor);
        list_->SetGraphicsRootSignature(rootSignature_.Get());
        ID3D12DescriptorHeap* heaps[] = {srvs_.Heap(), samplers_.Heap()};
        list_->SetDescriptorHeaps(2, heaps);
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const TextureRecord* white = textures_.Find(white_.slot);
        if (white != nullptr) {
            list_->SetGraphicsRootDescriptorTable(1, srvs_.Gpu(white->srv));
            list_->SetGraphicsRootDescriptorTable(2, samplers_.Gpu(white->sampler));
        }
        const DrawConstants identity;
        list_->SetGraphicsRoot32BitConstants(0, ConstantCount, &identity, 0);
    }

    Result<void> BeginUpload() noexcept {
        if (FAILED(uploadAllocator_->Reset()) ||
            FAILED(uploadList_->Reset(uploadAllocator_.Get(), nullptr))) {
            return Fail("upload command list reset failed");
        }
        return Result<void>::Success();
    }

    Result<void> SubmitUploadAndWait() noexcept {
        if (FAILED(uploadList_->Close())) {
            return Fail("upload command list close failed");
        }
        ID3D12CommandList* lists[] = {uploadList_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        const uint64 value = ++fenceValue_;
        if (FAILED(queue_->Signal(fence_.Get(), value))) {
            return Fail("queue signal failed");
        }
        return WaitFence(value);
    }

    Result<void> UploadTexture(TextureRecord& record, Span<const uint8> pixels) noexcept {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[MaxMipLevels] = {};
        UINT64 total = 0;
        const D3D12_RESOURCE_DESC resourceDesc = record.resource->GetDesc();
        device_->GetCopyableFootprints(&resourceDesc, 0, record.mipLevels, 0, footprints, nullptr,
                                       nullptr, &total);
        ComRef<ID3D12Resource> staging;
        const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC buffer = BufferResource(total);
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(staging.Out())))) {
            return Fail("texture staging buffer creation failed");
        }
        void* mapped = nullptr;
        const D3D12_RANGE noRead = {0, 0};
        if (FAILED(staging->Map(0, &noRead, &mapped))) {
            return Fail("texture staging map failed");
        }
        // 원본은 빈틈 없는 행이고 GPU 복사는 256바이트 정렬 행 간격을 요구하므로 mip·행마다 옮긴다.
        const uint32 bytesPerPixel = BytesPerPixel(record.format);
        usize sourceOffset = 0;
        for (uint32 level = 0; level < record.mipLevels; ++level) {
            const uint32 width = MipExtent(record.width, level);
            const uint32 height = MipExtent(record.height, level);
            const usize rowBytes = static_cast<usize>(width) * bytesPerPixel;
            uint8* rows = static_cast<uint8*>(mapped) + footprints[level].Offset;
            for (uint32 row = 0; row < height; ++row) {
                platform::MemoryCopy(rows + static_cast<usize>(row) *
                                                footprints[level].Footprint.RowPitch,
                                     pixels.Data() + sourceOffset + row * rowBytes, rowBytes);
            }
            sourceOffset += rowBytes * height;
        }
        staging->Unmap(0, nullptr);

        auto begun = BeginUpload();
        if (!begun) {
            return begun;
        }
        for (uint32 level = 0; level < record.mipLevels; ++level) {
            D3D12_TEXTURE_COPY_LOCATION destination = {};
            destination.pResource = record.resource.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = level;
            D3D12_TEXTURE_COPY_LOCATION source = {};
            source.pResource = staging.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprints[level];
            uploadList_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        const D3D12_RESOURCE_BARRIER toShader =
            Transition(record.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        uploadList_->ResourceBarrier(1, &toShader);
        // 업로드 완료를 기다린 뒤 staging을 놓는다(초기 로딩 경로라 동기 대기를 허용한다).
        auto submitted = SubmitUploadAndWait();
        if (!submitted) {
            // GPU가 아직 staging을 읽고 있을 수 있다(장치 제거 등). 장치 소멸까지 놓지 않는다.
            KeepUntilDestruction(Move(staging));
        }
        return submitted;
    }

    void KeepUntilDestruction(ComRef<ID3D12Resource>&& object) noexcept {
        Retired entry;
        entry.fenceValue = UINT64_MAX;
        entry.object.Attach(object.Detach());
        auto pushed = retired_.TryPushBack(Move(entry));
        if (!pushed) {
            // 기록할 공간조차 없으면 해제하지 않고 버린다(GPU가 읽는 중일 수 있는 메모리를 놓는
            // 것보다 낫다).
            (void)entry.object.Detach();
        }
    }

    void ConfigureValidationQueues() noexcept {
        if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(infoQueue_.Out())))) {
            // 오류만 저장해 저장 한도(기본 1024개)를 경고가 채우지 않게 한다.
            D3D12_MESSAGE_SEVERITY severities[] = {D3D12_MESSAGE_SEVERITY_CORRUPTION,
                                                   D3D12_MESSAGE_SEVERITY_ERROR};
            D3D12_INFO_QUEUE_FILTER filter = {};
            filter.AllowList.NumSeverities = 2;
            filter.AllowList.pSeverityList = severities;
            (void)infoQueue_->PushStorageFilter(&filter);
        }
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(dxgiInfoQueue_.Out())))) {
            DXGI_INFO_QUEUE_MESSAGE_SEVERITY severities[] = {
                DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION,
                DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR};
            DXGI_INFO_QUEUE_FILTER filter = {};
            filter.AllowList.NumSeverities = 2;
            filter.AllowList.pSeverityList = severities;
            (void)dxgiInfoQueue_->PushStorageFilter(DxgiDebugDxgi, &filter);
        }
    }

    // 저장된 검증 메시지 중 오류를 세어 누적하고 비운다. 메시지 길이에 상한을 두지 않는다.
    void DrainValidationMessages() const noexcept {
        if (infoQueue_) {
            const UINT64 count = infoQueue_->GetNumStoredMessages();
            for (UINT64 index = 0; index < count; ++index) {
                SIZE_T length = 0;
                if (FAILED(infoQueue_->GetMessage(index, nullptr, &length)) || length == 0) {
                    ++validationErrors_;
                    continue;
                }
                auto* message = static_cast<D3D12_MESSAGE*>(Scratch(length));
                if (message == nullptr || FAILED(infoQueue_->GetMessage(index, message, &length)) ||
                    message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                    message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
                    // 읽지 못한 메시지도 오류로 센다(필터가 오류만 저장한다).
                    ++validationErrors_;
                }
            }
            infoQueue_->ClearStoredMessages();
        }
        if (dxgiInfoQueue_) {
            const UINT64 count = dxgiInfoQueue_->GetNumStoredMessages(DxgiDebugDxgi);
            for (UINT64 index = 0; index < count; ++index) {
                SIZE_T length = 0;
                if (FAILED(dxgiInfoQueue_->GetMessage(DxgiDebugDxgi, index, nullptr, &length)) ||
                    length == 0) {
                    ++validationErrors_;
                    continue;
                }
                auto* message = static_cast<DXGI_INFO_QUEUE_MESSAGE*>(Scratch(length));
                if (message == nullptr ||
                    FAILED(dxgiInfoQueue_->GetMessage(DxgiDebugDxgi, index, message, &length)) ||
                    message->Severity == DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR ||
                    message->Severity == DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION) {
                    ++validationErrors_;
                }
            }
            dxgiInfoQueue_->ClearStoredMessages(DxgiDebugDxgi);
        }
    }

    // 메시지 구조체를 담을 16 byte 정렬 버퍼다. 할당 실패면 null이다.
    void* Scratch(SIZE_T length) const noexcept {
        const usize needed = static_cast<usize>(length) + 16;
        if (messageScratch_.Size() < needed) {
            auto resized = messageScratch_.TryResize(needed);
            if (!resized) {
                return nullptr;
            }
        }
        const uptr address = reinterpret_cast<uptr>(messageScratch_.Data());
        return reinterpret_cast<void*>((address + 15u) & ~static_cast<uptr>(15u));
    }

    // 모든 D3D 인터페이스는 IUnknown을 단일 상속하므로 포인터 변환이 안전하다.
    template <class T>
    void Retire(ComRef<T>&& object, int64 srv, int64 rtv) noexcept {
        ComRef<IUnknown> unknown;
        unknown.Attach(object.Detach());
        RetireObject(Move(unknown), srv, rtv);
    }

    void RetireObject(ComRef<IUnknown>&& object, int64 srv, int64 rtv) noexcept {
        Retired entry;
        entry.object = Move(object);
        // 제출된 작업은 모두 signal 뒤에 있으므로 마지막 signal 값이 끝나면 더 쓰지 않는다. 기록
        // 중이면 아직 제출하지 않은 프레임이 쓸 수 있으므로 그 프레임의 signal 값을 EndFrame에서
        // 정한다.
        entry.fenceValue = fenceValue_;
        entry.pending = recording_;
        entry.srv = srv;
        entry.rtv = rtv;
        // 용량은 자원을 만들 때 미리 확보했으므로(ReserveRetireCapacity) 실패하지 않는다.
        auto pushed = retired_.TryPushBack(Move(entry));
        if (!pushed) {
            platform::Fatal("D3D12 retire list capacity invariant violated");
        }
    }

    // 살아 있는 자원이 모두 retire되어도 목록이 자라지 않도록 용량을 확보한다. Retire가 할당 실패로
    // GPU 사용 중인 자원을 놓는 경로를 없앤다.
    Result<void> ReserveRetireCapacity(usize extra) noexcept {
        const usize live = buffers_.Size() + textures_.Size() + pipelines_.Size() +
                           swapChains_.Size() * SwapChainRetireEntries;
        return retired_.TryReserve(retired_.Size() + live + extra);
    }

    void AssignPendingRetires(uint64 value) noexcept {
        for (Retired& entry : retired_) {
            if (entry.pending) {
                entry.pending = false;
                entry.fenceValue = value;
            }
        }
    }

    void ProcessRetired() noexcept {
        const uint64 completed = fence_->GetCompletedValue();
        for (usize index = retired_.Size(); index > 0; --index) {
            Retired& entry = retired_[index - 1];
            if (entry.pending || entry.fenceValue > completed) {
                continue;
            }
            if (entry.srv >= 0) {
                srvs_.Free(static_cast<uint32>(entry.srv));
            }
            if (entry.rtv >= 0) {
                rtvs_.Free(static_cast<uint32>(entry.rtv));
            }
            auto removed = retired_.TryRemoveAtSwap(index - 1);
            (void)removed;
        }
    }

    // --- CommandContext ---

    void SetPipeline(PipelineHandle handle) noexcept override {
        const PipelineRecord* record = pipelines_.Find(handle.slot);
        if (record == nullptr) {
            pipelineBound_ = false;
            return;
        }
        list_->SetPipelineState(record->state.Get());
        pipelineBound_ = true;
        boundPipeline_ = handle.slot;
        // 이미 묶인 정점 버퍼가 있으면 새 파이프라인 간격으로 다시 묶는다.
        if (bufferBound_) {
            SetVertexBuffer(boundBuffer_);
        }
    }

    void SetVertexBuffer(BufferHandle handle) noexcept override {
        const BufferRecord* record = buffers_.Find(handle.slot);
        if (record == nullptr) {
            bufferBound_ = false;
            return;
        }
        D3D12_VERTEX_BUFFER_VIEW view = record->view;
        view.StrideInBytes = pipelineBound_ ? StrideOf(boundPipeline_) : 0;
        list_->IASetVertexBuffers(0, 1, &view);
        bufferBound_ = true;
        boundBuffer_ = handle;
    }

    void SetTexture(TextureHandle handle) noexcept override {
        const TextureRecord* record = textures_.Find(handle.slot);
        if (record == nullptr) {
            record = textures_.Find(white_.slot);
        }
        if (record != nullptr) {
            list_->SetGraphicsRootDescriptorTable(1, srvs_.Gpu(record->srv));
            list_->SetGraphicsRootDescriptorTable(2, samplers_.Gpu(record->sampler));
        }
    }

    void SetConstants(const DrawConstants& constants) noexcept override {
        list_->SetGraphicsRoot32BitConstants(0, ConstantCount, &constants, 0);
    }

    void Draw(uint32 vertexCount, uint32 firstVertex) noexcept override {
        if (!pipelineBound_ || !bufferBound_ || !buffers_.IsValid(boundBuffer_.slot)) {
            ++ignoredDraws_;
            return;
        }
        list_->DrawInstanced(vertexCount, 1, firstVertex, 0);
    }

    static constexpr uint32 MaxPipelines = 256;

    String adapterName_;
    String lastError_;
    SlotArray<BufferRecord> buffers_;
    SlotArray<TextureRecord> textures_;
    SlotArray<PipelineRecord> pipelines_;
    SlotArray<SwapChainRecord> swapChains_;
    Array<Retired> retired_;
    DescriptorPool rtvs_;
    DescriptorPool srvs_;
    DescriptorPool samplers_;
    SamplerEntry samplerDescs_[SamplerCapacity];
    uint32 samplerCount_ = 0;
    ComRef<IDXGIFactory4> factory_;
    ComRef<ID3D12Device> device_;
    ComRef<ID3D12InfoQueue> infoQueue_;
    ComRef<ID3D12CommandQueue> queue_;
    ComRef<ID3D12GraphicsCommandList> list_;
    ComRef<ID3D12CommandAllocator> uploadAllocator_;
    ComRef<ID3D12GraphicsCommandList> uploadList_;
    ComRef<ID3D12RootSignature> rootSignature_;
    ComRef<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    Frame frames_[FrameCount];
    uint32 frameSlot_ = 0;
    uint64 fenceValue_ = 0;
    Target target_;
    SwapChainHandle presentTarget_;
    TextureHandle white_;
    PipelineStride pipelineStrides_[MaxPipelines];
    uint32 pipelineStrideCount_ = 0;
    SlotHandle boundPipeline_;
    BufferHandle boundBuffer_;
    uint64 ignoredDraws_ = 0;
    ComRef<IDXGIInfoQueue> dxgiInfoQueue_;
    // 검증 메시지는 읽을 때마다 비우고 여기에 누적한다(저장 한도로 오래된 오류가 밀려나지 않게).
    mutable uint64 validationErrors_ = 0;
    mutable Array<uint8> messageScratch_;
    bool recording_ = false;
    bool pipelineBound_ = false;
    bool bufferBound_ = false;
    bool validation_ = false;
    bool software_ = false;
};

} // namespace

Result<UniquePtr<Device>> CreateD3D12Device(Allocator& allocator, const DeviceDesc& desc) noexcept {
    auto created = UniquePtr<D3D12Device>::TryMake(allocator, allocator);
    if (!created) {
        return Result<UniquePtr<Device>>::Failure(created.Error());
    }

    auto initialized = created.Value()->Initialize(desc);
    if (!initialized) {
        return Result<UniquePtr<Device>>::Failure(initialized.Error());
    }

    return Result<UniquePtr<Device>>::Success(UniquePtr<Device>(Move(created.Value())));
}

} // namespace rocks::graphics
