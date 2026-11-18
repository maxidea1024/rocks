#include "ObjectRegistryGcInternal.h"

#include <Base/Compiler.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Task/WorkerScheduler.h>

// ObjectRegistry GC의 병렬 표식이다(GC-S4 — 블록 단위 지역 큐·공유 풀·소유 스레드 큐).
namespace rocks {

using namespace registry_detail;

// 병렬 표식 참여자 하나(worker task 또는 소유 스레드)의 지역 상태다.
struct ObjectRegistry::MarkParticipant {
    ParallelMark* shared = nullptr;
    // 지역 큐: 블록 더미의 맨 위 블록에서 넣고 뺀다. 맨 위 아래의 블록은 언제나 가득 차 있다.
    GcBlock* stack = nullptr;
    // worker가 만난 수동 Trace(parallelTraceSafe가 아닌) 객체를 소유 스레드로 보내려고 모으는
    // 블록이다.
    GcBlock* owner = nullptr;
    uint32 marked = 0;
    uint32 traced = 0;
    // 공유 trace 수에 아직 더하지 않은 수다(예산 검사 간격).
    uint32 pending = 0;
    bool isOwner = false;
    // 지역 큐에서 꺼낸 항목을 처리 전에 두는 FIFO 미리 읽기 고리다(GC 설계 16.1절). 지역 큐는
    // LIFO라 몇 칸 아래 항목이 곧 꺼낼 항목이 아니다 — 고리가 미리 읽기 거리를 큐 순서와 무관하게
    // 고정한다. 고리의 항목은 이 참여자의 회색 객체이고 다른 참여자는 훔치지 않는다.
    uint32 ring[MarkRingSlots] = {};
    uint32 ringHead = 0;
    uint32 ringCount = 0;
};

// 한 slice(또는 Collect)의 병렬 표식이다(GC-S4, Docs/Design/GarbageCollectionScale.ko.md).
// 참여자는 worker task W개와 소유 스레드다. 공유 풀은 512칸 블록 목록이고 잠금 하나(gate)로 지킨다
// (설계의 "블록 풀 하나를 잠금으로 보호하는 단순한 방식"). 참여자는 지역 큐가 비면 소유 스레드 큐
// (소유 스레드만)와 공유 풀에서 블록을 가져오고, 둘 다 비면 idle로 기다린다. 모든 참여자가 idle이고
// 두 풀이 비면 끝이다(새 회색 객체는 idle이 아닌 참여자만 만들고, 수동 Trace 객체는 idle 전에
// 넘긴다). 예산(객체 수·시간)이나 오류는 stop을 세워 모두 멈추고 남은 블록을 leftovers로 모은다.
struct ObjectRegistry::ParallelMark {
    ObjectRegistry* registry = nullptr;
    platform::Mutex gate;
    platform::ConditionVariable changed;
    GcBlock* pool = nullptr;
    GcBlock* ownerPool = nullptr;
    GcBlock* leftovers = nullptr;
    GcBlock* freeBlocks = nullptr;
    // 풀의 블록 수·idle 참여자 수다(잠금 안에서 쓰고 밖에서는 relaxed로 읽어 나눔 판단에만 쓴다).
    uint32 poolBlocks = 0;
    uint32 idle = 0;
    uint32 participants = 0;
    bool done = false;
    // atomic: 멈춤 요청, 첫 오류(ErrorCode), 모든 참여자의 trace 수.
    uint32 stop = 0;
    uint32 error = 0;
    uint32 traced = 0;
    uint32 maxObjects = 0;
    uint64 deadline = 0;
    // 예산 검사 간격(참여자마다 trace한 객체 수)이다. 기본 64, 객체 예산이 그보다 작으면 그 값이다.
    uint32 budgetInterval = 64;

    bool Stopped() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&stop) != 0;
    }

    void RequestStop() noexcept {
        platform::AtomicStore<platform::MemoryOrder::Relaxed>(&stop, 1u);
        platform::LockGuard lock(gate);
        changed.NotifyAll();
    }

    void SetError(ErrorCode code) noexcept {
        uint32 expected = 0;
        (void)platform::AtomicCompareExchangeStrong<platform::MemoryOrder::Relaxed,
                                                    platform::MemoryOrder::Relaxed>(
            &error, expected, static_cast<uint32>(code));
        RequestStop();
    }

    GcBlock* TakeBlock() noexcept {
        {
            platform::LockGuard lock(gate);
            if (GcBlock* block = freeBlocks) {
                freeBlocks = block->next;
                block->next = nullptr;
                block->count = 0;
                return block;
            }
        }

        // 여러 worker가 동시에 부를 수 있다(Registry allocator의 다중 스레드 계약 — StepCollection
        // 주석).
        auto memory = registry->allocator_.Allocate(sizeof(GcBlock), alignof(GcBlock), GcBlockTag);
        if (!memory) {
            SetError(memory.Error());
            return nullptr;
        }

        return ::new (memory.Value()) GcBlock();
    }

    void ReleaseBlock(GcBlock* block) noexcept {
        platform::LockGuard lock(gate);
        block->next = freeBlocks;
        freeBlocks = block;
    }

    void Publish(GcBlock* block) noexcept {
        platform::LockGuard lock(gate);
        block->next = pool;
        pool = block;
        platform::AtomicStore<platform::MemoryOrder::Relaxed>(&poolBlocks, poolBlocks + 1);
        changed.NotifyOne();
    }

    void PublishOwner(GcBlock* block) noexcept {
        platform::LockGuard lock(gate);
        block->next = ownerPool;
        ownerPool = block;
        // 기다리는 참여자 중 소유 스레드를 깨워야 한다(누가 소유 스레드인지 골라 깨울 수 없다).
        changed.NotifyAll();
    }

    // 지역 큐에 넣는다. 맨 위 블록이 가득 차면 풀이 모자랄 때(참여자 수보다 적을 때) 그 블록을
    // 풀에 내놓고 새 블록을 받는다.
    void Push(MarkParticipant& self, uint32 index) noexcept {
        PushBlock(self, index, true);
    }

    // 고리를 되돌릴 때 쓴다(멈춘 뒤라 풀에 내놓지 않는다).
    void PushStack(MarkParticipant& self, uint32 index) noexcept {
        PushBlock(self, index, false);
    }

    void PushBlock(MarkParticipant& self, uint32 index, bool share) noexcept {
        GcBlock* top = self.stack;
        if (top == nullptr || top->count == GcBlock::Capacity) {
            if (share && top != nullptr &&
                platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&poolBlocks) < participants) {
                self.stack = top->next;
                Publish(top);
            }

            GcBlock* block = TakeBlock();
            if (block == nullptr) {
                return;
            }

            block->next = self.stack;
            self.stack = block;
            top = block;
        }

        top->items[top->count++] = index;
    }

    void PushOwner(MarkParticipant& self, uint32 index) noexcept {
        if (self.owner == nullptr || self.owner->count == GcBlock::Capacity) {
            if (self.owner != nullptr) {
                PublishOwner(self.owner);
            }

            self.owner = TakeBlock();
            if (self.owner == nullptr) {
                return;
            }
        }

        self.owner->items[self.owner->count++] = index;
    }

    // 지역 큐의 맨 위 항목을 꺼낸다(빈 블록은 반환한다).
    bool PopStack(MarkParticipant& self, uint32& index) noexcept {
        while (GcBlock* top = self.stack) {
            if (top->count != 0) {
                index = top->items[--top->count];
                return true;
            }

            self.stack = top->next;
            ReleaseBlock(top);
        }

        return false;
    }

    // 처리할 항목을 고리 앞에서 꺼낸다. 고리를 지역 큐에서 채우며 들어오는 항목의 hot 레코드를,
    // 꺼낼 때 ObjectPrefetchDistance칸 뒤 항목의 본체와 ArrayPrefetchDistance칸 뒤 항목의 참조 배열
    // 버퍼를 미리 읽는다(직렬 marker의 큐 앞쪽 미리 읽기와 같은 단계). 클러스터 항목(ClusterItem)은
    // 슬롯이 아니라 미리 읽지 않는다. 고리와 지역 큐가 모두 비면 거짓이다.
    bool Pop(MarkParticipant& self, uint32& index) noexcept {
        constexpr uint32 Mask = MarkRingSlots - 1;
        uint32 item = 0;
        while (self.ringCount < MarkRingSlots && PopStack(self, item)) {
            self.ring[(self.ringHead + self.ringCount) & Mask] = item;
            ++self.ringCount;
            if ((item & ClusterItem) == 0) {
                platform::PrefetchRead(&registry->HotOf(item));
            }
        }

        if (self.ringCount == 0) {
            return false;
        }

        index = self.ring[self.ringHead];
        self.ringHead = (self.ringHead + 1) & Mask;
        --self.ringCount;
        if (ObjectPrefetchDistance < self.ringCount) {
            const uint32 ahead = self.ring[(self.ringHead + ObjectPrefetchDistance) & Mask];
            if ((ahead & ClusterItem) == 0) {
                platform::PrefetchRead(registry->HotOf(ahead).object);
            }
        }

        if (ArrayPrefetchDistance < self.ringCount) {
            const uint32 ahead = self.ring[(self.ringHead + ArrayPrefetchDistance) & Mask];
            if ((ahead & ClusterItem) == 0) {
                registry->PrefetchArrays(ahead);
            }
        }

        return true;
    }

    // 멈출 때 고리의 항목을 지역 큐로 되돌린다(남은 블록과 함께 다음 slice 몫이 된다 — 회색 객체를
    // 잃지 않는다). 블록을 얻지 못하면 SetError가 멈춤 원인을 남긴다.
    void ReturnRing(MarkParticipant& self) noexcept {
        while (self.ringCount != 0) {
            const uint32 item = self.ring[self.ringHead];
            self.ringHead = (self.ringHead + 1) & (MarkRingSlots - 1);
            --self.ringCount;
            PushStack(self, item);
        }
    }

    // 클러스터를 표식한다(atomic 비교 교환 — 이긴 참여자만 클러스터 항목을 넣는다).
    void MarkCluster(MarkParticipant& self, uint32 id) noexcept {
        uint32& slot = registry->clusterEpochs_[id];
        const uint32 epoch = registry->gcEpoch_;
        uint32 expected = platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&slot);
        if (expected == epoch ||
            !platform::AtomicCompareExchangeStrong<platform::MemoryOrder::Relaxed,
                                                   platform::MemoryOrder::Relaxed>(&slot, expected,
                                                                                   epoch)) {
            return;
        }

        self.marked += registry->clusters_[id].members;
        Push(self, id | ClusterItem);
    }

    // 클러스터 처리(병렬): 경계 소속을 지역 큐에 넣고(mark 비트 fetch_or로 한 번) detached를
    // 표식한다. slice 동안 목록은 바뀌지 않고 한 클러스터는 이긴 참여자 하나만 처리한다.
    void ProcessCluster(MarkParticipant& self, uint32 id) noexcept {
        ClusterRecord& record = registry->clusters_[id];
        if (record.dissolved) {
            return;
        }

        for (uint32 at = record.boundaryHead; at != NoSlot;
             at = registry->CellOf(at).boundaryNext) {
            const uint64 bit = BitOf(at);
            GcBits& bits = registry->BitsOf(at);
            if ((LoadMark(bits.mark) & bit) == 0 &&
                (platform::AtomicFetchOr<platform::MemoryOrder::Relaxed>(&bits.mark, bit) & bit) ==
                    0) {
                Push(self, at);
            }
        }

        for (uint32 at = record.detachedHead; at != NoSlot; at = registry->CellOf(at).listNext) {
            const ErrorCode marked = Mark(self, at);
            if (marked != ErrorCode::None) {
                SetError(marked);
                return;
            }
        }

        platform::AtomicStore<platform::MemoryOrder::Relaxed>(&record.processedEpoch,
                                                              registry->gcEpoch_);
    }

    // 표식한다(atomic fetch_or — 이긴 참여자만 지역 큐에 넣는다). 잎은 Outer 사슬을 바로 따라간다.
    ErrorCode Mark(MarkParticipant& self, uint32 index) noexcept {
        for (;;) {
            GcBits& bits = registry->BitsOf(index);
            const uint64 bit = BitOf(index);
            if ((bits.live & bit) == 0) {
                return ErrorCode::StaleHandle;
            }

            if ((bits.clustered & bit) != 0) {
                // 이번 cycle에 클러스터 표식을 본 소속이면 비트 하나로 끝난다(경합해도 같은 비트를
                // 켤 뿐이다).
                uint64& seen = bits.clusterSeen;
                if ((LoadMark(seen) & bit) == 0) {
                    (void)platform::AtomicFetchOr<platform::MemoryOrder::Relaxed>(&seen, bit);
                    MarkCluster(self, registry->MemberClusterOf(index));
                }

                return ErrorCode::None;
            }

            if ((LoadMark(bits.mark) & bit) != 0 ||
                (platform::AtomicFetchOr<platform::MemoryOrder::Relaxed>(&bits.mark, bit) & bit) !=
                    0) {
                return ErrorCode::None;
            }

            ++self.marked;
            if ((bits.leaf & bit) == 0) {
                Push(self, index);
                return ErrorCode::None;
            }

            index = registry->HotOf(index).outer;
            if (index == NoSlot) {
                return ErrorCode::None;
            }
        }
    }

    static void MarkBatchShared(void* pointer, const ObjectHandle* handles, uint32 count) noexcept {
        auto& self = *static_cast<MarkParticipant*>(pointer);
        ParallelMark& shared = *self.shared;
        ObjectRegistry& registry = *shared.registry;
        const uint64 slots = uint64(registry.chunkCount_) * SlotsPerChunk;
        for (uint32 index = 0; index < count; ++index) {
            if (handles[index].slot < slots) {
                platform::PrefetchRead(&registry.BitsOf(handles[index].slot));
            }
        }

        for (uint32 index = 0; index < count; ++index) {
            if (!registry.IsMarkableEdge(handles[index], slots)) {
                shared.SetError(ErrorCode::StaleHandle);
                return;
            }

            const ErrorCode marked = shared.Mark(self, handles[index].slot);
            if (marked != ErrorCode::None) {
                shared.SetError(marked);
                return;
            }
        }
    }

    void Trace(MarkParticipant& self, uint32 index, TraceVisitor& visitor) noexcept {
        const GcHot& hot = registry->HotOf(index);
        registry->VisitStrongEdges(registry->gcTypes_[hot.type], *hot.object, visitor);
        visitor.Flush();
        if (hot.outer != NoSlot) {
            const ErrorCode outer = Mark(self, hot.outer);
            if (outer != ErrorCode::None) {
                SetError(outer);
                return;
            }
        }

        // 패키지 정책의 자식 고리는 slice 동안 바뀌지 않아 여러 참여자가 읽어도 경합이 없다.
        if (registry->options_.policy.packageNativeType != nullptr) {
            for (const Slot* child = registry->ColdOf(*registry->SlotAt(index)).firstChild;
                 child != nullptr; child = registry->ColdOf(*child).nextSibling) {
                const ErrorCode marked = Mark(self, child->index);
                if (marked != ErrorCode::None) {
                    SetError(marked);
                    return;
                }
            }
        }
    }

    // trace 수를 공유 계수에 더하고 객체 수·시간 예산을 본다.
    void ChargeBudget(MarkParticipant& self) noexcept {
        const uint32 total =
            platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&traced, self.pending) +
            self.pending;
        self.pending = 0;
        if (total >= maxObjects ||
            (deadline != 0 && platform::MonotonicNanoseconds() >= deadline)) {
            RequestStop();
        }
    }

    // 지역 큐가 비었다: 소유 스레드 큐(소유 스레드만)나 공유 풀에서 블록을 받는다. 받으면 true,
    // 끝이나 멈춤이면 false다.
    bool Refill(MarkParticipant& self) noexcept {
        platform::LockGuard lock(gate);
        for (;;) {
            if (Stopped() || done) {
                return false;
            }

            GcBlock** source = self.isOwner && ownerPool != nullptr ? &ownerPool
                               : pool != nullptr                    ? &pool
                                                                    : nullptr;
            if (source != nullptr) {
                GcBlock* block = *source;
                *source = block->next;
                if (source == &pool) {
                    platform::AtomicStore<platform::MemoryOrder::Relaxed>(&poolBlocks,
                                                                          poolBlocks - 1);
                }

                block->next = self.stack;
                self.stack = block;
                return true;
            }

            platform::AtomicStore<platform::MemoryOrder::Relaxed>(&idle, idle + 1);
            // 모두 idle이고 두 풀이 비었다: 회색 객체가 더 생길 수 없다. worker가 소유 스레드 큐를
            // 남긴 채 idle이 되면(소유 스레드가 아직 깨어나 잠금을 다시 잡기 전) 끝이 아니다.
            if (idle == participants && pool == nullptr && ownerPool == nullptr) {
                done = true;
                changed.NotifyAll();
                return false;
            }

            (void)changed.Wait(gate);
            platform::AtomicStore<platform::MemoryOrder::Relaxed>(&idle, idle - 1);
        }
    }

    // 참여자 하나의 표식 루프다.
    void Run(MarkParticipant& self) noexcept {
        TraceVisitor visitor(&self, &ParallelMark::MarkBatchShared);
        for (;;) {
            uint32 index = 0;
            while (!Stopped() && Pop(self, index)) {
                if ((index & ClusterItem) != 0) {
                    ProcessCluster(self, index & ~ClusterItem);
                } else if (!self.isOwner &&
                           !registry->gcTypes_[registry->HotOf(index).type].parallelTraceSafe) {
                    PushOwner(self, index);
                    continue;
                } else {
                    Trace(self, index, visitor);
                }

                ++self.traced;
                if (++self.pending == budgetInterval) {
                    ChargeBudget(self);
                    // 다른 참여자가 기다리고 지역 큐에 가득 찬 블록이 더 있으면 하나 내놓는다.
                    if (platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&idle) != 0 &&
                        self.stack != nullptr && self.stack->next != nullptr) {
                        GcBlock* spare = self.stack->next;
                        self.stack->next = spare->next;
                        Publish(spare);
                    }
                }
            }

            // idle 전에 소유 스레드로 보낼 객체를 넘긴다(끝 판정이 이 블록을 놓치지 않게).
            if (self.owner != nullptr) {
                if (self.owner->count != 0) {
                    PublishOwner(self.owner);
                } else {
                    ReleaseBlock(self.owner);
                }

                self.owner = nullptr;
            }

            if (Stopped() || !Refill(self)) {
                break;
            }
        }

        if (self.pending != 0) {
            ChargeBudget(self);
        }

        // 멈췄으면 고리의 항목을 지역 큐로 되돌리고 남은 지역 블록을 다음 slice 몫으로 모은다.
        ReturnRing(self);
        platform::LockGuard lock(gate);
        while (GcBlock* block = self.stack) {
            self.stack = block->next;
            block->next = leftovers;
            leftovers = block;
        }
    }
};

void ObjectRegistry::FlattenGrayBlocks() noexcept {
    while (GcBlock* block = gcGrayBlocks_) {
        gcGrayBlocks_ = block->next;
        for (uint32 item = 0; item < block->count; ++item) {
            // 표식 큐는 모든 슬롯을 담을 용량이 있다(회색 객체는 cycle마다 슬롯당 한 번이다).
            if (!gcWork_.TryPushBack(block->items[item])) {
                platform::Fatal("Preallocated GC work queue exhausted");
            }
        }

        block->next = gcFreeBlocks_;
        gcFreeBlocks_ = block;
    }
}

void ObjectRegistry::ReleaseGrayBlocks() noexcept {
    while (GcBlock* block = gcGrayBlocks_) {
        gcGrayBlocks_ = block->next;
        block->count = 0;
        block->next = gcFreeBlocks_;
        gcFreeBlocks_ = block;
    }
}

uint32 ObjectRegistry::MarkParallel(Array<uint32>& work, usize& cursor, MarkState& state,
                                    uint32 maxObjects, uint64 deadline, WorkerScheduler& scheduler,
                                    bool& completed, GcBlock** carried,
                                    GcSliceTimes* times) noexcept {
    completed = false;
    const uint64 setupStarted = platform::MonotonicNanoseconds();
    constexpr uint32 MaxWorkers = 64;
    const uint32 workers =
        scheduler.Stats().workerCount < MaxWorkers ? scheduler.Stats().workerCount : MaxWorkers;
    ParallelMark shared;
    shared.registry = this;
    shared.participants = workers + 1;
    shared.maxObjects = maxObjects;
    shared.deadline = deadline;
    shared.budgetInterval = maxObjects < shared.budgetInterval ? maxObjects : shared.budgetInterval;
    shared.freeBlocks = gcFreeBlocks_;
    gcFreeBlocks_ = nullptr;
    MarkParticipant participants[MaxWorkers + 1];
    for (uint32 index = 0; index <= workers; ++index) {
        participants[index].shared = &shared;
    }

    participants[0].isOwner = true;
    // 씨앗(work[cursor..))을 블록으로 묶어 공유 풀에 둔다. 큐는 비우고 끝에서 남은 것을 다시
    // 담는다.
    for (usize at = cursor; at < work.Size();) {
        GcBlock* block = shared.TakeBlock();
        if (block == nullptr) {
            break;
        }

        while (at < work.Size() && block->count < GcBlock::Capacity) {
            block->items[block->count++] = work[at++];
        }

        block->next = shared.pool;
        shared.pool = block;
        ++shared.poolBlocks;
    }

    work.Clear();
    cursor = 0;
    // 앞 slice가 남긴 회색 블록은 펴지 않고 그대로 풀에 붙인다.
    if (carried != nullptr) {
        while (GcBlock* block = *carried) {
            *carried = block->next;
            block->next = shared.pool;
            shared.pool = block;
            ++shared.poolBlocks;
        }
    }

    WorkerTaskHandle tasks[MaxWorkers];
    uint32 submitted = 0;
    if (!shared.Stopped()) {
        for (; submitted < workers; ++submitted) {
            MarkParticipant* self = &participants[submitted + 1];
            auto body = Function<void(WorkerTaskContext&)>::TryCreate(
                allocator_, [&shared, self](WorkerTaskContext&) noexcept { shared.Run(*self); });
            if (!body) {
                shared.SetError(body.Error());
                break;
            }

            auto accepted = scheduler.Submit(Move(body.Value()));
            if (!accepted) {
                shared.SetError(accepted.Error());
                break;
            }

            tasks[submitted] = Move(accepted.Value());
        }

        if (submitted != workers) {
            // 제출하지 못한 참여자는 끝 판정 수에서 뺀다(이미 멈춤이 걸려 모두 곧 나온다).
            platform::LockGuard lock(shared.gate);
            shared.participants = submitted + 1;
            shared.changed.NotifyAll();
        }
    }

    const uint64 markStarted = platform::MonotonicNanoseconds();
    shared.Run(participants[0]);
    const uint64 drainStarted = platform::MonotonicNanoseconds();
    for (uint32 index = 0; index < submitted; ++index) {
        auto waited = tasks[index].Wait();
        if (!waited || waited.Value() != platform::WaitResult::Signaled) {
            // stack에 남은 표식 상태를 쓰는 worker를 버리고 복귀할 수 없다.
            platform::Fatal("Cannot drain a GC marker task");
        }

        auto status = tasks[index].Status();
        if (!status) {
            shared.SetError(status.Error());
        } else if (status.Value().state != WorkerTaskState::Succeeded) {
            shared.SetError(status.Value().error == ErrorCode::None ? ErrorCode::DependencyFailed
                                                                    : status.Value().error);
        }
    }

    const uint64 requeueStarted = platform::MonotonicNanoseconds();
    // 모든 참여자가 끝났다. 남은 회색 객체(멈춤)를 큐에 다시 담고 블록을 재사용 목록으로 돌린다.
    uint32 traced = 0;
    for (uint32 index = 0; index <= workers; ++index) {
        state.marked += participants[index].marked;
        traced += participants[index].traced;
        if (participants[index].owner != nullptr) {
            participants[index].owner->next = shared.leftovers;
            shared.leftovers = participants[index].owner;
        }
    }

    uint64 gray = 0;
    GcBlock* lists[] = {shared.leftovers, shared.pool, shared.ownerPool};
    for (GcBlock* block : lists) {
        while (block != nullptr) {
            GcBlock* next = block->next;
            gray += block->count;
            if (carried != nullptr && block->count != 0) {
                // 블록째 다음 slice로 넘긴다.
                block->next = *carried;
                *carried = block;
                block = next;
                continue;
            }

            for (uint32 item = 0; item < block->count; ++item) {
                // 큐는 모든 슬롯을 담을 용량을 미리 확보했다(회색 객체는 슬롯마다 한 번이다).
                if (!work.TryPushBack(block->items[item])) {
                    platform::Fatal("Preallocated GC work queue exhausted");
                }
            }

            block->next = shared.freeBlocks;
            shared.freeBlocks = block;
            block = next;
        }
    }

    gcFreeBlocks_ = shared.freeBlocks;
    if (times != nullptr) {
        times->setup += markStarted - setupStarted;
        times->marking += drainStarted - markStarted;
        times->drain += requeueStarted - drainStarted;
        times->requeue += platform::MonotonicNanoseconds() - requeueStarted;
        times->grayCarried = static_cast<uint32>(gray);
    }

    const auto error = static_cast<ErrorCode>(shared.error);
    if (error != ErrorCode::None) {
        state.error = error;
        return traced;
    }

    completed = work.IsEmpty() && shared.done && (carried == nullptr || *carried == nullptr);
    return traced;
}

} // namespace rocks
