#include "D3D12DeviceImpl.h"

// transient heap·placed 자원·aliasing 획득(RenderBase 8절 GX12)이다. heap은 종류(Buffers·Textures·
// Targets)마다 ID3D12Heap 하나이고(resource heap tier 1 규칙, UE
// D3D12TransientResourceAllocator.cpp :15–53), placed 자원은 그 offset에 CreatePlacedResource로
// 둔다. 자원 설정은 committed 생성과 같다 (CreateTextureIn·CreateBufferIn). 같은 구간을 쓰는 자원은
// 처음 쓰기 전에 획득 전이를 내고, 장치는 heap마다 활성 자원을 추적해 새로 활성이 되는 자원에
// aliasing barrier(앞 자원 null)를 낸다(UE D3D12LegacyBarriers.cpp:1346–1388).
namespace rocks::graphics::d3d12 {

namespace {

// placed로 둘 수 있는 텍스처 용도다(초기 데이터가 없는 렌더 타깃·깊이·Storage).
bool PlaceableTexture(const TextureDesc& desc) noexcept {
    return HasTextureUsage(desc.usage, TextureUsage::RenderTarget | TextureUsage::DepthStencil |
                                           TextureUsage::Storage);
}

bool IsTransientHeapKind(TransientHeapKind kind) noexcept {
    return kind == TransientHeapKind::Buffers || kind == TransientHeapKind::Textures ||
           kind == TransientHeapKind::Targets;
}

// heap 종류의 D3D12 heap flag다(UE와 같이 종류마다 한 자원 계열만 받는다).
D3D12_HEAP_FLAGS TransientHeapFlags(TransientHeapKind kind) noexcept {
    switch (kind) {
    case TransientHeapKind::Buffers:
        return D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    case TransientHeapKind::Textures:
        return D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    case TransientHeapKind::Targets:
        return D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
    }

    return D3D12_HEAP_FLAG_NONE;
}

// 자원 설정이 heap 종류에 들어가는지다. 깊이 형식은 용도와 무관하게 DEPTH_STENCIL 자원이므로
// RT·DS heap(Targets)에만 둘 수 있다.
bool FitsTransientHeap(const D3D12_RESOURCE_DESC& resource, TransientHeapKind kind) noexcept {
    if (resource.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        return kind == TransientHeapKind::Buffers;
    }

    const bool target = (resource.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                           D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
    return kind == (target ? TransientHeapKind::Targets : TransientHeapKind::Textures);
}

// 두 heap 구간이 겹치는지다.
bool PlacedRangesOverlap(const PlacedRange& first, const PlacedRange& second) noexcept {
    return first.offset < second.offset + second.bytes &&
           second.offset < first.offset + first.bytes;
}

// GetResourceAllocationInfo가 잘못된 설정에 돌려주는 크기다.
constexpr uint64 InvalidAllocationBytes = NumericLimits<uint64>::Max();

} // namespace

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation).
Result<TransientHeapHandle>
D3D12Device::CreateTransientHeap(const TransientHeapDesc& desc) noexcept {
    if (desc.bytes == 0 || !IsTransientHeapKind(desc.kind)) {
        return Result<TransientHeapHandle>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(gate_);
    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<TransientHeapHandle>::Failure(reserved.Error());
    }

    D3D12_HEAP_DESC heapDesc = {};
    heapDesc.SizeInBytes = desc.bytes;
    heapDesc.Properties = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    // 렌더 타깃 heap은 MSAA 타깃을 둘 수 있게 4 MiB 정렬이다(UE와 같다).
    heapDesc.Alignment = desc.kind == TransientHeapKind::Targets
                             ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                             : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    // 내용은 획득 뒤 쓰므로 0으로 채울 필요가 없다.
    heapDesc.Flags = TransientHeapFlags(desc.kind);
    if (heapNotZeroed_) {
        heapDesc.Flags |= D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
    }

    TransientHeapRecord record(allocator_);
    record.size = desc.bytes;
    record.kind = desc.kind;
    const HRESULT created = device_->CreateHeap(&heapDesc, IID_PPV_ARGS(record.heap.Out()));
    if (FAILED(created)) {
        auto failed = Fail("CreateHeap (transient heap)", created);
        return Result<TransientHeapHandle>::Failure(
            created == E_OUTOFMEMORY ? ErrorCode::OutOfMemory : failed.Error());
    }

    auto added = heaps_.TryAdd(Move(record));
    if (!added) {
        return Result<TransientHeapHandle>::Failure(added.Error());
    }

    return Result<TransientHeapHandle>::Success(TransientHeapHandle{added.Value()});
}

void D3D12Device::RetireTransientHeap(TransientHeapHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    TransientHeapRecord* record = heaps_.Find(handle.slot);
    if (record == nullptr || record->retirePending) {
        return;
    }

    // placed 자원·view가 남아 있으면 마지막이 회수될 때(ReleasePlacement) 놓는다. 없으면 heap의
    // 마지막 사용(두 queue의 지금까지 제출과 기록 중인 기록) 뒤에 놓는다.
    record->retirePending = true;
    if (record->placedCount != 0) {
        return;
    }

    Defer(Move(record->heap), -1, -1);
    auto removed = heaps_.TryRemove(handle.slot);
    (void)removed;
}

void D3D12Device::ReleasePlacement(SlotHandle slot) noexcept {
    TransientHeapRecord* record = heaps_.Find(slot);
    if (record == nullptr || record->placedCount == 0) {
        return;
    }

    // placed 자원·view는 두 queue의 회수 fence가 끝난 뒤 놓인다(ProcessRetired). heap을 쓰는 것은
    // placed 자원뿐이므로 마지막이 놓이면 heap의 마지막 사용도 끝났다.
    if (--record->placedCount == 0 && record->retirePending) {
        auto removed = heaps_.TryRemove(slot);
        (void)removed;
    }
}

// 스레드 안전하다(장치 생성 뒤 바뀌지 않는 표와 GetResourceAllocationInfo만 쓴다).
Result<TransientPlacement>
D3D12Device::GetTexturePlacement(const TextureDesc& desc) const noexcept {
    const TextureDescCheck check = CheckTextureDesc(desc, 0);
    if (check.code != ErrorCode::None) {
        return Result<TransientPlacement>::Failure(check.code);
    }

    // Sampled 텍스처는 초기 데이터가 필요하므로 placed로 두지 않는다.
    if (!PlaceableTexture(desc)) {
        return Result<TransientPlacement>::Failure(ErrorCode::InvalidArgument);
    }

    const D3D12_RESOURCE_DESC resource = TextureResourceFor(desc);
    const TransientHeapKind kind = TransientHeapKindOf(desc);
    if (!FitsTransientHeap(resource, kind)) {
        return Result<TransientPlacement>::Failure(ErrorCode::Unsupported);
    }

    const D3D12_RESOURCE_ALLOCATION_INFO info = device_->GetResourceAllocationInfo(0, 1, &resource);
    if (info.SizeInBytes == InvalidAllocationBytes) {
        return Result<TransientPlacement>::Failure(ErrorCode::Unsupported);
    }

    TransientPlacement placement;
    placement.bytes = info.SizeInBytes;
    placement.alignment = info.Alignment;
    placement.kind = kind;
    return Result<TransientPlacement>::Success(placement);
}

Result<TransientPlacement> D3D12Device::GetBufferPlacement(const BufferDesc& desc) const noexcept {
    if (!ValidateBufferDesc(desc)) {
        return Result<TransientPlacement>::Failure(ErrorCode::InvalidArgument);
    }

    if (desc.size > caps_.limits.bufferBytes) {
        return Result<TransientPlacement>::Failure(ErrorCode::Unsupported);
    }

    const D3D12_RESOURCE_DESC resource = BufferResourceFor(desc);
    const D3D12_RESOURCE_ALLOCATION_INFO info = device_->GetResourceAllocationInfo(0, 1, &resource);
    if (info.SizeInBytes == InvalidAllocationBytes) {
        return Result<TransientPlacement>::Failure(ErrorCode::Unsupported);
    }

    TransientPlacement placement;
    placement.bytes = info.SizeInBytes;
    placement.alignment = info.Alignment;
    placement.kind = TransientHeapKind::Buffers;
    return Result<TransientPlacement>::Success(placement);
}

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation).
Result<TextureHandle> D3D12Device::CreatePlacedTexture(TransientHeapHandle heap, uint64 offset,
                                                       const TextureDesc& desc) noexcept {
    if (!PlaceableTexture(desc)) {
        return Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
    }

    const Placement placement{heap.slot, offset};
    return CreateTextureIn(desc, {}, &placement);
}

Result<BufferHandle> D3D12Device::CreatePlacedBuffer(TransientHeapHandle heap, uint64 offset,
                                                     const BufferDesc& desc) noexcept {
    const Placement placement{heap.slot, offset};
    return CreateBufferIn(desc, {}, &placement);
}

Result<TransientHeapRecord*> D3D12Device::CheckPlacement(const Placement& placement,
                                                         TransientHeapKind kind,
                                                         const D3D12_RESOURCE_DESC& resource,
                                                         uint64& bytes) noexcept {
    TransientHeapRecord* heap = heaps_.Find(placement.heap);
    if (heap == nullptr || heap->retirePending) {
        return Result<TransientHeapRecord*>::Failure(ErrorCode::StaleHandle);
    }

    if (heap->kind != kind) {
        return FailCode<TransientHeapRecord*>(ErrorCode::InvalidArgument,
                                              "resource kind differs from the transient heap kind");
    }

    if (!FitsTransientHeap(resource, kind)) {
        return FailCode<TransientHeapRecord*>(
            ErrorCode::Unsupported,
            "a depth format texture can only be placed in a render target transient heap");
    }

    const D3D12_RESOURCE_ALLOCATION_INFO info = device_->GetResourceAllocationInfo(0, 1, &resource);
    if (info.SizeInBytes == InvalidAllocationBytes) {
        return FailCode<TransientHeapRecord*>(ErrorCode::Unsupported,
                                              "the device reports no placement for the resource");
    }

    const uint64 alignment = info.Alignment != 0 ? info.Alignment : 1;
    if (placement.offset % alignment != 0 || info.SizeInBytes > heap->size ||
        placement.offset > heap->size - info.SizeInBytes) {
        return FailCode<TransientHeapRecord*>(
            ErrorCode::InvalidArgument,
            "placement offset is misaligned or outside the transient heap");
    }

    auto reserved = heap->ranges.TryEnsureCapacity(heap->ranges.Size() + 1);
    if (!reserved) {
        return Result<TransientHeapRecord*>::Failure(reserved.Error());
    }

    bytes = info.SizeInBytes;
    return Result<TransientHeapRecord*>::Success(heap);
}

void D3D12Device::TrackPlaced(SlotHandle heap, SlotHandle resource, uint64 offset,
                              uint64 bytes) noexcept {
    TransientHeapRecord* record = heaps_.Find(heap);
    if (record == nullptr) {
        return;
    }

    // 용량은 CheckPlacement가 잡았으므로 실패하지 않는다.
    PlacedRange range;
    range.resource = resource;
    range.offset = offset;
    range.bytes = bytes;
    auto pushed = record->ranges.TryPushBack(range);
    (void)pushed;
    ++record->placedCount;
}

void D3D12Device::ForgetPlaced(SlotHandle heap, SlotHandle resource) noexcept {
    TransientHeapRecord* record = heaps_.Find(heap);
    if (record == nullptr) {
        return;
    }

    for (usize index = 0; index < record->ranges.Size(); ++index) {
        if (record->ranges[index].resource == resource) {
            auto removed = record->ranges.TryRemoveAtSwap(index);
            (void)removed;
            return;
        }
    }
}

bool D3D12Device::ActivatePlaced(SlotHandle heap, SlotHandle resource,
                                 ID3D12Resource* object) noexcept {
    TransientHeapRecord* record = heaps_.Find(heap);
    if (record == nullptr) {
        return false;
    }

    PlacedRange* acquired = nullptr;
    for (PlacedRange& range : record->ranges) {
        if (range.resource == resource) {
            acquired = &range;
            break;
        }
    }

    // 이미 활성이면 그 뒤 이 구간을 다른 자원이 쓰지 않았으므로 메모리가 그대로다.
    if (acquired == nullptr || acquired->active) {
        return false;
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    barrier.Aliasing.pResourceBefore = nullptr;
    barrier.Aliasing.pResourceAfter = object;
    list_->ResourceBarrier(1, &barrier);
    for (PlacedRange& range : record->ranges) {
        if (&range != acquired && PlacedRangesOverlap(range, *acquired)) {
            range.active = false;
        }
    }

    acquired->active = true;
    placedActivated_ = true;
    return true;
}

void D3D12Device::AcquireTexture(TextureRecord& record, SlotHandle slot, bool cleared) noexcept {
    // compute 목록은 렌더 타깃·깊이 상태와 그 초기화를 받지 않는다. 렌더 그래프는 async 자원의
    // 획득을 graphics queue에서 낸다.
    if (computeRecording_) {
        ++ignoredDraws_;
        return;
    }

    if (!ActivatePlaced(record.heap, slot, record.resource.Get())) {
        return;
    }

    // 새로 활성이 된 렌더 타깃·깊이 placed 자원은 쓰기 전에 Clear·Discard·Copy로 초기화해야 하고
    // Discard는 RENDER_TARGET·DEPTH_WRITE 상태를 요구한다. 자원 전체가 정의되지 않았으므로 전체를
    // 버린다(획득 범위가 일부여도). 패스 clear가 전체를 덮으면 그것이 초기화다(RenderBase 8절).
    if (cleared) {
        return;
    }

    // 자원 flag와 같은 판정이다(TextureResourceFor: 렌더 타깃이 아닌 깊이 형식은 DEPTH_STENCIL).
    const PixelFormatKind formatKind = GetPixelFormatInfo(record.format).kind;
    const bool target = record.renderTarget;
    const bool depth = !target && (formatKind == PixelFormatKind::Depth ||
                                   formatKind == PixelFormatKind::DepthStencil);
    if (!target && !depth) {
        return;
    }

    TrackTransition(record, slot, {0, record.mipLevels, 0, record.slices},
                    target ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_DEPTH_WRITE);
    list_->DiscardResource(record.resource.Get(), nullptr);
}

void D3D12Device::AcquireBuffer(const BufferRecord& record, SlotHandle slot) noexcept {
    if (computeRecording_) {
        ++ignoredDraws_;
        return;
    }

    (void)ActivatePlaced(record.heap, slot, record.resource.Get());
}

void D3D12Device::CheckAcquireBefore(const ResourceTransition& transition) noexcept {
    // 획득 전이의 before는 Discard다(이전 내용이 없다). 어기면 검증 오류로 세고 획득은 그대로 한다.
    if (validation_ && transition.before != ResourceAccess::Discard) {
        ++validationErrors_;
        auto failed = FailCode<void>(ErrorCode::InvalidArgument,
                                     "acquire transition needs the Discard before-state");
        (void)failed;
    }
}

} // namespace rocks::graphics::d3d12
