#include "ObjectRegistryInternal.h"

#include <Base/Compiler.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/PlatformMath.h>

// ObjectRegistry 클러스터다(GC-S7, Docs/Design/GarbageCollectionScale.ko.md 13절). 클러스터 장부
// (소속·inCount·outCount·소속/detached/경계 목록), 가입 지정·생성·가입·이탈·해체, 직렬 marker의
// 클러스터 표식·처리, 레코드 표와 작업 큐 용량, Debug 장부 검증을 둔다. 병렬 marker의 클러스터
// 처리와 sweep·cutoff 규칙은 ObjectRegistryGc.cpp에 있다.
//
// 장부 기준(13.4절): 소유자 X가 클러스터 C의 소속이면 X의 강한 간선(Trace 간선과 outer)을 센다 —
// 대상이 C의 소속이거나 C의 detached면 inCount[대상], 그 밖이면 outCount[X]. 소유자가 소속이 아니면
// 장부가 없다. 패키지 고리 간선과 소속 간선 자체는 세지 않는다. 관찰자 기록 기준과는 따로다.
namespace rocks {

using namespace registry_detail;

namespace {

constexpr uint32 ClusterTableTag = 0x4743434cu; // 'GCCL'
// 레코드 표의 첫 용량이다.
constexpr uint32 MinimumClusterCapacity = 16;

uint64 BitOf(uint32 index) noexcept {
    return uint64(1) << (index % SlotsPerChunk);
}

uint64 LoadMark(const uint64& word) noexcept {
    return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&word);
}

void StoreMark(uint64& word, uint64 value) noexcept {
    platform::AtomicStore<platform::MemoryOrder::Relaxed>(&word, value);
}

uint32 LoadEpoch(const uint32& epoch) noexcept {
    return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&epoch);
}

void StoreEpoch(uint32& epoch, uint32 value) noexcept {
    platform::AtomicStore<platform::MemoryOrder::Relaxed>(&epoch, value);
}

} // namespace

bool ObjectRegistry::IsClusterMarkedLocked(uint32 id) const noexcept {
    return LoadEpoch(clusterEpochs_[id]) == gcEpoch_;
}

bool ObjectRegistry::IsClusterMemberLocked(const Slot& owner) const noexcept {
    // clustered 비트는 장부 상태의 ClusterMember와 같다(가입·이탈·해체가 함께 바꾼다 — Debug 장부
    // 검증이 대조한다). 참조 쓰기마다 부르므로 장부 칸 대신 비트 묶음을 읽는다.
    return clustersEnabled_ && (BitsOf(owner.index).clustered & BitOf(owner.index)) != 0;
}

void ObjectRegistry::LinkCellLocked(uint32& head, uint32 slot, bool boundary) noexcept {
    ClusterCell& cell = CellOf(slot);
    uint32& next = boundary ? cell.boundaryNext : cell.listNext;
    uint32& previous = boundary ? cell.boundaryPrev : cell.listPrev;
    next = head;
    previous = NoSlot;
    if (head != NoSlot) {
        ClusterCell& first = CellOf(head);
        (boundary ? first.boundaryPrev : first.listPrev) = slot;
    }

    head = slot;
}

void ObjectRegistry::UnlinkCellLocked(uint32& head, uint32 slot, bool boundary) noexcept {
    ClusterCell& cell = CellOf(slot);
    uint32& next = boundary ? cell.boundaryNext : cell.listNext;
    uint32& previous = boundary ? cell.boundaryPrev : cell.listPrev;
    if (previous != NoSlot) {
        ClusterCell& before = CellOf(previous);
        (boundary ? before.boundaryNext : before.listNext) = next;
    } else {
        head = next;
    }

    if (next != NoSlot) {
        ClusterCell& after = CellOf(next);
        (boundary ? after.boundaryPrev : after.listPrev) = previous;
    }

    next = NoSlot;
    previous = NoSlot;
}

void ObjectRegistry::LedgerEdgeLocked(uint32 owner, uint32 target, bool add) noexcept {
    // 분류는 소속이면 chunk 클러스터 표로 끝낸다(장부 칸은 바꿀 계수가 든 칸 하나만 읽는다). 소속이
    // 아닌 대상만 detached인지 대상 칸의 상태를 본다.
    const uint32 id = MemberClusterOf(owner);
    const bool member = (BitsOf(target).clustered & BitOf(target)) != 0;
    const bool inside =
        member ? MemberClusterOf(target) == id : ClusterStateOf(target) == (ClusterDetached | id);
    if (inside) {
        uint32& count = ClusterInCountOf(target);
        if (add) {
            ++count;
            return;
        }

        if (count == 0) {
            platform::Fatal("Cluster ledger in-count underflow");
        }

        // detached는 그 클러스터 소속 객체에서 오는 간선이 없어지면 클러스터 없음이 된다.
        if (--count == 0 && !member) {
            UnlinkCellLocked(clusters_[id].detachedHead, target, false);
            ClearClusterSlotLocked(target);
        }

        return;
    }

    ClusterCell& from = CellOf(owner);
    if (add) {
        if (from.outCount++ == 0) {
            LinkCellLocked(clusters_[id].boundaryHead, owner, true);
            ++clusters_[id].boundaryMembers;
        }

        return;
    }

    if (from.outCount == 0) {
        platform::Fatal("Cluster ledger out-count underflow");
    }

    if (--from.outCount == 0) {
        UnlinkCellLocked(clusters_[id].boundaryHead, owner, true);
        --clusters_[id].boundaryMembers;
    }
}

void ObjectRegistry::ClearClusterSlotLocked(uint32 slot) noexcept {
    CellOf(slot) = ClusterCell{};
    ClusterInCountOf(slot) = 0;
}

void ObjectRegistry::LedgerEditLocked(const Slot& owner, ObjectHandle removed,
                                      ObjectHandle added) noexcept {
    if (!IsClusterMemberLocked(owner)) {
        return;
    }

    if (!removed.IsNull()) {
        LedgerEdgeLocked(owner.index, removed.slot, false);
    }

    if (!added.IsNull()) {
        LedgerEdgeLocked(owner.index, added.slot, true);
    }
}

void ObjectRegistry::LedgerClearLocked(const Slot& owner, const ObjectHandle* values,
                                       usize count) noexcept {
    if (!IsClusterMemberLocked(owner)) {
        return;
    }

    for (usize index = 0; index < count; ++index) {
        if (!values[index].IsNull()) {
            LedgerEdgeLocked(owner.index, values[index].slot, false);
        }
    }
}

void ObjectRegistry::LedgerRemoveVisited(void* pointer, ObjectHandle handle) noexcept {
    auto& state = *static_cast<MarkState*>(pointer);
    if (!handle.IsNull()) {
        state.registry->LedgerEdgeLocked(state.ledgerOwner, handle.slot, false);
    }
}

void ObjectRegistry::LeaveVisited(void* pointer, ObjectHandle handle) noexcept {
    auto& state = *static_cast<MarkState*>(pointer);
    if (!handle.IsNull()) {
        platform::LockGuard lock(state.registry->gate_);
        state.registry->LedgerEdgeLocked(state.ledgerOwner, handle.slot, false);
    }
}

void ObjectRegistry::LeaveTrace(Slot& slot) noexcept {
    // 처리 표시 아래라 간선·Outer가 바뀌지 않는다. 수동 ops의 Trace를 잠금 안에서 부르지 않으려고
    // 간선마다 잠근다(Discard 관찰자 집계와 같은 방식).
    MarkState state{this, nullptr};
    state.ledgerOwner = slot.index;
    TraceVisitor visitor(&state, &ObjectRegistry::LeaveVisited);
    slot.type.Ops().trace(*slot.object, visitor);
    platform::LockGuard lock(gate_);
    const uint32 outer = HotOf(slot.index).outer;
    if (outer != NoSlot) {
        LedgerEdgeLocked(slot.index, outer, false);
    }
}

void ObjectRegistry::FinishLeaveLocked(Slot& slot) noexcept {
    const uint32 index = slot.index;
    ClusterCell& cell = CellOf(index);
    const uint32 id = ClusterStateOf(index) & ClusterIdMask;
    ClusterRecord& record = clusters_[id];
    if (cell.outCount != 0) {
        platform::Fatal("Cluster member left with outgoing ledger edges");
    }

    UnlinkCellLocked(record.memberHead, index, false);
    --record.members;
    --tally_.clusterMembers;
    BitsOf(index).clustered &= ~BitOf(index);
    const uint32 incoming = ClusterInCountOf(index);
    ClearClusterSlotLocked(index);
    if (incoming != 0) {
        // 같은 클러스터 소속 객체가 아직 가리킨다: 계수를 그대로 옮긴 detached다.
        ClusterStateOf(index) = ClusterDetached | id;
        ClusterInCountOf(index) = incoming;
        LinkCellLocked(record.detachedHead, index, false);
    }

    // 13.6절: Marking 중 이탈은 클러스터가 표식됐으면 객체를 shade한다(보수적 — 다음 cycle에
    // 회수). Sweeping이면 남은 클러스터는 모두 표식됐으므로 mark를 켠다.
    if (gcPhase_ == GcPhase::Marking && IsClusterMarkedLocked(id)) {
        if (ShadeLocked(HandleOf(slot)) != ErrorCode::None) {
            platform::Fatal("Preallocated cluster leave shade queue exhausted");
        }
    } else if (gcPhase_ == GcPhase::Sweeping) {
        GcBits& bits = BitsOf(index);
        StoreMark(bits.mark, LoadMark(bits.mark) | BitOf(index));
    }
}

void ObjectRegistry::DissolveClusterLocked(uint32 id) noexcept {
    ClusterRecord& record = clusters_[id];
    const bool cycle = gcPhase_ == GcPhase::Marking || gcPhase_ == GcPhase::Sweeping;
    const bool marked = cycle && IsClusterMarkedLocked(id);
    // 13.6절: 표식됐지만 아직 처리하지 않은 클러스터(장벽이나 slice 예산으로 큐에 남은 상태)는
    // 처리를 대신한다 — 경계 소속을 trace 항목으로 넣고 detached를 shade한다. 해체 뒤 이 항목은
    // 일반 trace다.
    if (gcPhase_ == GcPhase::Marking && marked && LoadEpoch(record.processedEpoch) != gcEpoch_) {
        for (uint32 at = record.boundaryHead; at != NoSlot; at = CellOf(at).boundaryNext) {
            GcBits& bits = BitsOf(at);
            const uint64 mark = LoadMark(bits.mark);
            if ((mark & BitOf(at)) == 0) {
                StoreMark(bits.mark, mark | BitOf(at));
                if (!gcShade_.TryPushBack(at)) {
                    platform::Fatal("Preallocated cluster dissolve queue exhausted");
                }
            }
        }

        for (uint32 at = record.detachedHead; at != NoSlot; at = CellOf(at).listNext) {
            if (MarkIndex(at, gcShade_, cycleGc_.marked) != ErrorCode::None) {
                platform::Fatal("Preallocated cluster dissolve queue exhausted");
            }
        }
    }

    // 소속을 비우고 클러스터 표식 상태를 mark로 옮긴다(표식됐으면 안쪽 간선의 대상이 모두 산다).
    for (uint32 at = record.memberHead; at != NoSlot;) {
        const uint32 next = CellOf(at).listNext;
        ClearClusterSlotLocked(at);
        GcBits& bits = BitsOf(at);
        bits.clustered &= ~BitOf(at);
        if (marked) {
            StoreMark(bits.mark, LoadMark(bits.mark) | BitOf(at));
        }

        at = next;
    }

    for (uint32 at = record.detachedHead; at != NoSlot;) {
        const uint32 next = CellOf(at).listNext;
        ClearClusterSlotLocked(at);
        at = next;
    }

    tally_.clusterMembers -= record.members;
    --tally_.clusters;
    record.members = 0;
    record.memberHead = NoSlot;
    record.boundaryHead = NoSlot;
    record.boundaryMembers = 0;
    record.detachedHead = NoSlot;
    record.live = false;
    record.dissolved = true;
    // Marking 중에는 큐에 이 클러스터 항목이 남았을 수 있어 큐가 빌 때(cutoff·취소)까지 재사용하지
    // 않는다.
    if (gcPhase_ == GcPhase::Marking) {
        record.nextFree = clusterDissolved_;
        clusterDissolved_ = id;
    } else {
        record.nextFree = clusterFree_;
        clusterFree_ = id;
        ++clusterFreeCount_;
    }
}

bool ObjectRegistry::IsPermanentLocked(uint32 slot) const noexcept {
    return permanentCluster_ != NoSlot &&
           ClusterStateOf(slot) == (ClusterMember | permanentCluster_);
}

Result<void> ObjectRegistry::CheckPermanentEdgeLocked(const Slot& owner,
                                                      ObjectHandle target) const noexcept {
    // 비교 하나다(Release에서도 거부한다 — 영구 객체는 trace하지 않으므로 일반 대상을 살리지
    // 못한다).
    if (target.IsNull() || !IsPermanentLocked(owner.index) || IsPermanentLocked(target.slot)) {
        return Result<void>::Success();
    }

    return Result<void>::Failure(ErrorCode::Unsupported);
}

Result<void> ObjectRegistry::ClosePermanentRegion(const ObjectAccessContext& access) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    platform::LockGuard lock(gate_);
    if (auto open = OpenState(closed_, processing_); !open) {
        return open;
    }

    permanentOpen_ = false;
    return Result<void>::Success();
}

void ObjectRegistry::AdvanceGcEpochLocked() noexcept {
    ++gcEpoch_;
    if (gcEpoch_ == 0) {
        // epoch가 한 바퀴 돌았다: 옛 cycle의 epoch가 새 cycle과 같아 보이지 않게 비운다.
        WrapGcEpochLocked();
    }

    // 영구 영역은 언제나 표식된 클러스터다(marker는 epoch 하나만 비교한다).
    if (permanentCluster_ != NoSlot) {
        clusterEpochs_[permanentCluster_] = gcEpoch_;
    }
}

void ObjectRegistry::WrapGcEpochLocked() noexcept {
    for (uint32 id = 0; id < clusterUsed_; ++id) {
        clusterEpochs_[id] = 0;
        clusters_[id].processedEpoch = 0;
    }

    gcEpoch_ = 1;
}

void ObjectRegistry::DissolveUnmarkedClustersLocked() noexcept {
    for (uint32 id = 0; id < clusterUsed_; ++id) {
        if (clusters_[id].live && !IsClusterMarkedLocked(id)) {
            DissolveClusterLocked(id);
        }
    }
}

uint32 ObjectRegistry::FlattenBoundaryClustersLocked() noexcept {
    // 패키지 클러스터만 대상이다. 엔진이 지정한 클러스터(World Level)는 가입을 엔진이 정하므로
    // 남기고, 영구 영역은 표식·sweep을 하지 않는 것이 목적이다.
    uint32 flattened = 0;
    for (uint32 id = 0; id < clusterUsed_; ++id) {
        if (clusters_[id].live && ExceedsFlattenThreshold(clusters_[id])) {
            DissolveClusterLocked(id);
            ++flattened;
        }
    }

    return flattened;
}

bool ObjectRegistry::ExceedsFlattenThreshold(const ClusterRecord& record) const noexcept {
    if (record.designated || record.permanent) {
        return false;
    }

    return uint64(record.boundaryMembers) * 100 >
           uint64(record.members) * ClusterFlattenBoundaryPercent;
}

bool ObjectRegistry::IsFlattenedPackageLocked(const Slot& package) const noexcept {
    // 패키지 정책에서 살아 있는 패키지 객체는 언제나 자기 클러스터의 루트다. 소속이 아니면
    // 안전장치가 해체했다(파괴 요청·cutoff로 해체된 패키지는 Alive가 아니거나 cutoff다).
    return package.state == SlotState::Alive && !IsCutoff(&package) &&
           (ClusterStateOf(package.index) & ClusterMember) == 0;
}

void ObjectRegistry::DissolveAllClustersLocked() noexcept {
    for (uint32 id = 0; id < clusterUsed_; ++id) {
        if (clusters_[id].live) {
            DissolveClusterLocked(id);
        }
    }
}

void ObjectRegistry::ReleaseDissolvedClustersLocked() noexcept {
    while (clusterDissolved_ != NoSlot) {
        const uint32 id = clusterDissolved_;
        ClusterRecord& record = clusters_[id];
        clusterDissolved_ = record.nextFree;
        record.nextFree = clusterFree_;
        clusterFree_ = id;
        ++clusterFreeCount_;
    }
}

uint32 ObjectRegistry::CreateClusterLocked(uint32 root, bool designated) noexcept {
    uint32 id;
    if (clusterFree_ != NoSlot) {
        id = clusterFree_;
        clusterFree_ = clusters_[id].nextFree;
    } else {
        if (clusterUsed_ == clusterCapacity_) {
            platform::Fatal("Cluster record table was not reserved");
        }

        id = clusterUsed_++;
    }

    --clusterFreeCount_;
    ClusterRecord& record = clusters_[id];
    record = ClusterRecord{};
    record.live = true;
    record.root = root;
    record.designated = designated;
    // 13.5절: Sweeping 중 만든 클러스터는 표식된 상태로 시작한다(아니면 소속이 cutoff로 보인다).
    clusterEpochs_[id] = gcPhase_ == GcPhase::Sweeping ? gcEpoch_ : 0;

    ++tally_.clusters;
    if (root != NoSlot) {
        JoinClusterLocked(id, root);
    }

    return id;
}

void ObjectRegistry::JoinClusterLocked(uint32 id, uint32 slot) noexcept {
    ClusterRecord& record = clusters_[id];
    ClearClusterSlotLocked(slot);
    ClusterStateOf(slot) = ClusterMember | id;
    LinkCellLocked(record.memberHead, slot, false);
    ++record.members;
    ++tally_.clusterMembers;
    GcBits& bits = BitsOf(slot);
    const uint64 bit = BitOf(slot);
    SetChunkClusterLocked(slot, id, bits.clustered & ~bit);
    bits.clustered |= bit;
}

void ObjectRegistry::SetChunkClusterLocked(uint32 slot, uint32 id, uint64 others) noexcept {
    // chunk 클러스터 표를 맞춘다(GcBits·ClusterChunk 주석): 이미 있는 칸이면 그 칸, 비었거나 남은
    // 소속이 고르지 않는 칸은 이 클러스터가 차지한다. 두 칸이 차면 넓은 표로 옮기고, 넓은 표도 차면
    // 섞임이다.
    GcBits& bits = BitsOf(slot);
    const uint32 local = slot % SlotsPerChunk;
    const uint64 bit = uint64(1) << local;
    const uint32 key = id + 1;
    if (others == 0) {
        bits.firstCluster = key;
        bits.secondCluster = 0;
        bits.clusterSecond = 0;
        return;
    }

    if (bits.firstCluster != ChunkClusterOverflow) {
        if (bits.firstCluster == key) {
            bits.clusterSecond &= ~bit;
            return;
        }

        if (bits.secondCluster == key) {
            bits.clusterSecond |= bit;
            return;
        }

        if ((others & ~bits.clusterSecond) == 0) {
            bits.firstCluster = key;
            bits.clusterSecond &= ~bit;
            return;
        }

        if ((others & bits.clusterSecond) == 0) {
            bits.secondCluster = key;
            bits.clusterSecond |= bit;
            return;
        }

        // 셋째 클러스터: 남은 소속을 넓은 표로 옮긴다(두 칸 → 넓은 표의 0·1번 칸).
        ClusterChunk& wide = ClusterChunkOf(slot);
        wide = ClusterChunk{};
        wide.clusters[0] = bits.firstCluster;
        wide.clusters[1] = bits.secondCluster;
        for (uint64 rest = others; rest != 0; rest &= rest - 1) {
            const uint32 member = platform::PlatformMath::CountTrailingZeros64(rest);
            wide.SetSelector(member, (bits.clusterSecond >> member & 1) != 0 ? 1u : 0u);
        }

        bits.firstCluster = ChunkClusterOverflow;
        bits.clusterSecond = 0;
        bits.secondCluster = 0;
    }

    ClusterChunk& chunk = ClusterChunkOf(slot);
    if (chunk.clusters[0] == ChunkClusterOverflow) {
        // 섞인 chunk의 소속은 장부 상태로 찾는다(표를 바꾸지 않는다).
        return;
    }

    for (uint32 entry = 0; entry < ChunkClusterEntries; ++entry) {
        if (chunk.clusters[entry] == key) {
            chunk.SetSelector(local, entry);
            return;
        }
    }

    uint32 used = 0;
    for (uint64 rest = others; rest != 0; rest &= rest - 1) {
        used |= 1u << chunk.Selector(platform::PlatformMath::CountTrailingZeros64(rest));
    }

    for (uint32 entry = 0; entry < ChunkClusterEntries; ++entry) {
        if ((used & (1u << entry)) == 0) {
            chunk.clusters[entry] = key;
            chunk.SetSelector(local, entry);
            return;
        }
    }

    chunk.clusters[0] = ChunkClusterOverflow;
}

uint64 ObjectRegistry::GcQueueBound(uint64 slots) const noexcept {
    // 슬롯마다 일반 항목 하나(mark 비트로 한 번 — 경계 소속 항목 포함)와 클러스터마다 항목 하나다.
    return slots + clusterCapacity_;
}

Result<void> ObjectRegistry::ReserveGcQueues(uint64 needed, uint64 limit) noexcept {
    needed = needed < limit ? needed : limit;
    Array<uint32>* const queues[] = {&gcWork_, &gcShade_};
    for (Array<uint32>* queue : queues) {
        if (needed <= queue->Capacity()) {
            continue;
        }

        uint64 capacity = uint64(queue->Capacity()) * 2;
        capacity = capacity > limit ? limit : capacity;
        capacity = capacity < needed ? needed : capacity;
        if (auto reserved = queue->TryReserve(static_cast<usize>(capacity)); !reserved) {
            return reserved;
        }
    }

    return Result<void>::Success();
}

Result<void> ObjectRegistry::ReserveClusters(uint32 count) noexcept {
    if (count <= clusterFreeCount_) {
        return Result<void>::Success();
    }

    const uint64 needed = uint64(clusterCapacity_) + (count - clusterFreeCount_);
    uint64 capacity = clusterCapacity_ == 0 ? MinimumClusterCapacity : uint64(clusterCapacity_) * 2;
    capacity = capacity < needed ? needed : capacity;
    capacity =
        capacity > RegistryOptions::ClusterSlotLimit ? RegistryOptions::ClusterSlotLimit : capacity;
    if (capacity < needed) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    // Marking 중이면 늘어난 클러스터 항목을 담을 작업 큐부터 늘린다(장벽은 할당하지 않는다).
    if (gcPhase_ == GcPhase::Marking) {
        const uint64 slots = uint64(chunkCount_) * SlotsPerChunk;
        const uint64 limit = uint64(options_.maxSlots) + capacity;
        if (auto reserved = ReserveGcQueues(slots + capacity, limit); !reserved) {
            return reserved;
        }
    }

    auto memory = allocator_.Allocate(sizeof(ClusterRecord) * static_cast<usize>(capacity),
                                      alignof(ClusterRecord), ClusterTableTag);
    if (!memory) {
        return Result<void>::Failure(memory.Error());
    }

    auto* grown = static_cast<ClusterRecord*>(memory.Value());
    for (uint32 index = 0; index < capacity; ++index) {
        ::new (&grown[index]) ClusterRecord();
    }

    auto epochs = allocator_.Allocate(sizeof(uint32) * static_cast<usize>(capacity),
                                      alignof(uint32), ClusterTableTag);
    if (!epochs) {
        ReleaseRegistryMemory(allocator_, grown);
        return Result<void>::Failure(epochs.Error());
    }

    auto* grownEpochs = static_cast<uint32*>(epochs.Value());
    for (uint32 index = 0; index < capacity; ++index) {
        grownEpochs[index] = 0;
    }

    ClusterRecord* previous = nullptr;
    uint32* previousEpochs = nullptr;
    {
        // 레코드는 잠금 안(장벽)과 처리 표시 아래(marker)에서만 읽는다. 지금은 처리 표시 아래다.
        platform::LockGuard lock(gate_);
        for (uint32 index = 0; index < clusterUsed_; ++index) {
            grown[index] = clusters_[index];
            grownEpochs[index] = clusterEpochs_[index];
        }

        previous = clusters_;
        previousEpochs = clusterEpochs_;
        clusters_ = grown;
        clusterEpochs_ = grownEpochs;
        clusterFreeCount_ += static_cast<uint32>(capacity) - clusterCapacity_;
        clusterCapacity_ = static_cast<uint32>(capacity);
    }

    ReleaseRegistryMemory(allocator_, previous);
    ReleaseRegistryMemory(allocator_, previousEpochs);
    return Result<void>::Success();
}

ErrorCode ObjectRegistry::MarkCluster(uint32 id, Array<uint32>& work, uint32& marked) noexcept {
    if (LoadEpoch(clusterEpochs_[id]) == gcEpoch_) {
        return ErrorCode::None;
    }

    StoreEpoch(clusterEpochs_[id], gcEpoch_);
    // 표식 수는 살아남는 객체 수로 센다(소속은 따로 표식하지 않는다).
    marked += clusters_[id].members;
    auto pushed = work.TryPushBack(id | ClusterItem);
    return pushed ? ErrorCode::None : pushed.Error();
}

void ObjectRegistry::ProcessCluster(uint32 id, MarkState& state) noexcept {
    ClusterRecord& record = clusters_[id];
    // 큐에 든 뒤 해체됐다(해체가 처리를 대신했다).
    if (record.dissolved) {
        return;
    }

    const uint64 started = state.counters != nullptr ? platform::MonotonicNanoseconds() : 0;
    const usize queued = state.work->Size();
    const uint32 marked = state.marked;

    // 경계 소속을 trace 항목으로 넣는다(mark 비트로 한 번만 — 꺼낼 때 소속이 아니어도 일반 trace와
    // 같다). 고리 안에 커서를 두고 slice를 넘기지 않는다(처리는 한 번에 끝난다).
    for (uint32 at = record.boundaryHead; at != NoSlot; at = CellOf(at).boundaryNext) {
        GcBits& bits = BitsOf(at);
        const uint64 mark = LoadMark(bits.mark);
        if ((mark & BitOf(at)) == 0) {
            StoreMark(bits.mark, mark | BitOf(at));
            if (!state.work->TryPushBack(at)) {
                state.error = ErrorCode::CapacityExceeded;
                return;
            }
        }
    }

    for (uint32 at = record.detachedHead; at != NoSlot; at = CellOf(at).listNext) {
        const ErrorCode result = MarkIndex(at, *state.work, state.marked);
        if (result != ErrorCode::None) {
            state.error = result;
            return;
        }
    }

    StoreEpoch(record.processedEpoch, gcEpoch_);
    if (state.counters != nullptr) {
        ++state.counters->clusterItems;
        state.counters->detachedMarks += state.marked - marked;
        state.counters->boundaryPushes += state.work->Size() - queued;
        state.counters->clusterProcessNanoseconds += platform::MonotonicNanoseconds() - started;
    }
}

Result<bool> ObjectRegistry::InspectClusterMembership(const ObjectAccessContext& access,
                                                      ObjectHandle object) const noexcept {
    auto lifecycle = InspectLifecycle(access, object);
    if (!lifecycle) {
        return Result<bool>::Failure(lifecycle.Error());
    }

    // 조회와 같은 잠금 구간이 아니어도 된다(결과는 어차피 반환 뒤 바뀔 수 있다 — 소유 스레드만
    // 바꾼다).
    platform::LockGuard lock(gate_);
    const Slot* slot = FindSlot(object);
    return Result<bool>::Success(slot != nullptr && IsClusterMemberLocked(*slot));
}

Result<void> ObjectRegistry::DesignateCluster(const ObjectAccessContext& access, uint64 transaction,
                                              ObjectHandle object, ObjectHandle root) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    platform::LockGuard lock(gate_);
    if (auto open = OpenState(closed_, processing_); !open) {
        return open;
    }

    // 패키지 정책 Registry는 패키지 클러스터를 스스로 만들고, 영구 창 안의 객체는 모두 영구다
    // (13.3절).
    if (!clustersEnabled_ || options_.policy.packageNativeType != nullptr || permanentOpen_) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    Slot* slot = FindSlot(object);
    if (!slot || slot->state != SlotState::Reserved || ColdOf(*slot).transaction != transaction ||
        slot->object == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    uint32& state = ClusterStateOf(slot->index);
    if (state != 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const bool complete = slot->type.Ops().completeFieldLayout;
    if (root.IsNull()) {
        if (!complete) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        state = ClusterPending;
        ClusterInCountOf(slot->index) = NoSlot;
        return Result<void>::Success();
    }

    Slot* rooted = FindSlot(root);
    if (!rooted || !IsJoinableRootLocked(rooted->index, transaction)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    // 소속 조건(완전한 필드 배치)을 못 맞추는 타입은 클러스터 밖 일반 객체로 남는다.
    if (complete) {
        state = ClusterPending;
        ClusterInCountOf(slot->index) = rooted->index;
    }

    return Result<void>::Success();
}

bool ObjectRegistry::IsJoinableRootLocked(uint32 root, uint64 transaction) const noexcept {
    const Slot* slot = SlotAt(root);
    if (slot == nullptr) {
        return false;
    }

    const uint32 state = ClusterStateOf(root);
    if (slot->state == SlotState::Reserved) {
        // 같은 트랜잭션에서 루트로 지정한 객체다.
        return ColdOf(*slot).transaction == transaction && state == ClusterPending &&
               ClusterInCountOf(root) == NoSlot;
    }

    if (slot->state != SlotState::Alive || IsCutoff(slot) || (state & ClusterMember) == 0) {
        return false;
    }

    const ClusterRecord& record = clusters_[state & ClusterIdMask];
    return record.live && !record.dissolved && record.root == root;
}

Result<void> ObjectRegistry::VerifyClusterLedger() noexcept {
    if constexpr (!IsDebugBuild) {
        return Result<void>::Success();
    } else {
        if (!clustersEnabled_ || !options_.verifyClusterLedger) {
            return Result<void>::Success();
        }

        // 처리 표시 아래라 장부·간선이 바뀌지 않는다(다른 스레드는 루트·Borrow 계수만 바꾼다).
        const uint32 slots = chunkCount_ * SlotsPerChunk;
        Array<uint32> incoming(allocator_);
        Array<uint32> outgoing(allocator_);
        if (auto sized = incoming.TryResize(slots); !sized) {
            return sized;
        }

        if (auto sized = outgoing.TryResize(slots); !sized) {
            return sized;
        }

        for (uint32 index = 0; index < slots; ++index) {
            incoming[index] = 0;
            outgoing[index] = 0;
        }

        // (1) 목록과 비트가 칸과 맞는다.
        uint32 members = 0;
        uint32 live = 0;
        for (uint32 id = 0; id < clusterUsed_; ++id) {
            const ClusterRecord& record = clusters_[id];
            if (!record.live) {
                continue;
            }

            ++live;
            uint32 counted = 0;
            bool rootListed = record.root == NoSlot;
            for (uint32 at = record.memberHead; at != NoSlot; at = CellOf(at).listNext) {
                const Slot* slot = SlotAt(at);
                if (ClusterStateOf(at) != (ClusterMember | id) ||
                    (BitsOf(at).clustered & BitOf(at)) == 0 || slot->state != SlotState::Alive ||
                    ++counted > slots) {
                    platform::Fatal("Cluster member list diverged from its ledger cells");
                }

                rootListed = rootListed || at == record.root;
            }

            if (counted != record.members || !rootListed) {
                platform::Fatal("Cluster member count or root diverged from its member list");
            }

            members += counted;
            uint32 boundary = 0;
            for (uint32 at = record.boundaryHead; at != NoSlot; at = CellOf(at).boundaryNext) {
                if (ClusterStateOf(at) != (ClusterMember | id) || CellOf(at).outCount == 0 ||
                    ++boundary > record.members) {
                    platform::Fatal("Cluster boundary list holds a non-boundary object");
                }
            }

            if (boundary != record.boundaryMembers) {
                platform::Fatal("Cluster boundary count diverged from its boundary list");
            }

            for (uint32 at = record.detachedHead; at != NoSlot; at = CellOf(at).listNext) {
                if (ClusterStateOf(at) != (ClusterDetached | id) || ClusterInCountOf(at) == 0 ||
                    (BitsOf(at).clustered & BitOf(at)) != 0) {
                    platform::Fatal("Cluster detached list holds a non-detached object");
                }
            }
        }

        if (members != tally_.clusterMembers || live != tally_.clusters) {
            platform::Fatal("Cluster statistics diverged from the cluster records");
        }

        // (2) 소속 객체의 간선을 다시 센다(Trace 간선과 outer).
        struct Recount {
            ObjectRegistry* registry;
            uint32 owner;
            uint32 id;
            Array<uint32>* incoming;
            Array<uint32>* outgoing;
        };

        auto visit = [](void* pointer, ObjectHandle handle) noexcept {
            auto& recount = *static_cast<Recount*>(pointer);
            if (handle.IsNull()) {
                return;
            }

            const uint32 state = recount.registry->ClusterStateOf(handle.slot);
            if (state == (ClusterMember | recount.id) || state == (ClusterDetached | recount.id)) {
                ++(*recount.incoming)[handle.slot];
            } else {
                ++(*recount.outgoing)[recount.owner];
            }
        };
        for (uint32 index = 0; index < slots; ++index) {
            const uint32 state = ClusterStateOf(index);
            const bool clustered = (BitsOf(index).clustered & BitOf(index)) != 0;
            if (clustered != ((state & ClusterMember) != 0)) {
                platform::Fatal("Cluster bit diverged from its ledger cell");
            }

            if (clustered && MemberClusterOf(index) != (state & ClusterIdMask)) {
                platform::Fatal("Chunk cluster hint diverged from the ledger");
            }

            if ((state & ClusterMember) == 0) {
                continue;
            }

            Recount recount{this, index, state & ClusterIdMask, &incoming, &outgoing};
            TraceVisitor visitor(&recount, +visit);
            const GcHot& hot = HotOf(index);
            gcTypes_[hot.type].trace(*hot.object, visitor);
            if (hot.outer != NoSlot) {
                visit(&recount, HandleOf(*SlotAt(hot.outer)));
            }
        }

        // (3) 비교와 소속 불변식(지정 클러스터의 루트 아닌 소속은 inCount ≥ 1 — 13.8절).
        for (uint32 index = 0; index < slots; ++index) {
            const ClusterCell& cell = CellOf(index);
            const uint32 state = ClusterStateOf(index);
            const bool counted = (state & (ClusterMember | ClusterDetached)) != 0;
            const uint32 inCount = ClusterInCountOf(index);
            if (state == 0 && (inCount != 0 || cell.outCount != 0)) {
                platform::Fatal("Cleared cluster ledger cell kept counts");
            }

            if (counted ? (inCount != incoming[index] || cell.outCount != outgoing[index])
                        : incoming[index] != 0) {
                platform::Fatal("Cluster ledger diverged from a recount");
            }

            if ((state & ClusterMember) != 0) {
                const ClusterRecord& record = clusters_[state & ClusterIdMask];
                if (options_.verifyClusterOwnership && record.designated && record.root != index &&
                    inCount == 0) {
                    platform::Fatal("Designated cluster member is not owned inside its cluster");
                }
            }
        }

        return Result<void>::Success();
    }
}

} // namespace rocks
