#include "WorldInternal.h"

#include <Object/ConstructionContext.h>
#include <Platform/Fatal.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>

namespace rocks {

// --- Level ---

World::LevelRecord* World::FindLevelRecord(ObjectHandle level) noexcept {
    for (LevelRecord& record : levels_) {
        if (record.handle == level) {
            return &record;
        }
    }

    return nullptr;
}

const World::LevelRecord* World::FindLevelRecord(ObjectHandle level) const noexcept {
    for (const LevelRecord& record : levels_) {
        if (record.handle == level) {
            return &record;
        }
    }

    return nullptr;
}

bool World::HasLevel(ObjectHandle level) const noexcept {
    return FindLevelRecord(level) != nullptr;
}

Result<ObjectHandle> World::CreateLevelInternal(StringView name, bool persistent) noexcept {
    auto reserved = levels_.TryEnsureCapacity(levels_.Size() + 1);
    if (!reserved) {
        return Result<ObjectHandle>::Failure(reserved.Error());
    }

    // Level은 클러스터 루트다(13.3절). Spawn·AddComponent가 소속 Actor·Component를 가입시킨다.
    // Level 타입이 완전한 필드 배치가 아니거나 클러스터를 끈 World(WorldDesc::levelClusters)면
    // 클러스터 없이 둔다(지정이 Unsupported다).
    ConstructionContext transaction(objects_, Access());
    auto pending = transaction.Create<Level>(ObjectTypeIdOf<Level>());
    if (!pending) {
        return Result<ObjectHandle>::Failure(pending.Error());
    }

    auto rooted = transaction.SetClusterRoot(pending.Value());
    if (!rooted && rooted.Error() != ErrorCode::Unsupported) {
        return Result<ObjectHandle>::Failure(rooted.Error());
    }

    auto committed = transaction.Commit();
    if (!committed) {
        return Result<ObjectHandle>::Failure(committed.Error());
    }

    const ObjectHandle handle = committed.Value().Handle();
    auto created = objects_.TryPin<Level>(Access(), handle);
    committed.Value().Reset();
    if (!created) {
        return Result<ObjectHandle>::Failure(created.Error());
    }

    auto configured = With<Level>(handle, [&](Level& level) noexcept {
        level.world_ = this;
        level.persistent_ = persistent;
        return level.name_.TryAssign(name);
    });
    if (!configured) {
        RequestDestroyOwned(handle);
        return Result<ObjectHandle>::Failure(configured.Error());
    }

    auto indexed = levelSlots_.Insert(handle.slot, 0);
    if (!indexed) {
        RequestDestroyOwned(handle);
        return Result<ObjectHandle>::Failure(indexed.Error());
    }

    // 용량을 위에서 확보했으므로 실패하지 않는다.
    auto added = levels_.TryEmplaceBack();
    added.Value()->root = Move(created.Value());
    added.Value()->handle = handle;
    added.Value()->clustered = static_cast<bool>(rooted);
    return Result<ObjectHandle>::Success(handle);
}

Result<ObjectHandle> World::CreateLevel(StringView name) noexcept {
    if (!CanMutate()) {
        return Result<ObjectHandle>::Failure(
            state_ == WorldState::Created ? ErrorCode::InvalidArgument : ErrorCode::ShuttingDown);
    }

    ObjectAccessScope scope(Access());
    return CreateLevelInternal(name, false);
}

Result<void> World::DestroyLevel(ObjectHandle level) noexcept {
    LevelRecord* record = FindLevelRecord(level);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (level == persistentLevel_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (record->pendingDestroy) {
        return Result<void>::Success();
    }

    ObjectAccessScope scope(Access());
    record->pendingDestroy = true;
    // Level 객체가 World API 밖에서 파괴됐으면(우회 파괴 복구) 빌릴 수 없어 pendingDestroy_를 켜지
    // 않는다 — 그 Level은 곧 회수되고 World 쪽 판단은 레코드의 pendingDestroy로 한다.
    auto marked = With<Level>(level, [](Level& value) noexcept { value.pendingDestroy_ = true; });
    (void)marked;
    // 소속 Actor를 모두 파괴 대기로 만든다(hook 없음). 정리 단계가 Actor를 정리한 뒤 Level을
    // 회수한다. Level 배열만 훑는다(World 전체 Actor가 아니라 — Level이 정리 중이면 Actor 파괴는
    // 배열을 고치지 않는다).
    auto listed = With<Level>(level, [&](Level& owner) noexcept {
        for (usize at = 0; at < owner.actors_.Size(); ++at) {
            const ActorRecord* actor = FindActorRecord(owner.actors_[at]);
            if (actor != nullptr && actor->state != ActorState::PendingDestroy) {
                auto destroyed = DestroyActor(actor->handle);
                (void)destroyed;
            }
        }
    });
    if (!listed) {
        // Level 객체가 World API 밖에서 파괴돼(DestroyRequested) 빌릴 수 없다(우회 파괴 복구). 소속
        // Actor를 World 레코드에서 찾는다 — 이 계약 위반의 복구에서만 Actor 수에 비례한다.
        // DestroyActor는 큐에만 넣으므로 레코드를 훑는 동안 저장소가 바뀌지 않는다.
        for (const ActorRecord& actor : actors_) {
            if (actor.level == level && actor.state != ActorState::PendingDestroy) {
                auto destroyed = DestroyActor(actor.handle);
                (void)destroyed;
            }
        }
    }

    // Tick·hook 밖에서 요청했고 소속 Actor가 이미 없으면 바로 회수한다.
    if (!inTick_ && hookDepth_ == 0) {
        FinishLevelDestroys();
    }

    return Result<void>::Success();
}

void World::ReleaseLevel(usize index) noexcept {
    const ObjectHandle handle = levels_[index].handle;
    // 소속 Actor는 이미 모두 정리됐다. 남은 간선(정리 중 개별 제거를 생략한 경우)을 한 번에 비운다.
    auto cleared = With<Level>(handle, [&](Level& level) noexcept {
        return objects_.ClearReferences(Access(), handle, level.actors_);
    });
    (void)cleared;
    RootRef<Level> root = Move(levels_[index].root);
    auto removed = levels_.TryRemoveAt(index);
    (void)removed;
    levelSlots_.Erase(handle.slot);
    RequestDestroyOwned(handle);
    root.Reset();
    gcPending_ = true;
    destroyedDuringCycle_ = true;
}

void World::FinishLevelDestroys() noexcept {
    for (usize index = levels_.Size(); index > 0; --index) {
        if (levels_[index - 1].pendingDestroy && levels_[index - 1].actorCount == 0) {
            ReleaseLevel(index - 1);
        }
    }
}

void World::DetachFromLevel(ObjectHandle level, const ActorRecord& record) noexcept {
    // 끝 원소를 이 Actor 자리로 옮기고 끝을 지운다(O(1) — RemoveReferenceAtSwap은 옮기는 원소를
    // 다시 검증하지 않으므로 끝 Actor가 World API 밖에서 파괴돼 DestroyRequested여도 성공한다).
    // 옮긴 Actor의 위치를 고친다. 이 경로의 실패(파괴 정리는 안전 지점에서만 하고 World
    // Registry에는 관찰자가 없다)는 일어날 수 없으므로 Fatal로 막는다 — 버리면 지울 Actor가 배열에
    // 남는다.
    auto detached = With<Level>(level, [&](Level& owner) noexcept {
        const uint32 at = record.levelPosition;
        if (owner.actors_.IsEmpty() || at >= owner.actors_.Size() ||
            owner.actors_[at] != record.handle) {
            platform::Fatal("Actor record position diverged from its Level array");
        }

        const usize last = owner.actors_.Size() - 1;
        const ObjectHandle moved = owner.actors_[last];
        if (auto removed = objects_.RemoveReferenceAtSwap(Access(), level, owner.actors_, at);
            !removed) {
            platform::Fatal(detail::LevelDetachFailureMessage(removed.Error()));
        }

        if (at != last) {
            ActorRecord* movedRecord = FindActorRecord(moved);
            if (movedRecord == nullptr) {
                platform::Fatal("Level array holds an actor without a record");
            }

            movedRecord->levelPosition = at;
        }
    });
    // Level 객체 자체가 World API 밖에서 파괴됐으면(DestroyRequested — 다음 복구가 Level을
    // 정리한다) 배열은 Level과 함께 사라지므로 뗄 것이 없다. 그 밖의 실패(회수된 Level의 레코드
    // 등)는 복구가 매 GC step 전에 돌아 일어날 수 없다.
    if (!detached && detached.Error() != ErrorCode::Destroyed) {
        platform::Fatal("World could not borrow the Level of an actor it destroys");
    }
}

} // namespace rocks
