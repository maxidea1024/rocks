#include <Object/ConstructionContext.h>
#include <Base/InitializerList.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogContext.h>
#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <World/WorldLog.h>

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

    // Spawn 인자 검사(Actor를 만들기 전 — 실패해도 되돌릴 것이 없다). 소유자·원인은 이 World의 살아
    // 있는 Actor여야 한다(파괴 대기면 Destroyed — Level과 같다).
    if (params.hasTransform && !params.transform.IsValid()) {
        return Result<ObjectHandle>::Failure(ErrorCode::InvalidArgument);
    }

    for (const ObjectHandle target : {params.owner, params.instigator}) {
        if (target.IsNull()) {
            continue;
        }

        const ActorRecord* linked = FindActorRecord(target);
        if (linked == nullptr) {
            return Result<ObjectHandle>::Failure(ErrorCode::StaleHandle);
        }

        if (linked->state == ActorState::PendingDestroy) {
            return Result<ObjectHandle>::Failure(ErrorCode::Destroyed);
        }
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
    record.hasSpawnTransform = params.hasTransform;
    record.spawnTransform = params.transform;
    created.Value().Reset();
    FindLevelRecord(level)->actorCount += 1;
    record.spawnOrder = ++spawnedTotal_;

    // 소유자·원인은 위에서 검사했다(새 Actor는 아무도 소유하지 않아 고리가 생길 수 없다).
    if (!params.owner.IsNull() || !params.instigator.IsNull()) {
        auto linked = With<Actor>(handle, [&params](Actor& value) noexcept {
            value.owner_ = params.owner;
            value.instigator_ = params.instigator;
        });
        (void)linked;
    }

    return Result<ObjectHandle>::Success(handle);
}

Result<void> World::ApplySpawnTransform(ObjectHandle actor) noexcept {
    ActorRecord* record = FindActorRecord(actor);
    if (record == nullptr || !record->hasSpawnTransform) {
        return Result<void>::Success();
    }

    const Transform spawn = record->spawnTransform;
    record->hasSpawnTransform = false;
    auto root = With<Actor>(actor, [](Actor& value) noexcept { return value.RootComponent(); });
    if (!root || root.Value().IsNull()) {
        return Result<void>::Success();
    }

    // world = root 상대 × spawn(UE RootRelative * UserSpawnTransform). root에 부모가 있으면 상대 =
    // world × 부모 월드의 역(SetWorldTransform과 같다 — 부모·자식은 TryMultiply(local,
    // parentToWorld)).
    auto parts = With<SceneComponent>(root.Value(), [](SceneComponent& scene) noexcept {
        struct Parts {
            Transform relative;
            ObjectHandle parent;
        };
        return Parts{scene.RelativeTransform(), scene.Parent()};
    });
    if (!parts) {
        return Result<void>::Failure(parts.Error());
    }

    auto world = Transform::TryMultiply(parts.Value().relative, spawn);
    if (!world) {
        return Result<void>::Failure(world.Error());
    }

    Transform relative = world.Value();
    if (!parts.Value().parent.IsNull()) {
        auto parentWorld =
            With<SceneComponent>(parts.Value().parent, [](SceneComponent& scene) noexcept {
                return scene.TryWorldTransform();
            });
        if (!parentWorld) {
            return Result<void>::Failure(parentWorld.Error());
        }

        auto inverse = parentWorld.Value().TryInverse();
        if (!inverse) {
            return Result<void>::Failure(inverse.Error());
        }

        auto local = Transform::TryMultiply(world.Value(), inverse.Value());
        if (!local) {
            return Result<void>::Failure(local.Error());
        }

        relative = local.Value();
    }

    return With<SceneComponent>(root.Value(), [&relative](SceneComponent& scene) noexcept {
        return scene.SetRelativeTransform(relative);
    });
}

// --- Owner·Instigator·LifeSpan(17.3·17.5절) ---

namespace {

// LifeSpan 만료 판정의 상대 허용오차다(TickScheduler interval·GameClock과 같은 값).
constexpr double LifeSpanTolerance = 1.0e-9;

} // namespace

Result<void> World::SetActorOwner(ObjectHandle actor, ObjectHandle owner) noexcept {
    if (FindActorRecord(actor) == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (!owner.IsNull()) {
        const ActorRecord* target = FindActorRecord(owner);
        if (target == nullptr) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        if (target->state == ActorState::PendingDestroy) {
            return Result<void>::Failure(ErrorCode::Destroyed);
        }

        // 새 소유자의 소유 사슬에 자신이 있으면 고리다(UE SetOwner — Error 로그 후 거부). 사슬은
        // 고리가 없도록 유지되므로 Actor 수 안에 끝난다.
        ObjectHandle current = owner;
        for (usize depth = 0; !current.IsNull() && depth <= actors_.Size(); ++depth) {
            if (current == actor) {
                LogContextScope context("World", Name());
                Log::Error(LogWorld,
                           "Rejected actor owner: ownership would form a cycle (slot {Owner} -> "
                           "slot {Actor})",
                           owner.slot, actor.slot);
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            auto next = With<Actor>(current, [](Actor& value) noexcept { return value.owner_; });
            current = next ? next.Value() : ObjectHandle{};
        }
    }

    return With<Actor>(actor, [owner](Actor& value) noexcept { value.owner_ = owner; });
}

Result<void> World::SetActorInstigator(ObjectHandle actor, ObjectHandle instigator) noexcept {
    const ActorRecord* target = instigator.IsNull() ? nullptr : FindActorRecord(instigator);
    if (FindActorRecord(actor) == nullptr || (!instigator.IsNull() && target == nullptr)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (target != nullptr && target->state == ActorState::PendingDestroy) {
        return Result<void>::Failure(ErrorCode::Destroyed);
    }

    return With<Actor>(actor,
                       [instigator](Actor& value) noexcept { value.instigator_ = instigator; });
}

Result<void> World::SetActorLifeSpan(ObjectHandle actor, double seconds) noexcept {
    ActorRecord* record = FindActorRecord(actor);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (!math::IsFinite(seconds)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 0 이하는 해제다(힙의 옛 항목은 번호가 달라 무시된다).
    if (seconds <= 0.0) {
        record->lifeSpanSequence = 0;
        record->pendingLifeSpan = 0.0;
        return Result<void>::Success();
    }

    // 시작 전이면 시작할 때부터 센다(UE는 InitialLifeSpan을 BeginPlay에서 건다).
    if (record->state != ActorState::Playing) {
        record->lifeSpanSequence = 0;
        record->pendingLifeSpan = seconds;
        return Result<void>::Success();
    }

    LifeSpanEntry entry{clock_.GameTimeSeconds() + seconds, ++lifeSpanCounter_, actor};
    auto pushed = lifeSpans_.TryPushBack(entry);
    if (!pushed) {
        return pushed;
    }

    record = FindActorRecord(actor);
    record->lifeSpanSequence = entry.sequence;
    // 최소 힙 위로 올리기((만료, 번호) 순).
    auto before = [](const LifeSpanEntry& left, const LifeSpanEntry& right) noexcept {
        return left.expiry != right.expiry ? left.expiry < right.expiry
                                           : left.sequence < right.sequence;
    };
    usize child = lifeSpans_.Size() - 1;
    while (child > 0) {
        const usize parent = (child - 1) / 2;
        if (!before(lifeSpans_[child], lifeSpans_[parent])) {
            break;
        }
        const LifeSpanEntry swap = lifeSpans_[child];
        lifeSpans_[child] = lifeSpans_[parent];
        lifeSpans_[parent] = swap;
        child = parent;
    }

    return Result<void>::Success();
}

void World::ExpireLifeSpans() noexcept {
    auto before = [](const LifeSpanEntry& left, const LifeSpanEntry& right) noexcept {
        return left.expiry != right.expiry ? left.expiry < right.expiry
                                           : left.sequence < right.sequence;
    };
    // 게임 시간은 프레임 delta를 더해 가므로 같은 길이를 다른 경로로 더한 값이 아주 조금 다를 수
    // 있다(GameClock 고정 스텝 판정과 같은 상대 허용오차로 흡수한다).
    const double now = clock_.GameTimeSeconds();
    const double limit = now + LifeSpanTolerance * (now > 1.0 ? now : 1.0);
    while (!lifeSpans_.IsEmpty() && lifeSpans_[0].expiry <= limit) {
        const LifeSpanEntry top = lifeSpans_[0];
        const LifeSpanEntry last = lifeSpans_.TryPopBack().Value();
        if (!lifeSpans_.IsEmpty()) {
            lifeSpans_[0] = last;
            usize parent = 0;
            for (;;) {
                const usize left = parent * 2 + 1;
                const usize right = left + 1;
                usize smallest = parent;
                if (left < lifeSpans_.Size() && before(lifeSpans_[left], lifeSpans_[smallest])) {
                    smallest = left;
                }
                if (right < lifeSpans_.Size() && before(lifeSpans_[right], lifeSpans_[smallest])) {
                    smallest = right;
                }
                if (smallest == parent) {
                    break;
                }
                const LifeSpanEntry swap = lifeSpans_[parent];
                lifeSpans_[parent] = lifeSpans_[smallest];
                lifeSpans_[smallest] = swap;
                parent = smallest;
            }
        }

        // 다시 설정·해제된 Actor의 옛 항목, 이미 파괴된 Actor는 무시한다.
        ActorRecord* record = FindActorRecord(top.actor);
        if (record != nullptr && record->lifeSpanSequence == top.sequence &&
            record->state != ActorState::PendingDestroy) {
            record->lifeSpanSequence = 0;
            auto destroyed = DestroyActor(top.actor);
            (void)destroyed;
        }
    }
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

    // 5) Spawn 인자의 Transform을 root에 쓴다. OnAwake에서 만든 root도 받도록 Component 초기화 뒤에
    //    한다. UE 기본(MultiplyWithRoot)처럼 root 상대 Transform × Spawn Transform을 root의 월드
    //    Transform으로 둔다(PostSpawnInitialize — Actor.cpp:4312–4324). 표현할 수 없는
    //    합성(shear)이면 Spawn이 실패한다.
    auto placed = ApplySpawnTransform(actor);
    record = FindActorRecord(actor);
    if (record == nullptr) {
        return finish(Result<void>::Failure(ErrorCode::Destroyed));
    }

    if (!placed) {
        auto destroyed = DestroyActor(actor);
        (void)destroyed;
        return finish(Result<void>::Failure(placed.Error()));
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
