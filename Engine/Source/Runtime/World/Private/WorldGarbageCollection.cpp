#include <Platform/Fatal.h>
#include <World/World.h>

namespace rocks {

void World::StepGarbageCollection() noexcept {
    // World API 밖에서 파괴된 Actor·Level을 매 프레임 GC step 전에 정리한다. 우회 파괴는 Tick 밖
    // (step 사이)에서만 일어나므로 sweep이 그 객체를 회수하기 전에 늘 여기를 지난다 — cycle 시작
    // 전에만 보면 cycle 도중의 우회 파괴가 정리 없이 회수되거나(Level이 사라지면 소속 Actor도
    // 도달하지 못한다), 다른 World 파괴가 없으면 GC가 돌지 않아 영영 남는다. 그 Component가 Level
    // 클러스터의 고아 소속으로 남지 않게도 한다(13.3·13.8절). 목록이 비면 잠금 한 번이다.
    if (RepairBypassedActors()) {
        FlushDestroys();
    }

    if (!gcPending_) {
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
        // 이번 cycle 시작 뒤에 파괴된 객체가 있으면 다음 cycle이 필요하다.
        gcPending_ = destroyedDuringCycle_;
    }
}

void World::RequestDestroyOwned(ObjectHandle object) noexcept {
    auto requested = objects_.RequestDestroy(Access(), object);
    if (!requested && requested.Error() != ErrorCode::StaleHandle &&
        requested.Error() != ErrorCode::Destroyed && requested.Error() != ErrorCode::ShuttingDown) {
        platform::Fatal("World could not request destruction of an object it owns");
    }
}

bool World::RepairBypassedActors() noexcept {
    // Registry의 파괴 요청 목록(지난 호출 뒤 DestroyRequested가 된 객체)만 본다 — Actor 수에
    // 비례하는 훑기가 아니다. World가 스스로 요청한 파괴(PendingDestroy 레코드, Component)는
    // 건너뛴다. DestroyActor는 레코드 상태를 바꾸고 파괴 큐에 넣을 뿐 목록을 바꾸지 않는다.
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

                ++repairedTicks_;
                repaired = true;
                if (!DestroyLevel(batch[at])) {
                    platform::Fatal("World could not repair a Level destroyed outside its API");
                }

                continue;
            }

            const ActorRecord* record = FindActorRecord(batch[at]);
            if (record == nullptr || record->state == ActorState::PendingDestroy) {
                continue;
            }

            ++repairedTicks_;
            repaired = true;
            auto destroyed = DestroyActor(batch[at]);
            (void)destroyed;
        }
    }

    return repaired;
}

} // namespace rocks
