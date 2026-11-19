#include "ObjectRegistryGcInternal.h"

#include <Base/Compiler.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/PlatformMath.h>
#include <Task/WorkerScheduler.h>

// ObjectRegistry GC의 cycle이다. 전체 수집(Collect)과 증분 수집(Begin·Step·Cancel)의 단계 진행,
// cutoff 뒤 white 판정(RetireWhiteLocked)과 GC 보고를 둔다. 표식은 ObjectRegistryGcMark.cpp·
// ObjectRegistryGcParallel.cpp, 회수는 ObjectRegistryGcReclaim.cpp, GC 밀집 자료는
// ObjectRegistryGcData.cpp다(Docs/Design/GarbageCollectionScale.ko.md 소스 파일 구성).
namespace rocks {

using namespace registry_detail;

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
        capacity = static_cast<usize>(GcQueueBound(uint64(chunkCount_) * SlotsPerChunk));
    }

    gcWork_.Clear();
    gcShade_.Clear();
    auto reserved = gcWork_.TryReserve(capacity);
    if (reserved) {
        reserved = gcShade_.TryReserve(capacity);
    }

    // Debug 장부 검증(RegistryOptions::verifyClusterLedger)은 처리 표시 아래 잠금 밖에서 한다.
    if (reserved) {
        reserved = VerifyClusterLedger();
    }

    platform::LockGuard lock(gate_);
    processing_ = false;
    if (!reserved) {
        return reserved;
    }

    // 표식 비트와 클러스터 표식을 본 소속 비트를 지운다(chunk마다 word 하나씩 — O(슬롯 수 / 64)).
    ClearMarksLocked();

    gcPhase_ = GcPhase::Marking;
    gcCursor_ = 0;
    AdvanceGcEpochLocked();
    cycleGc_ = {};
    cycleGc_.examined = tally_.liveObjects + tally_.destroyRequested;
    sweepChunk_ = 0;
    // 루트 목록만 걷는다(O(루트 수)). 루트가 있는 객체는 Retire되지 않으므로 살아 있다.
    for (uint32 position = 0; position < rootCount_; ++position) {
        if (MarkIndex(rootList_[position], gcWork_, cycleGc_.marked) != ErrorCode::None) {
            platform::Fatal("Root list holds a dead slot or the root queue is exhausted");
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
    gcShade_.Clear();
    ReleaseGrayBlocks();
    ReleaseDissolvedClustersLocked();
    gcCursor_ = 0;
    return Result<void>::Success();
}

uint32 ObjectRegistry::RetireWhiteLocked(uint32 chunk) noexcept {
    // 비트 묶음의 live & ~mark가 이 chunk의 white 후보다. 낮은 번호부터 Retire한다(슬롯 순서 유지).
    const GcBits& bits = BitsOf(chunk * SlotsPerChunk);
    uint64 white = bits.live & ~(LoadMark(bits.mark) | bits.clustered);
    uint32 retired = 0;
    while (white != 0) {
        const uint32 offset =
            static_cast<uint32>(platform::PlatformMath::CountTrailingZeros64(white));
        white &= white - 1;
        RetireLocked(chunks_[chunk]->slots[offset]);
        ++retired;
    }

    return retired;
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

    // 처리 표시 아래에서 gcPhase_·gcWork_·gcCursor_·sweepChunk_·GC 비트는 이 스레드만 바꾼다(장벽·
    // 생성·Discard는 Busy, 다른 스레드의 RootRef 복제도 Busy). 그래서 아래 루프는 잠금 없이 읽고,
    // 다른 스레드가 잠금 아래에서 읽는 전이(cutoff 게시·Retire·Reclaiming)만 잠가서 쓴다.
    uint32 processed = 0;
    const uint64 markStarted = platform::MonotonicNanoseconds();
    MarkState state{this, &gcWork_};
    GcSliceTimes slice;
    bool cutoffPublished = false;
    if (gcPhase_ == GcPhase::Marking) {
        // slice 사이에 장벽이 모은 회색 객체를 표식 큐로 옮긴다(slice 동안은 장벽이 없다 — 처리
        // 표시).
        DrainShadeQueue();
        slice.setup += platform::MonotonicNanoseconds() - markStarted;
        bool completed = false;
        if (scheduler == nullptr) {
            // 앞 병렬 slice가 블록으로 남긴 회색 객체를 직렬 큐로 편다.
            FlattenGrayBlocks();
            const uint64 serialStarted = platform::MonotonicNanoseconds();
            processed = MarkSerial(gcWork_, gcCursor_, state, budget.maxObjects, deadline);
            slice.marking += platform::MonotonicNanoseconds() - serialStarted;
            // 큐가 빈 것을 이 slice의 예산 안에서 보았을 때만 끝난 것으로 본다(예산을 다 쓴 slice는
            // Marking으로 끝나 다음 slice 전에 mutator가 장벽을 거칠 수 있다).
            completed = gcCursor_ == gcWork_.Size() && processed < budget.maxObjects &&
                        (deadline == 0 || platform::MonotonicNanoseconds() < deadline);
        } else {
            // 병렬 slice는 표식이 멈춘 뒤에도 worker 회수·회색 넘기기가 남는다. 앞 slice에서 잰 그
            // 꼬리 시간만큼(예산의 1/ParallelTailBudgetDivisor까지) 표식을 일찍 멈춘다.
            uint64 markDeadline = deadline;
            if (deadline != 0) {
                const uint64 limit = budget.maxNanoseconds / ParallelTailBudgetDivisor;
                const uint64 reserve = gcParallelTail_ < limit ? gcParallelTail_ : limit;
                markDeadline = deadline - reserve > started ? deadline - reserve : deadline;
            }

            processed = MarkParallel(gcWork_, gcCursor_, state, budget.maxObjects, markDeadline,
                                     *scheduler, completed, &gcGrayBlocks_, &slice);
            gcParallelTail_ = slice.drain + slice.requeue;
        }

        cycleGc_.marked += state.marked;
        if (state.error == ErrorCode::None && completed && gcShade_.IsEmpty()) {
            // 모든 장벽이 같은 gate에서 게시되고 slice 동안은 장벽이 없으므로(처리 표시) 빈 큐
            // 판정과 cutoff는 하나의 선형화다. 아직 slot을 Retired로 바꾸기 전에도 white 객체의
            // Resolve·Pin·연결을 차단한다. cutoff를 게시한 slice는 sweep 없이 끝낸다. sweep 단위가
            // chunk라 작은 그래프는 한 slice에 끝나므로, mutator가 cutoff 뒤 아직 분류하지 않은
            // white를 만나는 구간(Sweeping)을 언제나 한 번은 거친다
            // (Docs/Implementation/IncrementalGarbageCollection.ko.md의 cutoff 계약).
            platform::LockGuard lock(gate_);
            // 표식되지 않은 클러스터를 해체해 소속을 white 후보로 내보낸다(13.5절 — 패키지
            // RootRef를 Discard 없이 놓은 버그 경로). 큐가 비었으므로 해체한 레코드를 바로 재사용할
            // 수 있다.
            DissolveUnmarkedClustersLocked();
            // 경계 비율 안전장치: 남은 클러스터는 모두 표식됐으므로 해체가 소속에 mark를 켠다.
            cycleGc_.flattenedClusters += FlattenBoundaryClustersLocked();
            ReleaseDissolvedClustersLocked();
            gcPhase_ = GcPhase::Sweeping;
            sweepChunk_ = chunkCount_;
            cutoffPublished = true;
        }
    }

    if (state.error != ErrorCode::None) {
        platform::LockGuard lock(gate_);
        gcPhase_ = GcPhase::Idle;
        gcWork_.Clear();
        gcShade_.Clear();
        ReleaseGrayBlocks();
        ReleaseDissolvedClustersLocked();
        gcCursor_ = 0;
        processing_ = false;
        return Result<GcProgress>::Failure(state.error);
    }

    const uint64 sweepStarted = platform::MonotonicNanoseconds();
    cycleGc_.markNanoseconds += sweepStarted - markStarted;
    // sweep의 작업 단위는 chunk(64칸) 하나다(GC-S2). chunk의 white 후보는 비트 묶음의 live &
    // ~mark로 잠금 없이 판정하고(비트는 이 스레드만 바꾼다) white가 있는 chunk만 잠가 Retire한다.
    // cutoff 뒤 Sweeping 중 생성이 붙인 새 chunk(더 큰 번호)는 방문하지 않는다. 그 객체는 표식되어
    // 이번 cycle의 white 후보가 아니다. 순서는 새 chunk부터, chunk 안은 낮은 번호부터다.
    uint32 swept = 0;
    while (!cutoffPublished && gcPhase_ == GcPhase::Sweeping && processed < budget.maxObjects) {
        if (deadline != 0 && swept != 0 && swept % SweepTimeCheckInterval == 0 &&
            platform::MonotonicNanoseconds() >= deadline) {
            break;
        }

        if (sweepChunk_ == 0) {
            platform::LockGuard lock(gate_);
            gcPhase_ = GcPhase::Reclaiming;
            break;
        }

        const uint32 chunk = --sweepChunk_;
        const GcBits& bits = BitsOf(chunk * SlotsPerChunk);
        if ((bits.live & ~(LoadMark(bits.mark) | bits.clustered)) != 0) {
            platform::LockGuard lock(gate_);
            cycleGc_.retired += RetireWhiteLocked(chunk);
        }

        ++processed;
        ++swept;
    }

    const uint64 reclaimStarted = platform::MonotonicNanoseconds();
    cycleGc_.sweepNanoseconds += reclaimStarted - sweepStarted;
    slice.sweep = reclaimStarted - sweepStarted;
    if (gcPhase_ == GcPhase::Reclaiming && processed < budget.maxObjects &&
        (deadline == 0 || platform::MonotonicNanoseconds() < deadline)) {
        const uint32 reclaimed = ReclaimEligible(budget.maxObjects - processed, deadline);
        cycleGc_.reclaimed += reclaimed;
        processed += reclaimed;
    }

    const uint64 finished = platform::MonotonicNanoseconds();
    GcProgress progress;
    {
        platform::LockGuard lock(gate_);
        cycleGc_.reclaimNanoseconds += finished - reclaimStarted;
        slice.reclaim = finished - reclaimStarted;
        cycleGc_.durationNanoseconds += finished - started;
        if (gcPhase_ == GcPhase::Reclaiming && !HasReclaimableLocked()) {
            gcPhase_ = GcPhase::Idle;
            lastGc_ = cycleGc_;
            gcWork_.Clear();
            gcShade_.Clear();
            ReleaseGrayBlocks();
            gcCursor_ = 0;
        }

        processing_ = false;
        progress = {gcPhase_, processed, cycleGc_, slice};
    }

    if (progress.IsComplete()) {
        LogCycle("Incremental GC cycle", progress.report);
    }

    return Result<GcProgress>::Success(progress);
}

Result<GcReport> ObjectRegistry::Collect(const ObjectAccessContext& access,
                                         WorkerScheduler* scheduler, GcReclaim reclaim) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<GcReport>::Failure(valid.Error());
    }

    if (scheduler && scheduler->IsCurrentWorker()) {
        return Result<GcReport>::Failure(ErrorCode::WrongThread);
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
    Array<uint32> work(allocator_);
    auto reserved = work.TryReserve(static_cast<usize>(GcQueueBound(report.examined)));
    if (reserved) {
        reserved = VerifyClusterLedger();
    }

    if (!reserved) {
        platform::LockGuard lock(gate_);
        processing_ = false;
        return Result<GcReport>::Failure(reserved.Error());
    }

    MarkState state{this, &work};
    {
        platform::LockGuard lock(gate_);
        ClearMarksLocked();
        AdvanceGcEpochLocked();
        for (uint32 position = 0; position < rootCount_; ++position) {
            if (MarkIndex(rootList_[position], work, state.marked) != ErrorCode::None) {
                platform::Fatal("Root list holds a dead slot or the GC work queue is exhausted");
            }
        }
    }

    const uint64 markStarted = platform::MonotonicNanoseconds();
    report.beginNanoseconds = markStarted - started;
    usize cursor = 0;
    if (scheduler == nullptr) {
        state.counters = options_.countGcWork ? &report.work : nullptr;
        (void)MarkSerial(work, cursor, state, NumericLimits<uint32>::Max(), 0);
    } else {
        bool completed = false;
        (void)MarkParallel(work, cursor, state, NumericLimits<uint32>::Max(), 0, *scheduler,
                           completed);
        if (state.error == ErrorCode::None && !completed) {
            platform::Fatal("Unbounded parallel marking stopped before completion");
        }
    }

    const uint64 sweepStarted = platform::MonotonicNanoseconds();
    report.markNanoseconds = sweepStarted - markStarted;
    if (state.error != ErrorCode::None) {
        // 표식 비트는 다음 시도가 다시 지우므로 부분 표식이 남지 않는다.
        platform::LockGuard lock(gate_);
        processing_ = false;
        return Result<GcReport>::Failure(state.error);
    }

    // 이 지점 이전에는 도달성 판단을 확정하지 않는다. Trace 오류 시 sweep 전체를 생략한다.
    report.marked = state.marked;
    {
        platform::LockGuard lock(gate_);
        // 표식되지 않은 클러스터는 sweep 전에 해체한다(소속이 white 후보가 된다).
        DissolveUnmarkedClustersLocked();
        for (uint32 chunk = chunkCount_; chunk-- != 0;) {
            report.retired += RetireWhiteLocked(chunk);
        }

        // 경계 비율 안전장치: 분류가 끝난 뒤라 남은 소속은 모두 살아 있다(해체가 mark를 켜지 않아도
        // 된다).
        report.flattenedClusters = FlattenBoundaryClustersLocked();
    }

    const uint64 reclaimStarted = platform::MonotonicNanoseconds();
    report.sweepNanoseconds = reclaimStarted - sweepStarted;
    // Deferred는 분류한 객체를 회수 대상 목록에 남긴다(DrainRetired가 예산만큼 회수 — 17.2절).
    report.reclaimed = reclaim == GcReclaim::Immediately ? ReclaimEligible() : 0;
    const uint64 finished = platform::MonotonicNanoseconds();
    report.reclaimNanoseconds = finished - reclaimStarted;
    report.durationNanoseconds = finished - started;
    {
        platform::LockGuard lock(gate_);
        lastGc_ = report;
        processing_ = false;
    }

    LogCycle("Stop-the-world GC", report);
    return Result<GcReport>::Success(report);
}

GcReport ObjectRegistry::LastGcReport() const noexcept {
    platform::LockGuard lock(gate_);
    return lastGc_;
}

} // namespace rocks
