#include <Object/ConstructionContext.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>

namespace rocks {

// --- Spawn ---

Result<ObjectHandle> World::SpawnActorDeferred(TypeId type, const SpawnParams& params) noexcept {
    if (!CanMutate()) {
        return Result<ObjectHandle>::Failure(
            state_ == WorldState::Created ? ErrorCode::InvalidArgument : ErrorCode::ShuttingDown);
    }

    const ObjectHandle level = params.level.IsNull() ? persistentLevel_ : params.level;
    LevelRecord* levelRecord = FindLevelRecord(level);
    if (levelRecord == nullptr) {
        return Result<ObjectHandle>::Failure(ErrorCode::StaleHandle);
    }

    if (levelRecord->pendingDestroy) {
        return Result<ObjectHandle>::Failure(ErrorCode::Destroyed);
    }

    // 정리·시작 단계에서 할당 실패로 큐에 넣지 못하는 일이 없도록 여유를 먼저 확보한다. Actor
    // 하나는 각 큐에 많아야 한 번 들어간다.
    if (!pendingStart_.TryEnsureCapacity(actors_.Size() + 1) ||
        !destroyActors_.TryEnsureCapacity(actors_.Size() + 1)) {
        return Result<ObjectHandle>::Failure(ErrorCode::OutOfMemory);
    }

    ObjectAccessScope scope(Access());
    // Actor는 Level 클러스터에 가입한다(13.3절 — 완전한 필드 배치 타입만, native 파생은 밖에
    // 남는다).
    const bool levelClustered = levelRecord->clustered;
    bool clustered = false;
    Result<RootRef<Object>> created = Result<RootRef<Object>>::Failure(ErrorCode::InvalidArgument);
    {
        ConstructionContext transaction(objects_, Access());
        auto pending = transaction.Create<Actor>(type);
        if (!pending) {
            return Result<ObjectHandle>::Failure(pending.Error());
        }

        if (levelClustered) {
            auto joined = transaction.JoinCluster(pending.Value(), level);
            if (!joined) {
                return Result<ObjectHandle>::Failure(joined.Error());
            }

            clustered = JoinsCluster(transaction, pending.Value());
        }

        created = transaction.Commit();
        if (!created) {
            return Result<ObjectHandle>::Failure(created.Error());
        }
    }

    const ObjectHandle handle = created.Value().Handle();
    auto rollback = [&](ErrorCode error) noexcept {
        RequestDestroyOwned(handle);
        created.Value().Reset();
        return Result<ObjectHandle>::Failure(error);
    };

    auto attached = With<Actor>(handle, [this, level](Actor& actor) noexcept {
        actor.world_ = this;
        actor.level_ = level;
    });
    if (!attached) {
        return rollback(attached.Error());
    }

    auto slot = actors_.TryEmplace(allocator_);
    if (!slot) {
        return rollback(slot.Error());
    }

    auto indexed = actorIndex_.Insert(handle.slot, slot.Value());
    if (!indexed) {
        auto removed = actors_.TryRemove(slot.Value());
        (void)removed;
        return rollback(indexed.Error());
    }

    // Level의 강한 간선으로 소유를 넘긴다. 이후 임시 루트를 놓아도 Level이 살아 있는 동안 보존된다.
    uint32 levelPosition = 0;
    auto owned = With<Level>(level, [&](Level& owner) noexcept {
        levelPosition = static_cast<uint32>(owner.actors_.Size());
        return objects_.AddReference(Access(), level, owner.actors_, handle);
    });
    if (!owned) {
        actorIndex_.Erase(handle.slot);
        auto removed = actors_.TryRemove(slot.Value());
        (void)removed;
        return rollback(owned.Error());
    }

    ActorRecord& record = *actors_.Find(slot.Value());
    record.handle = handle;
    record.level = level;
    record.clustered = clustered;
    record.levelPosition = levelPosition;
    created.Value().Reset();
    FindLevelRecord(level)->actorCount += 1;
    ++spawnedTotal_;
    return Result<ObjectHandle>::Success(handle);
}

Result<ObjectHandle> World::SpawnActor(TypeId type, const SpawnParams& params) noexcept {
    auto spawned = SpawnActorDeferred(type, params);
    if (!spawned) {
        return spawned;
    }

    auto finished = FinishSpawning(spawned.Value());
    if (!finished) {
        return Result<ObjectHandle>::Failure(finished.Error());
    }

    return spawned;
}

Result<void> World::FinishSpawning(ObjectHandle actor) noexcept {
    if (!CanMutate()) {
        return Result<void>::Failure(ErrorCode::ShuttingDown);
    }

    ActorRecord* record = FindActorRecord(actor);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (record->state == ActorState::PendingDestroy) {
        return Result<void>::Failure(ErrorCode::Destroyed);
    }

    if (record->finishing) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (record->state != ActorState::Constructed) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ObjectAccessScope scope(Access());
    HookScope hooks(*this);
    record->finishing = true;
    auto finish = [this, actor](Result<void> outcome) noexcept {
        ActorRecord* current = FindActorRecord(actor);
        if (current != nullptr) {
            current->finishing = false;
        }

        return outcome;
    };

    // 1) 이미 추가된 Component의 엔진 등록(OnRegister). 실패한 Component만 정리한다. hook이 목록을
    //    바꿀 수 있으므로 원소마다 개수를 다시 읽고 null 원소는 건너뛴다.
    for (usize index = 0;; ++index) {
        const ComponentSlot slot = ComponentSlotAt(actor, index);
        if (index >= slot.count) {
            break;
        }

        const ObjectHandle component = slot.component;
        if (component.IsNull()) {
            continue;
        }

        auto pending = With<Component>(component, [](Component& value) noexcept {
            return !value.registered_ && value.state_ == ComponentState::Constructed;
        });
        if (!pending || !pending.Value()) {
            continue;
        }

        auto registered = With<Component>(
            component, [](Component& value) noexcept { return value.OnRegister(); });
        if (registered) {
            auto marked = With<Component>(
                component, [](Component& value) noexcept { value.registered_ = true; });
            (void)marked;
        } else {
            auto destroyed = DestroyComponent(component);
            (void)destroyed;
        }
    }

    record = FindActorRecord(actor);
    if (record == nullptr || record->state == ActorState::PendingDestroy) {
        // OnRegister 안에서 Actor 파괴가 요청됐다. OnAwake 전이므로 OnDestroy 없이 정리된다.
        return finish(Result<void>::Failure(ErrorCode::Destroyed));
    }

    // 2) 프레임워크 PreAwake(UE PreInitializeComponents) → Actor OnAwake. PreAwake 실패는 OnAwake
    //    실패와 같다.
    auto prepared =
        With<Actor>(actor, [](Actor& value) noexcept { return value.OnFrameworkPreAwake(); });
    record = FindActorRecord(actor);
    if (record == nullptr) {
        return finish(Result<void>::Failure(ErrorCode::Destroyed));
    }

    // PreAwake가 성공했으면 OnAwake가 실패해도 OnFrameworkEndPlay로 되돌릴 기회를 준다(PreAwake에서
    // 만든 것 — GameDirector의 GameState 등).
    record->frameworkAwake = prepared.IsOk();
    Result<void> awoke =
        prepared ? Result<void>::Success() : Result<void>::Failure(prepared.Error());
    if (prepared) {
        awoke = With<Actor>(actor, [](Actor& value) noexcept { return value.OnAwake(); });
        record = FindActorRecord(actor);
        if (record == nullptr) {
            return finish(Result<void>::Failure(ErrorCode::Destroyed));
        }
    }

    if (!awoke) {
        // OnDestroy 없이 정리한다(awake = false). 등록된 Component는 OnUnregister를 받는다.
        auto destroyed = DestroyActor(actor);
        (void)destroyed;
        return finish(Result<void>::Failure(awoke.Error()));
    }

    record->awake = true;
    if (record->state == ActorState::PendingDestroy) {
        // OnAwake 안에서 스스로 파괴를 요청했다. Spawn은 성공이며 정리 단계에서 OnDestroy를 받는다.
        return finish(Result<void>::Success());
    }

    auto settings = With<Actor>(actor, [](Actor& value) noexcept { return value.tickSettings_; });
    if (!settings || !IsValidTickSettings(settings.Value())) {
        auto destroyed = DestroyActor(actor);
        (void)destroyed;
        return finish(Result<void>::Failure(ErrorCode::InvalidArgument));
    }

    // 3) Component를 Actor 단계까지 진행한다(OnAwake 중 추가된 Component의 OnRegister 포함).
    AdvanceComponents(actor);

    record = FindActorRecord(actor);
    if (record == nullptr || record->state == ActorState::PendingDestroy) {
        return finish(Result<void>::Success());
    }

    // 4) 프레임워크 PostAwake(UE PostInitializeComponents — 모든 Component OnAwake 뒤). 실패하면
    //    Spawn이 실패하고, OnAwake가 성공했으므로 정리에서 OnDestroy를 받는다.
    auto posted =
        With<Actor>(actor, [](Actor& value) noexcept { return value.OnFrameworkPostAwake(); });
    record = FindActorRecord(actor);
    if (record == nullptr) {
        return finish(Result<void>::Failure(ErrorCode::Destroyed));
    }

    if (!posted) {
        auto destroyed = DestroyActor(actor);
        (void)destroyed;
        return finish(Result<void>::Failure(posted.Error()));
    }

    if (record->state == ActorState::PendingDestroy) {
        return finish(Result<void>::Success());
    }

    record->state = ActorState::Initialized;

    // 4) 활성이면 OnEnable(Actor -> Component).
    auto enabled = With<Actor>(actor, [](Actor& value) noexcept { return value.enabled_; });
    if (enabled && enabled.Value()) {
        EnableActor(actor, true);
    }

    // 시작은 World가 Playing일 때 다음 프레임 시작(또는 BeginPlay)에서 한다. 용량은 Spawn에서
    // 확보했다.
    auto queued = pendingStart_.TryPushBack(actor);
    (void)queued;
    return finish(Result<void>::Success());
}

Result<void> World::DestroyActor(ObjectHandle actor) noexcept {
    return DestroyActorFor(actor, EndPlayReason::Destroyed);
}

Result<void> World::DestroyActorFor(ObjectHandle actor, EndPlayReason reason) noexcept {
    ActorRecord* record = FindActorRecord(actor);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    // 이미 파괴 대기면 먼저 정한 이유를 유지한다(17.2절).
    if (record->state == ActorState::PendingDestroy) {
        return Result<void>::Success();
    }

    // 용량은 Spawn에서 확보했다.
    auto queued = destroyActors_.TryPushBack(actor);
    if (!queued) {
        return queued;
    }

    ObjectAccessScope scope(Access());
    record->state = ActorState::PendingDestroy;
    record->endReason = reason;
    // 실행 중 Tick에서도 즉시 빠진다(스케줄러가 저장소 정리를 단계 경계까지 미룬다).
    UnregisterTicks(record->ticks);
    for (usize index = 0;; ++index) {
        const ComponentSlot slot = ComponentSlotAt(actor, index);
        if (index >= slot.count) {
            break;
        }

        const ObjectHandle component = slot.component;
        if (component.IsNull()) {
            continue;
        }

        auto marked = With<Component>(component, [this, reason](Component& value) noexcept {
            // 먼저 단독 파괴가 요청된 Component는 그 이유를 유지한다(17.2절).
            if (value.state_ != ComponentState::PendingDestroy) {
                value.endReason_ = reason;
            }
            value.state_ = ComponentState::PendingDestroy;
            UnregisterTicks(value.ticks_);
        });
        (void)marked;
    }

    return Result<void>::Success();
}

void World::DestroyAllActors(EndPlayReason reason) noexcept {
    // 파괴 대기 표시는 hook을 부르지 않으므로 한 번의 순회로 충분하다(O(n)).
    for (const ActorRecord& record : actors_) {
        if (record.state != ActorState::PendingDestroy) {
            auto destroyed = DestroyActorFor(record.handle, reason);
            (void)destroyed;
        }
    }

    FlushDestroys();
}

bool World::JoinsCluster(ConstructionContext& transaction, ObjectHandle object) noexcept {
    auto type = transaction.AcquireType(object);
    return type && type.Value().Ops().completeFieldLayout;
}

} // namespace rocks
