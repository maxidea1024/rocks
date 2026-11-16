#include "ObjectRegistryInternal.h"

#include <Base/Compiler.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Task/WorkerScheduler.h>

// ObjectRegistry GC다. 전체 수집(Collect)과 증분 수집(Begin·Step·Cancel), 표식(MarkSerial·삽입
// 장벽 ShadeLocked·cutoff 판정), Registry가 방문하는 신원 간선(VisitIdentityEdges),
// sweep(RetireLocked), 회수 대상 목록(Pending·Deferred·Ready)과 회수(ReclaimEligible·
// DrainRetired)를 둔다.
namespace rocks {

using namespace registry_detail;

void ObjectRegistry::VisitIdentityEdges(const Slot& slot, TraceVisitor& visitor) const noexcept {
    visitor.Visit(slot.outer);
    // 패키지 정책: 자식 고리가 export 표다. 처리 표시 아래라 고리가 바뀌지 않고 열린 생성
    // 트랜잭션이 있으면 GC가 진행하지 않으므로 Reserved 자식은 Commit 검증(같은 트랜잭션)에서만
    // 만난다.
    if (options_.policy.packageNativeType != nullptr) {
        for (const Slot* child = slot.firstChild; child != nullptr; child = child->nextSibling) {
            visitor.Visit(HandleOf(*child));
        }
    }
}

namespace {

// 직렬 marker가 시계를 읽는 간격(trace한 객체 수)이다. 시간 예산은 soft deadline이라 이 간격만큼
// 넘을 수 있다.
constexpr uint32 TimeCheckInterval = 128;
// 작업 큐에서 몇 칸 앞의 Slot·객체 본체를 미리 읽는지다(3.4절 모델의 16·8칸). 참조 배열 버퍼는 본체
// 배치를 알아야 하므로(M3) 미리 읽지 않는다.
constexpr usize SlotPrefetchDistance = 16;
constexpr usize ObjectPrefetchDistance = 8;

} // namespace

// 배치 간선 처리의 공통 본문이다. 대상 슬롯의 첫 cache line(세대·상태)과 표식 칸을 먼저 모두 미리
// 읽은 뒤 방문 순서대로 검사·표식·enqueue한다(순서는 간선마다 처리할 때와 같다).
void ObjectRegistry::MarkHandles(MarkState& state, const ObjectHandle* handles,
                                 uint32 count) noexcept {
    for (uint32 index = 0; index < count; ++index) {
        if (const Slot* slot = SlotAt(handles[index].slot)) {
            platform::PrefetchRead(slot);
            platform::PrefetchRead(&slot->markEpoch);
        }
    }

    const uint64 epoch = markEpoch_;
    for (uint32 index = 0; index < count; ++index) {
        Slot* slot = FindSlot(handles[index]);
        if (!slot ||
            !(slot->state == SlotState::Alive || slot->state == SlotState::DestroyRequested)) {
            state.error = ErrorCode::StaleHandle;
            return;
        }

        if (slot->Mark() == epoch) {
            continue;
        }

        // 작업 큐는 모든 슬롯을 담을 용량을 미리 확보했다(Begin·생성·Collect) — 할당하지 않는다.
        auto pushed = state.work->TryPushBack(handles[index]);
        if (!pushed) {
            state.error = pushed.Error();
            return;
        }

        slot->SetMark(epoch);
    }
}

void ObjectRegistry::MarkBatch(void* pointer, const ObjectHandle* handles, uint32 count) noexcept {
    auto& state = *static_cast<MarkState*>(pointer);
    if (state.error == ErrorCode::None) {
        state.registry->MarkHandles(state, handles, count);
    }
}

void ObjectRegistry::MarkBatchLocked(void* pointer, const ObjectHandle* handles,
                                     uint32 count) noexcept {
    auto& state = *static_cast<MarkState*>(pointer);
    platform::LockGuard lock(state.registry->gate_);
    if (state.error == ErrorCode::None) {
        state.registry->MarkHandles(state, handles, count);
    }
}

uint32 ObjectRegistry::MarkSerial(Array<ObjectHandle>& work, usize& cursor, MarkState& state,
                                  uint32 maxObjects, uint64 deadline) noexcept {
    TraceVisitor visitor(&state, &ObjectRegistry::MarkBatch);
    uint32 processed = 0;
    while (cursor < work.Size() && processed < maxObjects && state.error == ErrorCode::None) {
        if (deadline != 0 && processed != 0 && processed % TimeCheckInterval == 0 &&
            platform::MonotonicNanoseconds() >= deadline) {
            break;
        }

        // 미스 사슬(Slot → 본체)을 큐 앞쪽에서 미리 당긴다. 큐의 handle은 표식할 때 검증했다.
        const usize size = work.Size();
        if (cursor + SlotPrefetchDistance < size) {
            if (const Slot* ahead = SlotAt(work[cursor + SlotPrefetchDistance].slot)) {
                platform::PrefetchRead(ahead);
                platform::PrefetchRead(&ahead->outer);
            }
        }

        if (cursor + ObjectPrefetchDistance < size) {
            if (const Slot* ahead = SlotAt(work[cursor + ObjectPrefetchDistance].slot)) {
                platform::PrefetchRead(ahead->object);
            }
        }

        Slot& slot = *FindSlot(work[cursor]);
        ++cursor;
        slot.type.Ops().trace(*slot.object, visitor);
        // inner → outer(패키지 정책이면 outer → inner도)는 Registry가 방문하는 강한 간선이다.
        VisitIdentityEdges(slot, visitor);
        // 객체마다 비워 큐 순서를 간선마다 처리할 때와 같게 둔다.
        visitor.Flush();
        ++processed;
    }

    return processed;
}

bool ObjectRegistry::IsCutoff(const Slot* slot) const noexcept {
    return slot && gcPhase_ == GcPhase::Sweeping &&
           (slot->state == SlotState::Alive || slot->state == SlotState::DestroyRequested) &&
           slot->Mark() != markEpoch_;
}

ErrorCode ObjectRegistry::ShadeLocked(ObjectHandle handle) noexcept {
    if (gcPhase_ != GcPhase::Marking || handle.IsNull()) {
        return ErrorCode::None;
    }

    Slot* slot = FindSlot(handle);
    if (!slot || !(slot->state == SlotState::Alive || slot->state == SlotState::DestroyRequested)) {
        return ErrorCode::StaleHandle;
    }

    if (slot->Mark() == markEpoch_) {
        return ErrorCode::None;
    }

    // Reserve는 Begin/CreatePending에서 gate 밖에 수행한다. barrier가 allocator 콜백을 실행하지
    // 않도록 capacity를 먼저 검사하며 불변식 위반은 실패로 반환한다.
    if (gcWork_.Size() == gcWork_.Capacity()) {
        return ErrorCode::CapacityExceeded;
    }

    auto pushed = gcWork_.TryPushBack(handle);
    if (!pushed) {
        return pushed.Error();
    }

    slot->SetMark(markEpoch_);
    return ErrorCode::None;
}

GcPhase ObjectRegistry::CollectionPhase() const noexcept {
    platform::LockGuard lock(gate_);
    return gcPhase_;
}

Result<void> ObjectRegistry::BeginCollection(const ObjectAccessContext& access) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    const uint64 started = platform::MonotonicNanoseconds();
    usize capacity;
    {
        platform::LockGuard lock(gate_);
        if (processing_ || constructions_ || totalBorrows_ || gcPhase_ != GcPhase::Idle) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        processing_ = true;
        capacity = static_cast<usize>(uint64(chunkCount_) * SlotsPerChunk);
    }

    gcWork_.Clear();
    auto reserved = gcWork_.TryReserve(capacity);
    platform::LockGuard lock(gate_);
    processing_ = false;
    if (!reserved) {
        return reserved;
    }

    if (++markEpoch_ == 0) {
        markEpoch_ = 1;
        for (uint32 chunk = chunkCount_; chunk-- != 0;) {
            for (auto& slot : chunks_[chunk]->slots) {
                slot.SetMark(0);
            }
        }
    }

    gcPhase_ = GcPhase::Marking;
    gcCursor_ = 0;
    cycleGc_ = {};
    sweepChunk_ = 0;
    // 슬롯 순회는 새 chunk부터 옛 chunk로, chunk 안은 낮은 번호부터다(root 표식·sweep·통계 순서).
    for (uint32 chunk = chunkCount_; chunk-- != 0;) {
        for (auto& slot : chunks_[chunk]->slots) {
            if (slot.state == SlotState::Alive || slot.state == SlotState::DestroyRequested) {
                ++cycleGc_.examined;
                if (slot.roots != 0 &&
                    ShadeLocked({domain_, slot.index, slot.generation}) != ErrorCode::None) {
                    platform::Fatal("Preallocated root snapshot queue exhausted");
                }
            }
        }
    }

    cycleGc_.durationNanoseconds = platform::MonotonicNanoseconds() - started;
    cycleGc_.beginNanoseconds = cycleGc_.durationNanoseconds;
    return Result<void>::Success();
}

Result<void> ObjectRegistry::CancelCollection(const ObjectAccessContext& access) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    platform::LockGuard lock(gate_);
    if (processing_ || gcPhase_ == GcPhase::Sweeping || gcPhase_ == GcPhase::Reclaiming) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    gcPhase_ = GcPhase::Idle;
    gcWork_.Clear();
    gcCursor_ = 0;
    return Result<void>::Success();
}

bool ObjectRegistry::IsReclaimable(const Slot& slot) const noexcept {
    // 고리에 남은 자식이 있으면 부모 슬롯을 재사용하지 않는다. white 부모가 자식보다 먼저
    // Retire되면 자식들이 같은 sweep에서 스스로 떨어질 때까지 기다린다(그 사이 DrainRetired 회수
    // 포함 — rollback은 저널의 슬롯만 회수하고, 그 자식은 같은 저널의 뒤쪽이라 먼저 떨어진다).
    return slot.state == SlotState::Retired && slot.roots == 0 && slot.borrows == 0 &&
           slot.backendHolds == 0 && slot.encodePins == 0 && slot.firstChild == nullptr;
}

ObjectRegistry::Slot* ObjectRegistry::NextReclaimableLocked() noexcept {
    // 지연 목록의 슬롯은 회수할 수 없다는 불변식이 있다(회수 가능해지는 순간 WakeDeferredLocked가
    // 풀린 목록으로 옮긴다). 그래서 지연 슬롯은 다시 걷지 않는다.
    if (retiredReady_.head != nullptr) {
        ++reclaimProbes_;
        return retiredReady_.head;
    }

    while (Slot* slot = retiredPending_.head) {
        ++reclaimProbes_;
        if (IsReclaimable(*slot)) {
            return slot;
        }

        UnlinkRetiredLocked(*slot);
        LinkRetiredLocked(*slot, RetiredQueue::Deferred);
    }

    return nullptr;
}

bool ObjectRegistry::HasReclaimableLocked() noexcept {
    return NextReclaimableLocked() != nullptr;
}

void ObjectRegistry::LinkRetiredLocked(Slot& slot, RetiredQueue queue) noexcept {
    RetiredList& list = queue == RetiredQueue::Pending    ? retiredPending_
                        : queue == RetiredQueue::Deferred ? retiredDeferred_
                                                          : retiredReady_;
    slot.queue = queue;
    slot.nextInList = nullptr;
    slot.prevInList = list.tail;
    if (list.tail != nullptr) {
        list.tail->nextInList = &slot;
    } else {
        list.head = &slot;
    }

    list.tail = &slot;
}

void ObjectRegistry::UnlinkRetiredLocked(Slot& slot) noexcept {
    RetiredList& list = slot.queue == RetiredQueue::Pending    ? retiredPending_
                        : slot.queue == RetiredQueue::Deferred ? retiredDeferred_
                                                               : retiredReady_;
    if (slot.prevInList != nullptr) {
        slot.prevInList->nextInList = slot.nextInList;
    } else {
        list.head = slot.nextInList;
    }

    if (slot.nextInList != nullptr) {
        slot.nextInList->prevInList = slot.prevInList;
    } else {
        list.tail = slot.prevInList;
    }

    slot.queue = RetiredQueue::None;
    slot.nextInList = nullptr;
    slot.prevInList = nullptr;
}

void ObjectRegistry::RetireLocked(Slot& slot) noexcept {
    // 패키지 정책 Registry의 객체는 Discard로만 죽는다(1.7절). 패키지 정책에서는 RequestDestroy가
    // 없으므로 Alive는 Discard되지 않았다는 뜻이다. 종료(closed_) 뒤의 회수는 세지 않는다.
    if (slot.state == SlotState::Alive && options_.policy.packageNativeType != nullptr &&
        !closed_) {
        if (tally_.retiredWithoutDiscard != NumericLimits<uint32>::Max()) {
            ++tally_.retiredWithoutDiscard;
        }

        if constexpr (IsDebugBuild) {
            if (options_.assertRetiredWithoutDiscard) {
                platform::Fatal("Package-policy object retired without Discard");
            }
        }
    }

    slot.SetState(tally_, SlotState::Retired);
    DetachIdentityLocked(slot);
    LinkRetiredLocked(slot, RetiredQueue::Pending);
}

void ObjectRegistry::WakeDeferredLocked(Slot& slot) noexcept {
    if (slot.queue == RetiredQueue::Deferred && IsReclaimable(slot)) {
        UnlinkRetiredLocked(slot);
        LinkRetiredLocked(slot, RetiredQueue::Ready);
    }
}

uint32 ObjectRegistry::MarkWithScheduler(MarkState& state, uint32 maxObjects, uint64 deadline,
                                         WorkerScheduler& scheduler) noexcept {
    // worker가 같은 큐에 동시에 넣으므로 표식·enqueue·오류는 배치마다 gate_
    // 아래다(MarkBatchLocked). 병렬 표식의 잠금 제거는 GC-S4다.
    auto setError = [this, &state](ErrorCode error) noexcept {
        platform::LockGuard lock(gate_);
        if (state.error == ErrorCode::None) {
            state.error = error;
        }
    };
    auto hasError = [this, &state]() noexcept {
        platform::LockGuard lock(gate_);
        return state.error != ErrorCode::None;
    };
    auto inBudget = [deadline]() noexcept {
        return deadline == 0 || platform::MonotonicNanoseconds() < deadline;
    };
    uint32 processed = 0;
    while (processed < maxObjects && inBudget() && !hasError()) {
        ObjectHandle next;
        bool parallel = false;
        {
            platform::LockGuard lock(gate_);
            if (gcCursor_ == gcWork_.Size()) {
                break;
            }

            next = gcWork_[gcCursor_];
            parallel = FindSlot(next)->type.Ops().parallelTraceSafe;
        }

        if (!parallel) {
            ++gcCursor_;
            TraceVisitor visitor(&state, &ObjectRegistry::MarkBatchLocked);
            Slot* slot = FindSlot(next);
            slot->type.Ops().trace(*slot->object, visitor);
            VisitIdentityEdges(*slot, visitor);
            visitor.Flush();
            ++processed;
            continue;
        }

        // 한 batch는 최대 64개다. 각 task의 완료는 capture 정리 뒤 관측하고 모두 회수한 뒤에만
        // stack state/visitor를 없앤다. 미선언 Trace는 이 batch와 겹쳐 실행하지 않는다.
        WorkerTaskHandle tasks[64];
        uint32 submitted = 0;
        while (submitted < 64 && processed < maxObjects && inBudget() && !hasError()) {
            ObjectHandle handle;
            {
                platform::LockGuard lock(gate_);
                if (gcCursor_ == gcWork_.Size()) {
                    break;
                }

                handle = gcWork_[gcCursor_];
                if (!FindSlot(handle)->type.Ops().parallelTraceSafe) {
                    break;
                }

                ++gcCursor_;
            }

            auto body = Function<void(WorkerTaskContext&)>::TryCreate(
                allocator_, [this, &state, handle](WorkerTaskContext&) noexcept {
                    TraceVisitor visitor(&state, &ObjectRegistry::MarkBatchLocked);
                    Slot* slot = FindSlot(handle);
                    slot->type.Ops().trace(*slot->object, visitor);
                    // outer는 공개 전에 정해져 바뀌지 않고 고리는 slice(처리 표시) 동안 바뀌지
                    // 않으므로 worker가 읽어도 경합이 없다.
                    VisitIdentityEdges(*slot, visitor);
                    visitor.Flush();
                });
            if (!body) {
                setError(body.Error());
                break;
            }

            auto accepted = scheduler.Submit(Move(body.Value()));
            if (!accepted) {
                setError(accepted.Error());
                break;
            }

            tasks[submitted++] = Move(accepted.Value());
            ++processed;
        }

        for (uint32 index = 0; index < submitted; ++index) {
            auto waited = tasks[index].Wait();
            if (!waited || waited.Value() != platform::WaitResult::Signaled) {
                // stack에 남은 trace context를 사용하는 worker를 버리고 복귀할 수 없다.
                platform::Fatal("Cannot drain a GC marker task");
            }

            auto status = tasks[index].Status();
            if (!status) {
                setError(status.Error());
            } else if (status.Value().state != WorkerTaskState::Succeeded) {
                setError(status.Value().error == ErrorCode::None ? ErrorCode::DependencyFailed
                                                                 : status.Value().error);
            }
        }
    }

    return processed;
}

Result<GcProgress> ObjectRegistry::StepCollection(const ObjectAccessContext& access,
                                                  GcBudget budget,
                                                  WorkerScheduler* scheduler) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<GcProgress>::Failure(valid.Error());
    }

    if (budget.maxObjects == 0) {
        return Result<GcProgress>::Failure(ErrorCode::InvalidArgument);
    }

    if (scheduler && scheduler->IsCurrentWorker()) {
        return Result<GcProgress>::Failure(ErrorCode::WrongThread);
    }

    const uint64 started = platform::MonotonicNanoseconds();
    uint64 deadline = 0;
    if (budget.maxNanoseconds != 0) {
        const uint64 maximum = NumericLimits<uint64>::Max();
        deadline =
            budget.maxNanoseconds > maximum - started ? maximum : started + budget.maxNanoseconds;
    }

    {
        platform::LockGuard lock(gate_);
        if (processing_ || constructions_ || totalBorrows_) {
            return Result<GcProgress>::Failure(ErrorCode::Busy);
        }

        if (gcPhase_ == GcPhase::Idle) {
            return Result<GcProgress>::Failure(ErrorCode::InvalidArgument);
        }

        processing_ = true;
    }

    // 처리 표시 아래에서 gcPhase_·gcWork_·gcCursor_·sweepChunk_는 이 스레드만 바꾼다(장벽·생성·
    // Discard는 Busy, 다른 스레드의 RootRef 복제도 Busy). 그래서 아래 루프는 잠금 없이 읽고, 다른
    // 스레드가 잠금 아래에서 읽는 전이(cutoff 게시·Reclaiming)만 잠가서 쓴다.
    uint32 processed = 0;
    const uint64 markStarted = platform::MonotonicNanoseconds();
    MarkState state{this, &gcWork_};
    bool cutoffPublished = false;
    if (gcPhase_ == GcPhase::Marking) {
        processed = scheduler == nullptr
                        ? MarkSerial(gcWork_, gcCursor_, state, budget.maxObjects, deadline)
                        : MarkWithScheduler(state, budget.maxObjects, deadline, *scheduler);
        // 큐가 빈 것을 이 slice의 예산 안에서 보았을 때만 cutoff를 게시한다(예산을 다 쓴 slice는
        // Marking으로 끝나 다음 slice 전에 mutator가 장벽을 거칠 수 있다).
        if (state.error == ErrorCode::None && gcCursor_ == gcWork_.Size() &&
            processed < budget.maxObjects &&
            (deadline == 0 || platform::MonotonicNanoseconds() < deadline)) {
            // 모든 장벽이 같은 gate에서 게시되고 slice 동안은 장벽이 없으므로(처리 표시) 빈 큐
            // 판정과 cutoff는 하나의 선형화다. 아직 slot을 Retired로 바꾸기 전에도 white 객체의
            // Resolve·Pin· 연결을 차단한다. cutoff를 게시한 slice는 sweep 없이 끝낸다. sweep 단위가
            // chunk라 작은 그래프는 한 slice에 끝나므로, mutator가 cutoff 뒤 아직 분류하지 않은
            // white를 만나는 구간(Sweeping)을 언제나 한 번은
            // 거친다(Docs/Implementation/IncrementalGarbageCollection.ko.md의 cutoff 계약).
            platform::LockGuard lock(gate_);
            cycleGc_.marked = static_cast<uint32>(gcWork_.Size());
            gcPhase_ = GcPhase::Sweeping;
            sweepChunk_ = chunkCount_;
            cutoffPublished = true;
        }
    }

    if (state.error != ErrorCode::None) {
        platform::LockGuard lock(gate_);
        gcPhase_ = GcPhase::Idle;
        gcWork_.Clear();
        gcCursor_ = 0;
        processing_ = false;
        return Result<GcProgress>::Failure(state.error);
    }

    const uint64 sweepStarted = platform::MonotonicNanoseconds();
    cycleGc_.markNanoseconds += sweepStarted - markStarted;
    // sweep의 작업 단위는 chunk(64칸) 하나다(GC-S2). chunk마다 잠금 한 번, 시계 한 번이다. cutoff
    // 뒤 Sweeping 중 생성이 붙인 새 chunk(더 큰 번호)는 방문하지 않는다. 그 객체는 현재 epoch로
    // 표식되어 이번 cycle의 white 후보가 아니다. 순서는 새 chunk부터, chunk 안은 낮은 번호부터다.
    while (!cutoffPublished && gcPhase_ == GcPhase::Sweeping && processed < budget.maxObjects &&
           (deadline == 0 || processed == 0 || platform::MonotonicNanoseconds() < deadline)) {
        platform::LockGuard lock(gate_);
        if (sweepChunk_ != 0) {
            for (Slot& slot : chunks_[--sweepChunk_]->slots) {
                if ((slot.state == SlotState::Alive || slot.state == SlotState::DestroyRequested) &&
                    slot.Mark() != markEpoch_) {
                    RetireLocked(slot);
                    ++cycleGc_.retired;
                }
            }

            ++processed;
        }

        if (sweepChunk_ == 0) {
            gcPhase_ = GcPhase::Reclaiming;
        }
    }

    const uint64 reclaimStarted = platform::MonotonicNanoseconds();
    cycleGc_.sweepNanoseconds += reclaimStarted - sweepStarted;
    if (gcPhase_ == GcPhase::Reclaiming && processed < budget.maxObjects &&
        (deadline == 0 || platform::MonotonicNanoseconds() < deadline)) {
        const uint32 reclaimed = ReclaimEligible(budget.maxObjects - processed, deadline);
        cycleGc_.reclaimed += reclaimed;
        processed += reclaimed;
    }

    const uint64 finished = platform::MonotonicNanoseconds();
    platform::LockGuard lock(gate_);
    cycleGc_.reclaimNanoseconds += finished - reclaimStarted;
    cycleGc_.durationNanoseconds += finished - started;
    if (gcPhase_ == GcPhase::Reclaiming && !HasReclaimableLocked()) {
        gcPhase_ = GcPhase::Idle;
        lastGc_ = cycleGc_;
        gcWork_.Clear();
        gcCursor_ = 0;
    }

    processing_ = false;
    return Result<GcProgress>::Success({gcPhase_, processed, cycleGc_});
}

Result<GcReport> ObjectRegistry::Collect(const ObjectAccessContext& access) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<GcReport>::Failure(valid.Error());
    }

    const uint64 started = platform::MonotonicNanoseconds();
    GcReport report;
    {
        platform::LockGuard lock(gate_);
        if (processing_ || constructions_ || totalBorrows_ || gcPhase_ != GcPhase::Idle) {
            return Result<GcReport>::Failure(ErrorCode::Busy);
        }

        processing_ = true;
        report.examined = tally_.liveObjects + tally_.destroyRequested;
    }

    // 모든 후보를 담을 용량을 먼저 확보한다. OOM이 발생해도 아직 어떤 객체도 retire하지 않았다.
    // 후보당 한 번만 큐에 넣으므로 이후 순회는 추가 할당 없이 깊은 그래프도 처리한다.
    Array<ObjectHandle> work(allocator_);
    auto reserved = work.TryReserve(report.examined);
    if (!reserved) {
        platform::LockGuard lock(gate_);
        processing_ = false;
        return Result<GcReport>::Failure(reserved.Error());
    }

    {
        platform::LockGuard lock(gate_);
        if (++markEpoch_ == 0) {
            markEpoch_ = 1;
            for (uint32 chunk = chunkCount_; chunk-- != 0;) {
                for (auto& slot : chunks_[chunk]->slots) {
                    slot.SetMark(0);
                }
            }
        }

        for (uint32 chunk = chunkCount_; chunk-- != 0;) {
            for (auto& slot : chunks_[chunk]->slots) {
                if ((slot.state == SlotState::Alive || slot.state == SlotState::DestroyRequested) &&
                    slot.roots != 0) {
                    auto pushed = work.TryPushBack({domain_, slot.index, slot.generation});
                    if (!pushed) {
                        platform::Fatal("Preallocated GC work queue exhausted");
                    }

                    slot.SetMark(markEpoch_);
                }
            }
        }
    }

    const uint64 markStarted = platform::MonotonicNanoseconds();
    report.beginNanoseconds = markStarted - started;
    MarkState state{this, &work};
    usize cursor = 0;
    (void)MarkSerial(work, cursor, state, NumericLimits<uint32>::Max(), 0);

    const uint64 sweepStarted = platform::MonotonicNanoseconds();
    report.markNanoseconds = sweepStarted - markStarted;
    if (state.error != ErrorCode::None) {
        platform::LockGuard lock(gate_);
        processing_ = false;
        return Result<GcReport>::Failure(state.error);
    }

    // 이 지점 이전에는 도달성 판단을 확정하지 않는다. Trace 오류 시 sweep 전체를 생략하고,
    // 다음 시도는 새 epoch를 사용해 부분 표식을 무시한다.
    report.marked = static_cast<uint32>(work.Size());
    {
        platform::LockGuard lock(gate_);
        for (uint32 chunk = chunkCount_; chunk-- != 0;) {
            for (auto& slot : chunks_[chunk]->slots) {
                if ((slot.state == SlotState::Alive || slot.state == SlotState::DestroyRequested) &&
                    slot.Mark() != markEpoch_) {
                    RetireLocked(slot);
                    ++report.retired;
                }
            }
        }
    }

    const uint64 reclaimStarted = platform::MonotonicNanoseconds();
    report.sweepNanoseconds = reclaimStarted - sweepStarted;
    report.reclaimed = ReclaimEligible();
    const uint64 finished = platform::MonotonicNanoseconds();
    report.reclaimNanoseconds = finished - reclaimStarted;
    report.durationNanoseconds = finished - started;
    {
        platform::LockGuard lock(gate_);
        lastGc_ = report;
        processing_ = false;
    }

    return Result<GcReport>::Success(report);
}

ObjectRegistry::ReclaimWork ObjectRegistry::TakeForReclaimLocked(Slot& slot) noexcept {
    if (slot.indexed || slot.linked) {
        platform::Fatal("Reclaiming an object still in the name index or child ring");
    }

    if (slot.queue != RetiredQueue::None) {
        platform::Fatal("Reclaiming an object still in a retired queue");
    }

    // 저장소를 목록에서 분리한 뒤 잠금을 푼다. 콜백 중 통계 조회는 가능하지만 재회수는 불가하다.
    // 지역 TypeLease가 소멸자 코드까지 살아 있게 하므로 해제 콜백이 끝난 뒤 반환한다. 이름 기록도
    // 잠금 밖에서 반환한다(조회는 잠금 안에서 복사해 두었다).
    slot.SetState(tally_, SlotState::Reclaiming);
    ReclaimWork work;
    work.slot = &slot;
    work.type = Move(slot.type);
    work.object = slot.object;
    work.storage = slot.storage;
    work.record = slot.name;
    slot.name = nullptr;
    return work;
}

void ObjectRegistry::FinishReclaim(ReclaimWork& work) noexcept {
    // User destructors and allocator callbacks never execute under the registry gate.
    work.type.Ops().destroy(work.object);
    auto released = allocator_.Deallocate(work.storage);
    if (!released) {
        platform::Fatal("Object allocation ownership mismatch");
    }

    ReleaseRegistryMemory(allocator_, work.record);
    {
        platform::LockGuard lock(gate_);
        Slot& slot = *work.slot;
        slot.object = nullptr;
        slot.storage = nullptr;
        slot.transaction = 0;
        slot.stableType = {};
        slot.SetMark(0);
        slot.outer = {};
        slot.outermost = nullptr;
        slot.parent = nullptr;
        slot.lastChild = nullptr;
        slot.pathBytes = 0;
        slot.discarded = false;
        // 세대를 0/과거 값으로 순환시키면 오래된 WeakRef가 새 객체를 가리킬 수 있다.
        // 한도에 도달한 슬롯은 재사용하지 않고 Exhausted로 남긴다.
        if (slot.generation >= options_.maxGeneration) {
            slot.SetState(tally_, SlotState::Exhausted);
        } else {
            ++slot.generation;
            slot.SetState(tally_, SlotState::Free);
            slot.nextInList = freeSlots_;
            freeSlots_ = &slot;
        }
    }

    work.type.Reset();
}

uint32 ObjectRegistry::ReclaimEligible(uint32 maximum, uint64 deadline) noexcept {
    // 회수 사이에 잠금을 풀고 사용자 소멸자를 부른다. 그 사이 소멸자(또는 다른 스레드)가 지연
    // 슬롯의 보유 계수를 반환하면 그 슬롯이 풀린 목록으로 옮겨져 다음 차례에 회수된다(목록을
    // 처음부터 다시 걷지 않는다).
    uint32 reclaimed = 0;
    while (reclaimed < maximum && (deadline == 0 || platform::MonotonicNanoseconds() < deadline)) {
        ReclaimWork work;
        {
            platform::LockGuard lock(gate_);
            Slot* selected = NextReclaimableLocked();
            if (selected == nullptr) {
                break;
            }

            UnlinkRetiredLocked(*selected);
            work = TakeForReclaimLocked(*selected);
        }

        FinishReclaim(work);
        ++reclaimed;
    }

    return reclaimed;
}

Result<uint32> ObjectRegistry::DrainRetired(const ObjectAccessContext& access) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<uint32>::Failure(valid.Error());
    }

    {
        platform::LockGuard lock(gate_);
        if (processing_) {
            return Result<uint32>::Failure(ErrorCode::Busy);
        }

        processing_ = true;
    }

    uint32 count = ReclaimEligible();
    {
        platform::LockGuard lock(gate_);
        processing_ = false;
    }

    return Result<uint32>::Success(count);
}

GcReport ObjectRegistry::LastGcReport() const noexcept {
    platform::LockGuard lock(gate_);
    return lastGc_;
}

} // namespace rocks
