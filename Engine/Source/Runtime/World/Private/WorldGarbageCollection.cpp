#include <Core/Diagnostics/Log.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <World/World.h>
#include <World/WorldLog.h>

namespace rocks {

void World::RequestFullCollection() noexcept {
    platform::AtomicStore<platform::MemoryOrder::Release>(&fullCollectionRequested_, uint32{1});
}

void World::StepGarbageCollection() noexcept {
    // World API 밖에서 파괴된 Actor·Component·Level을 매 프레임 GC step 전에 정리한다. World 객체의
    // 회수는 World 루트·Level 배열·Actor의 components 간선에서 끊긴 뒤에만 일어나고 그 간선을 끊는
    // 것은 World 정리뿐이라, 우회 파괴가 언제 일어나든(Tick·OnDestroy hook 안 포함) 여기 전에
    // 회수되지 않는다. 매 프레임인 이유는 cycle 시작 전에만 보면 다른 World 파괴가 없을 때 GC가
    // 돌지 않아 영영 정리하지 않기 때문이다. 그 Component가 Level 클러스터의 고아 소속으로 남지
    // 않게도 한다(13.3·13.8절). 목록이 비면 잠금 한 번이다(엔진 런타임 7.4절).
    if (RepairBypassedObjects()) {
        FlushDestroys();
    }

    // 회수 대기를 예산만큼 먼저 회수한다(GC 설계 17.3절).
    DrainDeferredReclaim();
    secondsSinceFullCollection_ += lastRealDeltaSeconds_;
    // 주기 수집은 간격만 본다(World 파괴가 없어도 — GC 설계 17.3절, UE와 같다). 0 이하는 주기
    // 수집을 하지 않는다.
    const bool periodic = gcSchedule_.mode == WorldGcMode::FullCollection &&
                          gcSchedule_.fullCollectionIntervalSeconds > 0.0 &&
                          secondsSinceFullCollection_ >= gcSchedule_.fullCollectionIntervalSeconds;
    const bool requested = platform::AtomicExchange<platform::MemoryOrder::AcquireRelease>(
                               &fullCollectionRequested_, uint32{0}) != 0;
    if (requested || periodic) {
        // Busy(외부 Borrow)면 요청을 남겨 다음 GC 단계에서 다시 한다.
        if (!RunFullCollection() && requested) {
            RequestFullCollection();
        }

        return;
    }

    if (!gcPending_ || gcSchedule_.mode != WorldGcMode::Incremental) {
        return;
    }

    if (objects_.CollectionPhase() == GcPhase::Idle) {
        auto begun = objects_.BeginCollection(Access());
        // 외부가 Borrow를 보유하면 Busy다. 다음 프레임에 다시 시도한다.
        if (!begun) {
            return;
        }

        destroyedDuringCycle_ = false;
    }

    auto progress = objects_.StepCollection(Access(), gcBudget_, gcScheduler_);
    if (progress && progress.Value().IsComplete()) {
        // cycle 하나에 한 번이다(매 프레임이 아니다).
        const GcReport& report = progress.Value().report;
        Log::Debug(LogWorld,
                   "World {World} GC cycle reclaimed {Reclaimed} of {Examined} examined objects "
                   "({Retired} retired) in {Microseconds} us",
                   Name(), report.reclaimed, report.examined, report.retired,
                   report.durationNanoseconds / 1000);
        // 이번 cycle 시작 뒤에 파괴된 객체가 있으면 다음 cycle이 필요하다.
        gcPending_ = destroyedDuringCycle_;
    }
}

void World::DrainDeferredReclaim() noexcept {
    // 반환값이 아니라 질의로 판정한다: 선점으로 짧게 끝난 프레임도, 보유(BackendLease 등)가 풀린
    // 뒤의 객체도, 회수가 남은 채 다시 한 전체 수집 뒤에도 다음 프레임에 이어서 회수한다(GC 설계
    // 17.3절). 증분 cycle이 진행 중이면 그 Reclaiming 단계가 회수하므로 부르지 않는다.
    if (objects_.CollectionPhase() == GcPhase::Idle && objects_.HasPendingReclaim()) {
        drainingDeferred_ = true;
        auto drained = objects_.DrainRetired(Access(), gcSchedule_.reclaimBudget);
        (void)drained;
        if (objects_.HasPendingReclaim()) {
            return;
        }
    }

    if (drainingDeferred_ && !objects_.HasPendingReclaim()) {
        drainingDeferred_ = false;
        const GcReport report = objects_.LastGcReport();
        Log::Debug(LogWorld,
                   "World {World} finished deferred reclaim: {Reclaimed} objects in {Microseconds} "
                   "us of reclaim time",
                   Name(), report.reclaimed, report.reclaimNanoseconds / 1000);
    }
}

bool World::RunFullCollection() noexcept {
    // 진행 중인 증분 cycle: cutoff 전(Marking)은 취소하고, 그 뒤(sweep·회수)는 회수 없이
    // 끝낸다(남은 분류만 하고 회수는 예산으로 나눈다 — GC 설계 17.2절).
    if (objects_.CollectionPhase() == GcPhase::Marking) {
        if (!objects_.CancelCollection(Access())) {
            return false;
        }
    } else if (objects_.CollectionPhase() != GcPhase::Idle) {
        if (!objects_.FinishCollectionDeferringReclaim(Access())) {
            return false;
        }
    }

    auto collected = objects_.Collect(Access(), gcScheduler_, GcReclaim::Deferred);
    if (!collected) {
        // 외부 Borrow 등으로 Busy다.
        return false;
    }

    const GcReport& report = collected.Value();
    Log::Debug(LogWorld,
               "World {World} full GC marked {Marked} of {Examined} objects and retired {Retired} "
               "in {Microseconds} us (reclaim deferred)",
               Name(), report.marked, report.examined, report.retired,
               report.durationNanoseconds / 1000);
    gcPending_ = false;
    destroyedDuringCycle_ = false;
    secondsSinceFullCollection_ = 0.0;
    return true;
}

void World::RequestDestroyOwned(ObjectHandle object) noexcept {
    auto requested = objects_.RequestDestroy(Access(), object);
    if (!requested && requested.Error() != ErrorCode::StaleHandle &&
        requested.Error() != ErrorCode::Destroyed && requested.Error() != ErrorCode::ShuttingDown) {
        platform::Fatal("World could not request destruction of an object it owns");
    }
}

bool World::RepairBypassedObjects() noexcept {
    // Registry의 파괴 요청 목록(지난 호출 뒤 DestroyRequested가 된 객체)만 본다 — Actor 수에
    // 비례하는 훑기가 아니다. World가 스스로 요청한 파괴는 건너뛴다(Actor는 PendingDestroy 레코드,
    // Component는 회수 요청 전에 색인에서 빠진다). DestroyActor는 레코드 상태를 바꾸고 파괴 큐에
    // 넣을 뿐 목록을 바꾸지 않는다.
    bool repaired = false;
    ObjectHandle batch[RepairBatch];
    for (;;) {
        auto taken = objects_.TakeDestroyRequests(Access(), Span<ObjectHandle>(batch, RepairBatch));
        if (!taken || taken.Value() == 0) {
            break;
        }

        for (usize at = 0; at < taken.Value(); ++at) {
            if (levelSlots_.Find(batch[at].slot) != nullptr) {
                const LevelRecord* level = FindLevelRecord(batch[at]);
                if (level == nullptr || level->pendingDestroy) {
                    continue;
                }

                // 영구 Level은 World API로도 파괴할 수 없다(Spawn 기본 대상) — 복구할 수 없는 계약
                // 위반이다.
                if (batch[at] == persistentLevel_) {
                    platform::Fatal("Persistent Level was destroyed outside the World API");
                }

                ++repairedBypasses_;
                repaired = true;
                if (!DestroyLevel(batch[at])) {
                    platform::Fatal("World could not repair a Level destroyed outside its API");
                }

                continue;
            }

            if (QueueBypassedComponent(batch[at])) {
                ++repairedBypasses_;
                repaired = true;
                continue;
            }

            const ActorRecord* record = FindActorRecord(batch[at]);
            if (record == nullptr || record->state == ActorState::PendingDestroy) {
                continue;
            }

            ++repairedBypasses_;
            repaired = true;
            auto destroyed = DestroyActor(batch[at]);
            (void)destroyed;
        }
    }

    return repaired;
}

bool World::QueueBypassedComponent(ObjectHandle component) noexcept {
    ComponentRecord* entry = componentRecords_.Find(component.slot);
    if (entry == nullptr || entry->component != component || entry->bypassQueued) {
        return false;
    }

    // 용량은 AddComponent가 Component마다 확보했다(한 번만 넣는다).
    entry->bypassQueued = true;
    auto queued = destroyComponents_.TryPushBack(component);
    (void)queued;
    return true;
}

void World::ForgetComponent(ObjectHandle component) noexcept {
    ComponentRecord* entry = componentRecords_.Find(component.slot);
    if (entry == nullptr || entry->component != component) {
        return;
    }

    UnregisterTicks(entry->ticks);
    componentRecords_.Erase(component.slot);
}

} // namespace rocks
