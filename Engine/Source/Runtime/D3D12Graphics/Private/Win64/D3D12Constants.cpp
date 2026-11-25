#include "D3D12DeviceImpl.h"

// 상수 버퍼(Docs/Design/RenderBase 9.11절 GX6)다: handle은 upload heap page 구간(GPU가 그대로
// 읽는다), 기록 명령(SetConstantData)은 queue별 upload ring, 그리기 때 table은 CPU 전용 heap(srvs_)
// 에서 shader-visible ring으로 복사한다(UE offline → online heap과 같다).
namespace rocks::graphics::d3d12 {

namespace {

constexpr D3D12_RANGE NoRead = {0, 0};

// SetConstantData가 기록 명령 ring에서 잡는 칸의 정렬이다(CBV 주소 규칙).
constexpr uint64 TransientConstantAlignment = ConstantBufferAlignment;

Result<void> CreateMappedUpload(ID3D12Device* device, uint64 bytes, ComRef<ID3D12Resource>& out,
                                uint8*& data) noexcept {
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC desc = BufferResource(bytes);
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(out.Out())))) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    void* mapped = nullptr;
    if (FAILED(out->Map(0, &NoRead, &mapped))) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    data = static_cast<uint8*>(mapped);
    return Result<void>::Success();
}

} // namespace

Result<void> D3D12Device::InitializeConstants() noexcept {
    D3D12_DESCRIPTOR_HEAP_DESC online = {};
    online.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    online.NumDescriptors = OnlineDescriptorsPerQueue * 2;
    online.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    const HRESULT created = device_->CreateDescriptorHeap(&online, IID_PPV_ARGS(online_.Out()));
    if (FAILED(created)) {
        return Fail("CreateDescriptorHeap (shader-visible descriptor ring)", created);
    }

    onlineIncrement_ =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    TransientRings* rings[] = {&graphicsRings_, &computeRings_};
    for (uint32 index = 0; index < 2; ++index) {
        TransientRings& ring = *rings[index];
        ring.descriptorBase = index * OnlineDescriptorsPerQueue;
        auto descriptors = ring.descriptors.Reset(OnlineDescriptorsPerQueue);
        auto constants = ring.constants.Reset(DefaultConstantRingBytes);
        if (!descriptors || !constants) {
            return Result<void>::Failure(!descriptors ? descriptors.Error() : constants.Error());
        }

        if (!CreateMappedUpload(device_.Get(), DefaultConstantRingBytes, ring.buffer, ring.data)) {
            return Fail("constant ring creation failed");
        }

        ring.address = ring.buffer->GetGPUVirtualAddress();
    }

    // 넘친 CBV table의 빈 칸(셰이더가 읽지 않는 slot)이 가리킬 0 구간이다.
    uint32 page = 0;
    GpuPageAllocation block;
    auto zero = AllocateConstantBlock(ConstantBufferAlignment, page, block);
    if (!zero) {
        return zero;
    }

    platform::MemoryZero(constantPages_[page]->data + block.offset, ConstantBufferAlignment);
    zeroConstants_ = constantPages_[page]->address + block.offset;

    // PS 진단 버퍼 자리에 묶는 내부 버퍼다(9.11절 6항). committed 자원은 0으로 시작하고, 버퍼라
    // COMMON에서 UAV로 암묵 승격된다.
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC debug = BufferResource(PixelDebugBufferBytes);
    debug.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const HRESULT debugCreated = device_->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &debug, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(debugBuffer_.Out()));
    if (FAILED(debugCreated)) {
        return Fail("CreateCommittedResource (pixel debug buffer)", debugCreated);
    }

    return Result<void>::Success();
}

Result<void> D3D12Device::AllocateConstantBlock(uint64 bytes, uint32& page,
                                                GpuPageAllocation& block) noexcept {
    const uint64 size = AlignConstantBufferBytes(bytes);
    for (usize index = 0; index < constantPages_.Size(); ++index) {
        auto allocated = constantPages_[index]->ranges.Allocate(size, ConstantBufferAlignment);
        if (allocated) {
            page = static_cast<uint32>(index);
            block = allocated.Value();
            return Result<void>::Success();
        }

        if (allocated.Error() != ErrorCode::CapacityExceeded) {
            return Result<void>::Failure(allocated.Error());
        }
    }

    auto made = UniquePtr<ConstantPage>::TryMake(allocator_, allocator_);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    ConstantPage& fresh = *made.Value();
    auto reset = fresh.ranges.Reset(ConstantPageBytes);
    if (!reset) {
        return reset;
    }

    if (!CreateMappedUpload(device_.Get(), ConstantPageBytes, fresh.resource, fresh.data)) {
        return Fail("constant buffer page creation failed");
    }

    fresh.address = fresh.resource->GetGPUVirtualAddress();
    auto allocated = fresh.ranges.Allocate(size, ConstantBufferAlignment);
    if (!allocated) {
        return Result<void>::Failure(allocated.Error());
    }

    if (!constantPages_.TryPushBack(Move(made.Value()))) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    page = static_cast<uint32>(constantPages_.Size() - 1);
    block = allocated.Value();
    return Result<void>::Success();
}

void D3D12Device::RetireConstantBlock(uint32 page, const GpuPageAllocation& block) noexcept {
    // 회수 시점(마지막 사용 fence)은 다른 자원과 같은 규칙이다(RetireObject). 객체 없이 구간만
    // 남긴다.
    RetireObject(ComRef<IUnknown>(), -1, -1);
    Retired& entry = retired_[retired_.Size() - 1];
    entry.constantPage = page;
    entry.constantBlock = block;
}

Result<ConstantBufferHandle>
D3D12Device::CreateConstantBuffer(ConstantBufferLifetime lifetime,
                                  Span<const uint8> contents) noexcept {
    platform::LockGuard lock(gate_);
    if ((lifetime != ConstantBufferLifetime::SingleFrame &&
         lifetime != ConstantBufferLifetime::MultiFrame) ||
        !IsValidConstantBufferBytes(contents.Size())) {
        return FailCode<ConstantBufferHandle>(
            ErrorCode::InvalidArgument,
            "constant buffer contents must be 16-byte multiples of 16 to 65536 bytes");
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<ConstantBufferHandle>::Failure(reserved.Error());
    }

    ConstantBufferRecord record;
    record.lifetime = lifetime;
    record.bytes = static_cast<uint32>(contents.Size());
    auto allocated = AllocateConstantBlock(contents.Size(), record.page, record.block);
    if (!allocated) {
        return Result<ConstantBufferHandle>::Failure(allocated.Error());
    }

    platform::MemoryCopy(constantPages_[record.page]->data + record.block.offset, contents.Data(),
                         contents.Size());
    const uint32 page = record.page;
    const GpuPageAllocation block = record.block;
    auto added = constantBuffers_.TryAdd(Move(record));
    if (!added) {
        constantPages_[page]->ranges.Free(block);
        return Result<ConstantBufferHandle>::Failure(added.Error());
    }

    return Result<ConstantBufferHandle>::Success(ConstantBufferHandle{added.Value()});
}

Result<void> D3D12Device::UpdateConstantBuffer(ConstantBufferHandle buffer,
                                               Span<const uint8> contents) noexcept {
    platform::LockGuard lock(gate_);
    ConstantBufferRecord* record = constantBuffers_.Find(buffer.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (record->lifetime != ConstantBufferLifetime::MultiFrame ||
        contents.Size() != record->bytes) {
        return FailCode<void>(ErrorCode::InvalidArgument,
                              "only MultiFrame constant buffers can be updated, with their size");
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return reserved;
    }

    // 새 자리를 받아 옮긴다(rename): 앞에 기록한 그리기는 옛 구간을 계속 읽는다.
    uint32 page = 0;
    GpuPageAllocation block;
    auto allocated = AllocateConstantBlock(contents.Size(), page, block);
    if (!allocated) {
        return allocated;
    }

    platform::MemoryCopy(constantPages_[page]->data + block.offset, contents.Data(),
                         contents.Size());
    RetireConstantBlock(record->page, record->block);
    record->page = page;
    record->block = block;
    return Result<void>::Success();
}

void D3D12Device::Retire(ConstantBufferHandle buffer) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    ConstantBufferRecord* record = constantBuffers_.Find(buffer.slot);
    if (record == nullptr) {
        return;
    }

    RetireConstantBlock(record->page, record->block);
    auto removed = constantBuffers_.TryRemove(buffer.slot);
    (void)removed;
}

TransientRings& D3D12Device::ActiveRings() noexcept {
    return computeRecording_ ? computeRings_ : graphicsRings_;
}

Result<uint64> D3D12Device::AllocateTransient(UploadRingAllocator& ring, uint64 size,
                                              uint64 alignment) noexcept {
    const bool compute = computeRecording_;
    for (;;) {
        ring.Reclaim(compute ? ComputeCompletedValue() : fence_->GetCompletedValue());
        auto offset = ring.Allocate(size, alignment);
        if (offset || offset.Error() != ErrorCode::CapacityExceeded || !ownership_.IsOwner()) {
            return offset;
        }

        // 앞 제출이 쓰는 묶음만 기다릴 수 있다(지금 기록의 열린 구간은 제출 전이라 기다리지
        // 못한다).
        const uint64 oldest = ring.OldestFence();
        if (oldest == 0 || oldest == RetainUntilDestructionFence) {
            return offset;
        }

        auto waited = compute ? WaitComputeOwner(oldest) : WaitFenceOwner(oldest);
        if (!waited) {
            return Result<uint64>::Failure(waited.Error());
        }
    }
}

bool D3D12Device::AllocateOnline(uint32 count, D3D12_CPU_DESCRIPTOR_HANDLE& cpu,
                                 D3D12_GPU_DESCRIPTOR_HANDLE& gpu) noexcept {
    TransientRings& rings = ActiveRings();
    auto offset = AllocateTransient(rings.descriptors, count, 1);
    if (!offset) {
        if (!onlineExhaustedLogged_) {
            onlineExhaustedLogged_ = true;
            Log::Warning(LogGraphicsD3D12,
                         "D3D12 shader-visible descriptor ring of a recording is exhausted; "
                         "further draws in the recording are ignored");
        }

        return false;
    }

    const uint64 index = rings.descriptorBase + offset.Value();
    cpu = online_->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(index) * onlineIncrement_;
    gpu = online_->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += index * onlineIncrement_;
    return true;
}

void D3D12Device::CloseTransientRings(bool compute, uint64 fence) noexcept {
    TransientRings& rings = compute ? computeRings_ : graphicsRings_;
    rings.constants.CloseBatch(fence);
    rings.descriptors.CloseBatch(fence);
}

D3D12_GPU_VIRTUAL_ADDRESS D3D12Device::BoundConstantAddress(uint32 stage, uint32 slot,
                                                            uint32& bytes) const noexcept {
    const BoundConstant& bound = boundConstants_[stage][slot];
    if (!bound.handle.IsNull()) {
        const ConstantBufferRecord* record = constantBuffers_.Find(bound.handle);
        if (record == nullptr) {
            bytes = 0;
            return 0;
        }

        bytes = record->bytes;
        return constantPages_[record->page]->address + record->block.offset;
    }

    bytes = bound.bytes;
    return bound.data;
}

bool D3D12Device::ConstantsBound(const PipelineRecord& pipeline) const noexcept {
    uint32 bound[ConstantBufferStageCount][MaxConstantBufferSlots] = {};
    for (uint32 stage = 0; stage < ConstantBufferStageCount; ++stage) {
        const uint32 used = pipeline.slots.constantSlots.usedMask[stage];
        for (uint32 slot = 0; slot < MaxConstantBufferSlots; ++slot) {
            if ((used & (1u << slot)) != 0) {
                (void)BoundConstantAddress(stage, slot, bound[stage][slot]);
            }
        }
    }

    return ConstantBuffersSatisfied(pipeline.slots.constantSlots, bound);
}

bool D3D12Device::CopyTable(uint32 cacheStage, uint32 cacheKind, const uint32* sources,
                            uint32 count, D3D12_GPU_DESCRIPTOR_HANDLE& gpu) noexcept {
    TableCache& cache = tableCache_[cacheStage][cacheKind];
    if (cache.valid && cache.count == count &&
        memcmp(cache.sources, sources, count * sizeof(uint32)) == 0) {
        gpu = cache.gpu;
        return true;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
    if (!AllocateOnline(count, cpu, gpu)) {
        cache.valid = false;
        return false;
    }

    for (uint32 index = 0; index < count; ++index) {
        D3D12_CPU_DESCRIPTOR_HANDLE destination = cpu;
        destination.ptr += static_cast<SIZE_T>(index) * onlineIncrement_;
        device_->CopyDescriptorsSimple(1, destination, srvs_.Cpu(sources[index]),
                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        cache.sources[index] = sources[index];
    }

    cache.valid = true;
    cache.count = count;
    cache.gpu = gpu;
    return true;
}

void D3D12Device::SetConstantBuffer(ShaderStage stage, uint32 slot,
                                    ConstantBufferHandle buffer) noexcept {
    platform::LockGuard lock(gate_);
    const uint32 index = ConstantBufferStageIndex(stage);
    if (index >= ConstantBufferStageCount || slot >= MaxConstantBufferSlots) {
        return;
    }

    const bool live = constantBuffers_.Find(buffer.slot) != nullptr;
    boundConstants_[index][slot] = live ? BoundConstant{buffer.slot, 0, 0} : BoundConstant{};
}

Result<void> D3D12Device::SetConstantData(ShaderStage stage, uint32 slot,
                                          Span<const uint8> bytes) noexcept {
    platform::LockGuard lock(gate_);
    const uint32 index = ConstantBufferStageIndex(stage);
    if (!recording_ || index >= ConstantBufferStageCount || slot >= MaxConstantBufferSlots ||
        !IsValidConstantBufferBytes(bytes.Size())) {
        return FailCode<void>(ErrorCode::InvalidArgument,
                              "SetConstantData needs a recording, a stage slot below 14 and "
                              "16-byte multiples of 16 to 65536 bytes");
    }

    TransientRings& rings = ActiveRings();
    auto offset = AllocateTransient(rings.constants, AlignConstantBufferBytes(bytes.Size()),
                                    TransientConstantAlignment);
    if (!offset) {
        boundConstants_[index][slot] = {};
        return FailCode<void>(offset.Error(),
                              "the constant ring of this recording is full (SetConstantData)");
    }

    platform::MemoryCopy(rings.data + offset.Value(), bytes.Data(), bytes.Size());
    boundConstants_[index][slot] = BoundConstant{SlotHandle{}, rings.address + offset.Value(),
                                                 static_cast<uint32>(bytes.Size())};
    return Result<void>::Success();
}

} // namespace rocks::graphics::d3d12
