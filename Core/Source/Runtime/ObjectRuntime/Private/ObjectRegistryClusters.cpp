#include "ObjectRegistryInternal.h"

#include <Base/Compiler.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>

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
    const ClusterRecord& record = clusters_[id];
    return record.permanent || LoadEpoch(record.markEpoch) == gcEpoch_;
}

bool ObjectRegistry::IsClusterMemberLocked(const Slot& owner) const noexcept {
    return clustersEnabled_ && (CellOf(owner.index).state & ClusterMember) != 0;
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
    ClusterCell& from = CellOf(owner);
    const uint32 id = from.state & ClusterIdMask;
    ClusterRecord& record = clusters_[id];
    ClusterCell& to = CellOf(target);
    if (to.state == (ClusterMember | id) || to.state == (ClusterDetached | id)) {
        if (add) {
            ++to.inCount;
            return;
        }

        if (to.inCount == 0) {
            platform::Fatal("Cluster ledger in-count underflow");
        }

        // detached는 그 클러스터 소속 객체에서 오는 간선이 없어지면 클러스터 없음이 된다.
        if (--to.inCount == 0 && (to.state & ClusterDetached) != 0) {
            UnlinkCellLocked(record.detachedHead, target, false);
            to = ClusterCell{};
        }

        return;
    }

    if (add) {
        if (from.outCount++ == 0) {
            LinkCellLocked(record.boundaryHead, owner, true);
        }

        return;
    }

    if (from.outCount == 0) {
        platform::Fatal("Cluster ledger out-count underflow");
    }

    if (--from.outCount == 0) {
        UnlinkCellLocked(record.boundaryHead, owner, true);
    }
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
    const uint32 id = cell.state & ClusterIdMask;
    ClusterRecord& record = clusters_[id];
    if (cell.outCount != 0) {
        platform::Fatal("Cluster member left with outgoing ledger edges");
    }

    UnlinkCellLocked(record.memberHead, index, false);
    --record.members;
    --tally_.clusterMembers;
    BitsOf(index).clustered &= ~BitOf(index);
    const uint32 incoming = cell.inCount;
    cell = ClusterCell{};
    if (incoming != 0) {
        // 같은 클러스터 소속 객체가 아직 가리킨다: 계수를 그대로 옮긴 detached다.
        cell.state = ClusterDetached | id;
        cell.inCount = incoming;
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
        ClusterCell& cell = CellOf(at);
        const uint32 next = cell.listNext;
        cell = ClusterCell{};
        GcBits& bits = BitsOf(at);
        bits.clustered &= ~BitOf(at);
        if (marked) {
            StoreMark(bits.mark, LoadMark(bits.mark) | BitOf(at));
        }

        at = next;
    }

    for (uint32 at = record.detachedHead; at != NoSlot;) {
        ClusterCell& cell = CellOf(at);
        const uint32 next = cell.listNext;
        cell = ClusterCell{};
        at = next;
    }

    tally_.clusterMembers -= record.members;
    --tally_.clusters;
    record.members = 0;
    record.memberHead = NoSlot;
    record.boundaryHead = NoSlot;
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
    return permanentCluster_ != NoSlot && CellOf(slot).state == (ClusterMember | permanentCluster_);
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
    if (++gcEpoch_ != 0) {
        return;
    }

    // epoch가 한 바퀴 돌았다: 옛 cycle의 epoch가 새 cycle과 같아 보이지 않게 비운다.
    for (uint32 id = 0; id < clusterUsed_; ++id) {
        clusters_[id].markEpoch = 0;
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
    if (gcPhase_ == GcPhase::Sweeping) {
        record.markEpoch = gcEpoch_;
    }

    ++tally_.clusters;
    if (root != NoSlot) {
        JoinClusterLocked(id, root);
    }

    return id;
}

void ObjectRegistry::JoinClusterLocked(uint32 id, uint32 slot) noexcept {
    ClusterRecord& record = clusters_[id];
    ClusterCell& cell = CellOf(slot);
    cell = ClusterCell{};
    cell.state = ClusterMember | id;
    LinkCellLocked(record.memberHead, slot, false);
    ++record.members;
    ++tally_.clusterMembers;
    BitsOf(slot).clustered |= BitOf(slot);
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

    ClusterRecord* previous = nullptr;
    {
        // 레코드는 잠금 안(장벽)과 처리 표시 아래(marker)에서만 읽는다. 지금은 처리 표시 아래다.
        platform::LockGuard lock(gate_);
        for (uint32 index = 0; index < clusterUsed_; ++index) {
            grown[index] = clusters_[index];
        }

        previous = clusters_;
        clusters_ = grown;
        clusterFreeCount_ += static_cast<uint32>(capacity) - clusterCapacity_;
        clusterCapacity_ = static_cast<uint32>(capacity);
    }

    ReleaseRegistryMemory(allocator_, previous);
    return Result<void>::Success();
}

ErrorCode ObjectRegistry::MarkCluster(uint32 id, Array<uint32>& work, uint32& marked) noexcept {
    ClusterRecord& record = clusters_[id];
    if (record.permanent || LoadEpoch(record.markEpoch) == gcEpoch_) {
        return ErrorCode::None;
    }

    StoreEpoch(record.markEpoch, gcEpoch_);
    // 표식 수는 살아남는 객체 수로 센다(소속은 따로 표식하지 않는다).
    marked += record.members;
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
    if (!slot || slot->state != SlotState::Reserved || slot->transaction != transaction ||
        slot->object == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    ClusterCell& cell = CellOf(slot->index);
    if (cell.state != 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const bool complete = slot->type.Ops().completeFieldLayout;
    if (root.IsNull()) {
        if (!complete) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        cell.state = ClusterPending;
        cell.inCount = NoSlot;
        return Result<void>::Success();
    }

    Slot* rooted = FindSlot(root);
    if (!rooted || !IsJoinableRootLocked(rooted->index, transaction)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    // 소속 조건(완전한 필드 배치)을 못 맞추는 타입은 클러스터 밖 일반 객체로 남는다.
    if (complete) {
        cell.state = ClusterPending;
        cell.inCount = rooted->index;
    }

    return Result<void>::Success();
}

bool ObjectRegistry::IsJoinableRootLocked(uint32 root, uint64 transaction) const noexcept {
    const Slot* slot = SlotAt(root);
    if (slot == nullptr) {
        return false;
    }

    const ClusterCell& cell = CellOf(root);
    if (slot->state == SlotState::Reserved) {
        // 같은 트랜잭션에서 루트로 지정한 객체다.
        return slot->transaction == transaction && cell.state == ClusterPending &&
               cell.inCount == NoSlot;
    }

    if (slot->state != SlotState::Alive || IsCutoff(slot) || (cell.state & ClusterMember) == 0) {
        return false;
    }

    const ClusterRecord& record = clusters_[cell.state & ClusterIdMask];
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
                if (CellOf(at).state != (ClusterMember | id) ||
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
            for (uint32 at = record.boundaryHead; at != NoSlot; at = CellOf(at).boundaryNext) {
                if (CellOf(at).state != (ClusterMember | id) || CellOf(at).outCount == 0) {
                    platform::Fatal("Cluster boundary list holds a non-boundary object");
                }
            }

            for (uint32 at = record.detachedHead; at != NoSlot; at = CellOf(at).listNext) {
                if (CellOf(at).state != (ClusterDetached | id) || CellOf(at).inCount == 0 ||
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

            const uint32 state = recount.registry->CellOf(handle.slot).state;
            if (state == (ClusterMember | recount.id) || state == (ClusterDetached | recount.id)) {
                ++(*recount.incoming)[handle.slot];
            } else {
                ++(*recount.outgoing)[recount.owner];
            }
        };
        for (uint32 index = 0; index < slots; ++index) {
            const ClusterCell& cell = CellOf(index);
            const bool clustered = (BitsOf(index).clustered & BitOf(index)) != 0;
            if (clustered != ((cell.state & ClusterMember) != 0)) {
                platform::Fatal("Cluster bit diverged from its ledger cell");
            }

            if ((cell.state & ClusterMember) == 0) {
                continue;
            }

            Recount recount{this, index, cell.state & ClusterIdMask, &incoming, &outgoing};
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
            const bool counted = (cell.state & (ClusterMember | ClusterDetached)) != 0;
            if (cell.state == 0 && (cell.inCount != 0 || cell.outCount != 0)) {
                platform::Fatal("Cleared cluster ledger cell kept counts");
            }

            if (counted ? (cell.inCount != incoming[index] || cell.outCount != outgoing[index])
                        : incoming[index] != 0) {
                platform::Fatal("Cluster ledger diverged from a recount");
            }

            if ((cell.state & ClusterMember) != 0) {
                const ClusterRecord& record = clusters_[cell.state & ClusterIdMask];
                if (options_.verifyClusterOwnership && record.designated && record.root != index &&
                    cell.inCount == 0) {
                    platform::Fatal("Designated cluster member is not owned inside its cluster");
                }
            }
        }

        return Result<void>::Success();
    }
}

} // namespace rocks
