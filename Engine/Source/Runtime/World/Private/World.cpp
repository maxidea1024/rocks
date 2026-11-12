#include <Core/Math/MathUtility.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <World/SceneComponent.h>
#include <World/World.h>

namespace rocks {

namespace {

// hook별 실행 단계다. OnTick은 설정한 단계, OnFixedTick은 Physics, OnLateTick은 LateUpdate다.
TickPhase HookPhase(uint8 hook, const TickSettings& settings) noexcept {
    switch (hook) {
    case detail::TickHookFixed:
        return TickPhase::Physics;
    case detail::TickHookLate:
        return TickPhase::LateUpdate;
    default:
        return settings.phase;
    }
}

bool HookWanted(uint8 hook, const TickSettings& settings) noexcept {
    switch (hook) {
    case detail::TickHookFixed:
        return settings.fixedTick;
    case detail::TickHookLate:
        return settings.lateTick;
    default:
        return settings.tick;
    }
}

} // namespace

bool IsValidTickSettings(const TickSettings& settings) noexcept {
    if (!math::IsFinite(settings.intervalSeconds) || settings.intervalSeconds < 0.0) {
        return false;
    }

    // OnTick 단계는 고정(Physics)·LateUpdate 단계와 겹치지 않아야 한다. 그 단계는 전용 hook을 쓴다.
    if (settings.tick) {
        return static_cast<uint32>(settings.phase) < TickPhaseCount &&
               settings.phase != TickPhase::Physics && settings.phase != TickPhase::LateUpdate;
    }

    return true;
}

const char* ActorStateName(ActorState state) noexcept {
    switch (state) {
    case ActorState::Constructed:
        return "Constructed";
    case ActorState::Initialized:
        return "Initialized";
    case ActorState::Playing:
        return "Playing";
    case ActorState::PendingDestroy:
        return "PendingDestroy";
    }

    return "Unknown";
}

const char* ComponentStateName(ComponentState state) noexcept {
    switch (state) {
    case ComponentState::Constructed:
        return "Constructed";
    case ComponentState::Initialized:
        return "Initialized";
    case ComponentState::Playing:
        return "Playing";
    case ComponentState::PendingDestroy:
        return "PendingDestroy";
    }

    return "Unknown";
}

Result<void> RegisterWorldTypes(TypeRegistry& types) noexcept {
    auto actor = RegisterNativeClass<Actor>(types);
    if (!actor) {
        return Result<void>::Failure(actor.Error());
    }
    auto component = RegisterNativeClass<Component>(types);
    if (!component) {
        return Result<void>::Failure(component.Error());
    }
    auto scene = RegisterNativeClass<SceneComponent>(types);
    if (!scene) {
        return Result<void>::Failure(scene.Error());
    }
    auto level = RegisterNativeClass<Level>(types);
    if (!level) {
        return Result<void>::Failure(level.Error());
    }
    return Result<void>::Success();
}

World::World(Allocator& allocator, TypeRegistry& types, const WorldDesc& desc) noexcept
    : allocator_(allocator), objects_(allocator, types, desc.context, desc.objects),
      name_(allocator), ticks_(allocator), subsystems_(allocator), subsystemTypes_(desc.subsystems),
      timeStep_(desc.timeStep), actors_(allocator), actorIndex_(allocator), levels_(allocator),
      pendingStart_(allocator), pendingComponentStart_(allocator), destroyActors_(allocator),
      destroyComponents_(allocator), gcBudget_(desc.gcBudget) {
    // 이름은 진단용이다. 복사에 실패하면 빈 이름으로 둔다.
    auto named = name_.TryAssign(desc.name);
    (void)named;
}

World::~World() {
    if (state_ != WorldState::Destroyed) {
        auto finished = Shutdown();
        if (!finished) {
            platform::Fatal("World destroyed while objects are still borrowed or rooted outside");
        }
    }
}

bool World::CanMutate() const noexcept {
    return state_ == WorldState::Initialized || state_ == WorldState::Playing;
}

// --- 초기화·시작 ---

Result<void> World::Initialize() noexcept {
    if (state_ != WorldState::Created) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (hookDepth_ != 0 || inTick_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    auto policy = clock_.SetPolicy(timeStep_);
    if (!policy) {
        return policy;
    }

    ObjectAccessScope scope(Access());
    auto persistent = CreateLevelInternal("Persistent", true);
    if (!persistent) {
        return Result<void>::Failure(persistent.Error());
    }
    persistentLevel_ = persistent.Value();

    // Subsystem Initialize가 Actor를 Spawn할 수 있도록 먼저 Initialized로 둔다.
    state_ = WorldState::Initialized;
    Result<void> initialized = Result<void>::Success();
    {
        HookScope hooks(*this);
        initialized = subsystems_.Initialize(subsystemTypes_, *this);
    }
    if (!initialized) {
        // 그동안 Spawn된 Actor와 Level을 정리하고 Created로 되돌린다.
        DestroyAllActors();
        pendingStart_.Clear();
        pendingComponentStart_.Clear();
        while (levels_.Size() != 0) {
            ReleaseLevel(levels_.Size() - 1);
        }
        persistentLevel_ = {};
        state_ = WorldState::Created;
        return initialized;
    }

    subsystemTypes_ = {};
    return Result<void>::Success();
}

Result<void> World::BeginPlay() noexcept {
    if (state_ != WorldState::Initialized) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (hookDepth_ != 0 || inTick_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    ObjectAccessScope scope(Access());
    HookScope hooks(*this);
    state_ = WorldState::Playing;
    for (usize index = 0; index < subsystems_.Count(); ++index) {
        subsystems_.At(index)->OnWorldBeginPlay(*this);
    }

    ProcessStarts();
    return Result<void>::Success();
}

// --- 조회 ---

World::ActorRecord* World::FindActorRecord(ObjectHandle actor) noexcept {
    const SlotHandle* slot = actorIndex_.Find(actor.slot);
    if (slot == nullptr) {
        return nullptr;
    }

    ActorRecord* record = actors_.Find(*slot);
    return record != nullptr && record->handle == actor ? record : nullptr;
}

const World::ActorRecord* World::FindActorRecord(ObjectHandle actor) const noexcept {
    const SlotHandle* slot = actorIndex_.Find(actor.slot);
    if (slot == nullptr) {
        return nullptr;
    }

    const ActorRecord* record = actors_.Find(*slot);
    return record != nullptr && record->handle == actor ? record : nullptr;
}

ObjectHandle World::ComponentAt(ObjectHandle actor, usize index) noexcept {
    auto found = With<Actor>(actor, [index](Actor& owner) noexcept {
        return index < owner.components_.Size() ? owner.components_[index] : ObjectHandle{};
    });
    return found ? found.Value() : ObjectHandle{};
}

usize World::ComponentCountOf(ObjectHandle actor) noexcept {
    auto count = With<Actor>(actor, [](Actor& owner) noexcept { return owner.components_.Size(); });
    return count ? count.Value() : 0;
}

Result<ComponentState> World::ReadComponentState(ObjectHandle component) noexcept {
    return With<Component>(component, [](Component& value) noexcept { return value.state_; });
}

bool World::HasActor(ObjectHandle actor) const noexcept {
    return FindActorRecord(actor) != nullptr;
}

Result<ActorState> World::GetActorState(ObjectHandle actor) const noexcept {
    const ActorRecord* record = FindActorRecord(actor);
    if (record == nullptr) {
        return Result<ActorState>::Failure(ErrorCode::StaleHandle);
    }

    return Result<ActorState>::Success(record->state);
}

Result<ObjectHandle> World::GetActorLevel(ObjectHandle actor) const noexcept {
    const ActorRecord* record = FindActorRecord(actor);
    if (record == nullptr) {
        return Result<ObjectHandle>::Failure(ErrorCode::StaleHandle);
    }

    return Result<ObjectHandle>::Success(record->level);
}

Result<ComponentState> World::GetComponentState(ObjectHandle component) noexcept {
    return ReadComponentState(component);
}

Result<uint32> World::GetComponents(ObjectHandle actor, Span<ObjectHandle> out) noexcept {
    if (FindActorRecord(actor) == nullptr) {
        return Result<uint32>::Failure(ErrorCode::StaleHandle);
    }

    return With<Actor>(actor, [out](Actor& owner) noexcept {
        const usize count = owner.components_.Size();
        for (usize index = 0; index < count && index < out.Size(); ++index) {
            out[index] = owner.components_[index];
        }
        return static_cast<uint32>(count);
    });
}

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
    auto reserved = levels_.TryReserve(levels_.Size() + 1);
    if (!reserved) {
        return Result<ObjectHandle>::Failure(reserved.Error());
    }

    auto created = objects_.NewObject<Level>(Access(), NativeTypeIdOf<Level>());
    if (!created) {
        return Result<ObjectHandle>::Failure(created.Error());
    }

    const ObjectHandle handle = created.Value().Handle();
    auto configured = With<Level>(handle, [&](Level& level) noexcept {
        level.world_ = this;
        level.persistent_ = persistent;
        return level.name_.TryAssign(name);
    });
    if (!configured) {
        auto requested = objects_.RequestDestroy(Access(), handle);
        (void)requested;
        return Result<ObjectHandle>::Failure(configured.Error());
    }

    // 용량을 위에서 확보했으므로 실패하지 않는다.
    auto added = levels_.TryEmplaceBack();
    added.Value()->root = Move(created.Value());
    added.Value()->handle = handle;
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
    auto marked = With<Level>(level, [](Level& value) noexcept { value.pendingDestroy_ = true; });
    (void)marked;
    // 소속 Actor를 모두 파괴 대기로 만든다(hook 없음). 정리 단계가 Actor를 정리한 뒤 Level을
    // 회수한다.
    for (const ActorRecord& actor : actors_) {
        if (actor.level == level && actor.state != ActorState::PendingDestroy) {
            auto destroyed = DestroyActor(actor.handle);
            (void)destroyed;
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
    auto requested = objects_.RequestDestroy(Access(), handle);
    (void)requested;
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
    if (!pendingStart_.TryReserve(actors_.Size() + 1) ||
        !destroyActors_.TryReserve(actors_.Size() + 1)) {
        return Result<ObjectHandle>::Failure(ErrorCode::OutOfMemory);
    }

    ObjectAccessScope scope(Access());
    auto created = objects_.NewObject<Actor>(Access(), type);
    if (!created) {
        return Result<ObjectHandle>::Failure(created.Error());
    }

    const ObjectHandle handle = created.Value().Handle();
    auto rollback = [&](ErrorCode error) noexcept {
        auto requested = objects_.RequestDestroy(Access(), handle);
        (void)requested;
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

    auto slot = actors_.TryEmplace();
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
    auto owned = With<Level>(level, [&](Level& owner) noexcept {
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

    // 1) 이미 추가된 Component의 엔진 등록(OnRegister). 실패한 Component만 정리한다.
    for (usize index = 0;; ++index) {
        const ObjectHandle component = ComponentAt(actor, index);
        if (component.IsNull()) {
            break;
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

    // 2) Actor OnAwake.
    auto awoke = With<Actor>(actor, [](Actor& value) noexcept { return value.OnAwake(); });
    record = FindActorRecord(actor);
    if (record == nullptr) {
        return finish(Result<void>::Failure(ErrorCode::Destroyed));
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

    record->state = ActorState::Initialized;

    // 4) 활성이면 OnEnable(Actor → Component).
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
    ActorRecord* record = FindActorRecord(actor);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

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
    // 실행 중 Tick에서도 즉시 빠진다(스케줄러가 저장소 정리를 단계 경계까지 미룬다).
    UnregisterTicks(record->ticks);
    for (usize index = 0;; ++index) {
        const ObjectHandle component = ComponentAt(actor, index);
        if (component.IsNull()) {
            break;
        }
        auto marked = With<Component>(component, [this](Component& value) noexcept {
            value.state_ = ComponentState::PendingDestroy;
            UnregisterTicks(value.ticks_);
        });
        (void)marked;
    }

    return Result<void>::Success();
}

void World::DestroyAllActors() noexcept {
    // 파괴 대기 표시는 hook을 부르지 않으므로 한 번의 순회로 충분하다(O(n)).
    for (const ActorRecord& record : actors_) {
        if (record.state != ActorState::PendingDestroy) {
            auto destroyed = DestroyActor(record.handle);
            (void)destroyed;
        }
    }

    FlushDestroys();
}

// --- Component ---

Result<ObjectHandle> World::AddComponent(ObjectHandle actor, TypeId type) noexcept {
    if (!CanMutate()) {
        return Result<ObjectHandle>::Failure(
            state_ == WorldState::Created ? ErrorCode::InvalidArgument : ErrorCode::ShuttingDown);
    }

    ActorRecord* record = FindActorRecord(actor);
    if (record == nullptr) {
        return Result<ObjectHandle>::Failure(ErrorCode::StaleHandle);
    }

    if (record->state == ActorState::PendingDestroy) {
        return Result<ObjectHandle>::Failure(ErrorCode::Destroyed);
    }

    // Component 하나는 각 큐에 많아야 한 번 들어간다.
    if (!destroyComponents_.TryReserve(componentCount_ + 1) ||
        !pendingComponentStart_.TryReserve(componentCount_ + 1)) {
        return Result<ObjectHandle>::Failure(ErrorCode::OutOfMemory);
    }

    ObjectAccessScope scope(Access());
    auto created = objects_.NewObject<Component>(Access(), type);
    if (!created) {
        return Result<ObjectHandle>::Failure(created.Error());
    }

    const ObjectHandle handle = created.Value().Handle();
    auto rollback = [&](ErrorCode error) noexcept {
        auto requested = objects_.RequestDestroy(Access(), handle);
        (void)requested;
        created.Value().Reset();
        return Result<ObjectHandle>::Failure(error);
    };

    auto attached = With<Component>(handle, [this, actor](Component& component) noexcept {
        component.world_ = this;
        component.owner_ = actor;
    });
    if (!attached) {
        return rollback(attached.Error());
    }

    // Actor의 강한 간선으로 소유를 넘긴다. 이후 임시 루트를 놓아도 Actor가 살아 있는 동안 보존된다.
    auto owned = With<Actor>(actor, [&](Actor& owner) noexcept {
        return objects_.AddReference(Access(), actor, owner.components_, handle);
    });
    if (!owned) {
        return rollback(owned.Error());
    }

    created.Value().Reset();
    ++componentCount_;

    // Actor가 이미 깨어 있으면(OnAwake 이후) 지금 Actor 단계까지 진행한다. 아직 Constructed이거나
    // OnAwake 실행 중이면 FinishSpawning이 OnAwake 뒤에 진행한다.
    record = FindActorRecord(actor);
    if (record != nullptr && record->awake) {
        HookScope hooks(*this);
        auto advanced = AdvanceComponent(actor, handle);
        if (!advanced) {
            return Result<ObjectHandle>::Failure(advanced.Error());
        }
    }

    return Result<ObjectHandle>::Success(handle);
}

Result<void> World::AdvanceComponent(ObjectHandle actor, ObjectHandle component) noexcept {
    // 1) 엔진 등록.
    auto needsRegister = With<Component>(component, [](Component& value) noexcept {
        return !value.registered_ && value.state_ == ComponentState::Constructed;
    });
    if (!needsRegister) {
        return Result<void>::Failure(needsRegister.Error());
    }
    if (needsRegister.Value()) {
        auto registered = With<Component>(
            component, [](Component& value) noexcept { return value.OnRegister(); });
        if (!registered) {
            auto destroyed = DestroyComponent(component);
            (void)destroyed;
            return registered;
        }
        auto marked =
            With<Component>(component, [](Component& value) noexcept { value.registered_ = true; });
        (void)marked;
    }

    // 2) OnAwake와 Tick 설정 검증.
    auto needsAwake = With<Component>(component, [](Component& value) noexcept {
        return value.registered_ && !value.awake_ && value.state_ == ComponentState::Constructed;
    });
    if (!needsAwake || !needsAwake.Value()) {
        return Result<void>::Success();
    }
    auto awoke =
        With<Component>(component, [](Component& value) noexcept { return value.OnAwake(); });
    if (!awoke) {
        auto destroyed = DestroyComponent(component);
        (void)destroyed;
        return awoke;
    }

    auto initialized = With<Component>(component, [](Component& value) noexcept {
        value.awake_ = true;
        if (value.state_ == ComponentState::PendingDestroy) {
            return Result<bool>::Success(false);
        }
        if (!IsValidTickSettings(value.tickSettings_)) {
            return Result<bool>::Failure(ErrorCode::InvalidArgument);
        }
        value.state_ = ComponentState::Initialized;
        return Result<bool>::Success(value.enabled_);
    });
    if (!initialized) {
        auto destroyed = DestroyComponent(component);
        (void)destroyed;
        return Result<void>::Failure(initialized.Error());
    }

    // 3) 소유 Actor가 활성이면 OnEnable, Playing이면 다음 프레임 시작에 OnStart.
    const ActorRecord* owner = FindActorRecord(actor);
    if (owner != nullptr && owner->enabledApplied && !owner->transitioning && initialized.Value()) {
        EnableComponent(component, true);
    }

    owner = FindActorRecord(actor);
    if (owner != nullptr && owner->state == ActorState::Playing) {
        QueueComponentStart(component);
    }

    return Result<void>::Success();
}

void World::AdvanceComponents(ObjectHandle actor) noexcept {
    // hook이 Component를 추가하면 배열이 길어지므로 크기를 매번 다시 읽는다.
    for (usize index = 0;; ++index) {
        const ActorRecord* record = FindActorRecord(actor);
        if (record == nullptr || record->state == ActorState::PendingDestroy) {
            return;
        }
        const ObjectHandle component = ComponentAt(actor, index);
        if (component.IsNull()) {
            return;
        }
        auto advanced = AdvanceComponent(actor, component);
        (void)advanced;
    }
}

Result<void> World::DestroyComponent(ObjectHandle component) noexcept {
    ObjectAccessScope scope(Access());
    auto state = ReadComponentState(component);
    if (!state) {
        return Result<void>::Failure(
            state.Error() == ErrorCode::Destroyed ? ErrorCode::Destroyed : ErrorCode::StaleHandle);
    }

    if (state.Value() == ComponentState::PendingDestroy) {
        // 소유 Actor의 파괴로 이미 대기 중이면 Actor 정리에서 함께 처리한다.
        return Result<void>::Success();
    }

    // 용량은 AddComponent에서 확보했다.
    auto queued = destroyComponents_.TryPushBack(component);
    if (!queued) {
        return queued;
    }

    return With<Component>(component, [this](Component& value) noexcept {
        value.state_ = ComponentState::PendingDestroy;
        UnregisterTicks(value.ticks_);
    });
}

// --- 활성 ---

void World::EnableActor(ObjectHandle actor, bool enabled) noexcept {
    ActorRecord* record = FindActorRecord(actor);
    if (record == nullptr || record->transitioning || record->enabledApplied == enabled ||
        !record->awake) {
        return;
    }

    // 전환 중에는 hook이 활성 상태를 다시 바꿔도 중간 상태를 노출하지 않는다(끝난 뒤 맞춘다).
    record->transitioning = true;
    if (enabled) {
        record->enabledApplied = true;
        auto tickEnabled =
            With<Actor>(actor, [](Actor& value) noexcept { return value.tickEnabled_; });
        SetTicksEnabled(record->ticks, tickEnabled && tickEnabled.Value());
        auto called = With<Actor>(actor, [](Actor& value) noexcept { value.OnEnable(); });
        (void)called;
        for (usize index = 0;; ++index) {
            const ObjectHandle component = ComponentAt(actor, index);
            if (component.IsNull()) {
                break;
            }
            auto wants = With<Component>(component, [](Component& value) noexcept {
                return value.awake_ && value.enabled_ &&
                       value.state_ != ComponentState::PendingDestroy;
            });
            if (wants && wants.Value()) {
                EnableComponent(component, true);
            }
        }
    } else {
        // 비활성화는 Component(역순) → Actor 순이다.
        for (usize index = ComponentCountOf(actor); index > 0; --index) {
            const ObjectHandle component = ComponentAt(actor, index - 1);
            if (!component.IsNull()) {
                EnableComponent(component, false);
            }
        }
        record = FindActorRecord(actor);
        if (record != nullptr) {
            record->enabledApplied = false;
            SetTicksEnabled(record->ticks, false);
        }
        auto called = With<Actor>(actor, [](Actor& value) noexcept { value.OnDisable(); });
        (void)called;
    }

    record = FindActorRecord(actor);
    if (record != nullptr) {
        record->transitioning = false;
    }
}

void World::EnableComponent(ObjectHandle component, bool enabled) noexcept {
    // 기록을 먼저 바꾸고 hook을 부른다(hook이 다시 활성 상태를 바꿔도 같은 hook이 중복되지 않는다).
    auto changed = With<Component>(component, [this, enabled](Component& value) noexcept {
        if (value.enabledApplied_ == enabled || !value.awake_) {
            return false;
        }
        value.enabledApplied_ = enabled;
        SetTicksEnabled(value.ticks_, enabled && value.tickEnabled_);
        return true;
    });
    if (!changed || !changed.Value()) {
        return;
    }

    auto called = With<Component>(component, [enabled](Component& value) noexcept {
        if (enabled) {
            value.OnEnable();
        } else {
            value.OnDisable();
        }
    });
    (void)called;

    // 비활성이라 시작을 미뤘던 Component는 소유 Actor가 Playing이면 다음 프레임 시작에 시작한다.
    if (enabled) {
        auto owner = With<Component>(component, [](Component& value) noexcept {
            return value.state_ == ComponentState::Initialized ? value.owner_ : ObjectHandle{};
        });
        const ActorRecord* record = owner ? FindActorRecord(owner.Value()) : nullptr;
        if (record != nullptr && record->state == ActorState::Playing) {
            QueueComponentStart(component);
        }
    }
}

Result<void> World::SetActorEnabled(ObjectHandle actor, bool enabled) noexcept {
    (void)enabled;
    ActorRecord* record = FindActorRecord(actor);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (record->state != ActorState::Initialized && record->state != ActorState::Playing) {
        return Result<void>::Success();
    }

    ObjectAccessScope scope(Access());
    HookScope hooks(*this);
    // Actor::enabled_가 원하는 상태다. 전환 hook이 다시 바꾸면 끝난 뒤 최종 값에 맞춘다(반복 상한).
    for (uint32 attempt = 0; attempt < 4; ++attempt) {
        record = FindActorRecord(actor);
        if (record == nullptr || record->transitioning) {
            break;
        }
        auto wanted = With<Actor>(actor, [](Actor& value) noexcept { return value.enabled_; });
        if (!wanted || wanted.Value() == record->enabledApplied) {
            break;
        }
        EnableActor(actor, wanted.Value());
    }

    return Result<void>::Success();
}

Result<void> World::SetComponentEnabled(ObjectHandle component, bool enabled) noexcept {
    ObjectAccessScope scope(Access());
    auto owner = With<Component>(component, [](Component& value) noexcept {
        return value.state_ == ComponentState::PendingDestroy || !value.awake_ ? ObjectHandle{}
                                                                               : value.owner_;
    });
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }

    const ActorRecord* record = FindActorRecord(owner.Value());
    if (record == nullptr) {
        return Result<void>::Success();
    }

    // 소유 Actor가 비활성(또는 전환 중)이면 Component 자체 플래그만 바뀌고 Actor 활성화 때
    // 반영된다.
    if (!enabled || (record->enabledApplied && !record->transitioning)) {
        HookScope hooks(*this);
        EnableComponent(component, enabled);
    }

    return Result<void>::Success();
}

Result<void> World::SetActorTickEnabled(ObjectHandle actor, bool enabled) noexcept {
    ActorRecord* record = FindActorRecord(actor);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    SetTicksEnabled(record->ticks, enabled && record->enabledApplied);
    return Result<void>::Success();
}

Result<void> World::SetComponentTickEnabled(ObjectHandle component, bool enabled) noexcept {
    return With<Component>(component, [this, enabled](Component& value) noexcept {
        SetTicksEnabled(value.ticks_, enabled && value.enabledApplied_);
    });
}

// --- Tick 등록 ---

void World::RegisterTicks(ObjectHandle object, uint8 kind, const TickSettings& settings,
                          bool enabled, TickHandle (&handles)[detail::TickHookCount]) noexcept {
    for (uint8 hook = 0; hook < detail::TickHookCount; ++hook) {
        if (!handles[hook].IsNull() || !HookWanted(hook, settings)) {
            continue;
        }
        TickRegistration registration;
        registration.target = {object, kind, hook};
        registration.phase = HookPhase(hook, settings);
        registration.priority = settings.priority;
        registration.intervalSeconds = settings.intervalSeconds;
        registration.tickWhenPaused = settings.tickWhenPaused;
        registration.enabled = enabled;
        // 설정은 FinishSpawning·AddComponent에서 검증했다. 할당 실패만 남으며 그 hook은 Tick 없이
        // 동작한다.
        auto registered = ticks_.Register(registration);
        if (registered) {
            handles[hook] = registered.Value();
        }
    }
}

void World::UnregisterTicks(TickHandle (&handles)[detail::TickHookCount]) noexcept {
    for (TickHandle& handle : handles) {
        if (!handle.IsNull()) {
            auto removed = ticks_.Unregister(handle);
            (void)removed;
            handle = {};
        }
    }
}

void World::SetTicksEnabled(TickHandle (&handles)[detail::TickHookCount], bool enabled) noexcept {
    for (TickHandle& handle : handles) {
        if (!handle.IsNull()) {
            auto changed = ticks_.SetEnabled(handle, enabled);
            (void)changed;
        }
    }
}

void World::RegisterComponentTicks(ObjectHandle component) noexcept {
    TickSettings settings;
    bool enabled = false;
    ObjectHandle owner;
    TickHandle handles[detail::TickHookCount];
    auto read = With<Component>(component, [&](Component& value) noexcept {
        settings = value.tickSettings_;
        enabled = value.enabledApplied_ && value.tickEnabled_;
        owner = value.owner_;
        for (uint32 hook = 0; hook < detail::TickHookCount; ++hook) {
            handles[hook] = value.ticks_[hook];
        }
        return value.state_ == ComponentState::Playing;
    });
    const ActorRecord* record = FindActorRecord(owner);
    if (!read || !read.Value() || record == nullptr || record->state != ActorState::Playing) {
        return;
    }

    TickHandle actorTicks[detail::TickHookCount];
    for (uint32 hook = 0; hook < detail::TickHookCount; ++hook) {
        actorTicks[hook] = record->ticks[hook];
    }

    RegisterTicks(component, detail::TickKindComponent, settings, enabled, handles);
    // 같은 단계의 소유 Actor Tick 뒤에 실행되게 한다(UE와 같은 기본 순서). OnTick 단계가 Actor와
    // 다르면 의존을 걸지 않고 단계 순서를 따른다.
    for (uint32 hook = 0; hook < detail::TickHookCount; ++hook) {
        if (handles[hook].IsNull() || actorTicks[hook].IsNull()) {
            continue;
        }
        auto ordered = ticks_.AddPrerequisite(handles[hook], actorTicks[hook]);
        (void)ordered;
    }

    auto stored = With<Component>(component, [&](Component& value) noexcept {
        for (uint32 hook = 0; hook < detail::TickHookCount; ++hook) {
            value.ticks_[hook] = handles[hook];
        }
    });
    if (!stored) {
        UnregisterTicks(handles);
    }
}

// --- 시작 ---

void World::QueueComponentStart(ObjectHandle component) noexcept {
    auto queue = With<Component>(component, [](Component& value) noexcept {
        if (value.startQueued_) {
            return false;
        }
        value.startQueued_ = true;
        return true;
    });
    if (queue && queue.Value()) {
        // 용량은 AddComponent에서 Component 수만큼 확보했다(중복 없이 한 번씩만 들어간다).
        auto pushed = pendingComponentStart_.TryPushBack(component);
        if (!pushed) {
            auto reset = With<Component>(
                component, [](Component& value) noexcept { value.startQueued_ = false; });
            (void)reset;
        }
    }
}

void World::StartComponent(ObjectHandle component) noexcept {
    // 활성이고 초기화된 Component만 시작한다. 비활성이면 활성화될 때 다시 큐에 들어간다.
    auto eligible = With<Component>(component, [](Component& value) noexcept {
        return value.state_ == ComponentState::Initialized && value.enabledApplied_;
    });
    if (!eligible || !eligible.Value()) {
        return;
    }

    auto started = With<Component>(component, [](Component& value) noexcept { value.OnStart(); });
    (void)started;
    auto playing = With<Component>(component, [](Component& value) noexcept {
        if (value.state_ != ComponentState::Initialized) {
            return false;
        }
        value.state_ = ComponentState::Playing;
        return true;
    });
    if (playing && playing.Value()) {
        RegisterComponentTicks(component);
    }
}

void World::ProcessStarts() noexcept {
    if (state_ != WorldState::Playing) {
        return;
    }

    // 1) 초기화된 Actor 중 준비된 활성 Actor를 시작한다. hook이 새 Actor를 큐 뒤에 추가할 수
    // 있으므로
    //    시작 시점의 개수만 처리하고, 아직 시작할 수 없는 항목은 앞쪽으로 모아 남긴다.
    const usize count = pendingStart_.Size();
    usize kept = 0;
    for (usize index = 0; index < count; ++index) {
        const ObjectHandle actor = pendingStart_[index];
        const ActorRecord* record = FindActorRecord(actor);
        if (record == nullptr || record->state != ActorState::Initialized) {
            continue;
        }
        // 비활성 Actor는 활성화될 때까지 시작을 미룬다(Unity와 같은 의미).
        if (!record->enabledApplied) {
            pendingStart_[kept++] = actor;
            continue;
        }
        auto ready =
            With<Actor>(actor, [](Actor& value) noexcept { return value.IsReadyToStart(); });
        if (!ready) {
            continue;
        }
        if (!ready.Value()) {
            pendingStart_[kept++] = actor;
            continue;
        }

        // Component(추가 순서) → Actor 순으로 OnStart한다.
        for (usize componentIndex = 0;; ++componentIndex) {
            record = FindActorRecord(actor);
            if (record == nullptr || record->state != ActorState::Initialized) {
                break;
            }
            const ObjectHandle component = ComponentAt(actor, componentIndex);
            if (component.IsNull()) {
                break;
            }
            StartComponent(component);
        }

        record = FindActorRecord(actor);
        if (record == nullptr || record->state != ActorState::Initialized) {
            continue;
        }
        auto started = With<Actor>(actor, [](Actor& value) noexcept { value.OnStart(); });
        (void)started;
        ActorRecord* updated = FindActorRecord(actor);
        if (updated == nullptr || updated->state != ActorState::Initialized) {
            continue;
        }
        updated->state = ActorState::Playing;
        TickSettings settings;
        bool tickEnabled = false;
        auto read = With<Actor>(actor, [&](Actor& value) noexcept {
            settings = value.tickSettings_;
            tickEnabled = value.tickEnabled_;
        });
        updated = FindActorRecord(actor);
        if (read && updated != nullptr) {
            RegisterTicks(actor, detail::TickKindActor, settings,
                          updated->enabledApplied && tickEnabled, updated->ticks);
        }
        // Actor OnStart 안에서 추가된 Component를 시작하고, 먼저 시작한 Component의 Tick을
        // 등록한다.
        for (usize componentIndex = 0;; ++componentIndex) {
            const ObjectHandle component = ComponentAt(actor, componentIndex);
            if (component.IsNull()) {
                break;
            }
            StartComponent(component);
            RegisterComponentTicks(component);
        }
    }

    // 처리한 구간을 지우고 hook이 뒤에 추가한 항목을 앞으로 당긴다.
    if (count > kept) {
        auto removed = pendingStart_.TryRemoveAt(kept, count - kept);
        (void)removed;
    }

    // 2) 이미 Playing인 Actor에 나중에 추가됐거나 뒤늦게 활성화된 Component를 시작한다.
    const usize componentCount = pendingComponentStart_.Size();
    for (usize index = 0; index < componentCount; ++index) {
        const ObjectHandle component = pendingComponentStart_[index];
        auto owner = With<Component>(component, [](Component& value) noexcept {
            value.startQueued_ = false;
            return value.owner_;
        });
        const ActorRecord* record = owner ? FindActorRecord(owner.Value()) : nullptr;
        if (record == nullptr || record->state != ActorState::Playing) {
            continue;
        }
        StartComponent(component);
    }

    if (componentCount != 0) {
        auto removed = pendingComponentStart_.TryRemoveAt(0, componentCount);
        (void)removed;
    }
}

// --- 프레임 ---

void World::ExecuteTick(const TickTarget& target, const TickContext& context) noexcept {
    if (target.kind == detail::TickKindActor) {
        const ActorRecord* record = FindActorRecord(target.object);
        if (record == nullptr || record->state != ActorState::Playing || !record->enabledApplied) {
            ++skippedTicks_;
            return;
        }
        auto called = With<Actor>(target.object, [&](Actor& actor) noexcept {
            switch (target.hook) {
            case detail::TickHookFixed:
                actor.OnFixedTick(context);
                break;
            case detail::TickHookLate:
                actor.OnLateTick(context);
                break;
            default:
                actor.OnTick(context);
                break;
            }
        });
        if (!called) {
            // Registry를 직접 조작해 우회 파괴된 Actor다. 레코드를 정리 대기로 돌린다(hook은 받지
            // 못한다).
            ++skippedTicks_;
            ++repairedTicks_;
            auto removed = ticks_.Unregister(context.handle);
            (void)removed;
            auto destroyed = DestroyActor(target.object);
            (void)destroyed;
        }
        return;
    }

    auto called = With<Component>(target.object, [&](Component& component) noexcept {
        // 파괴 대기·비활성 Component와 소유 Actor가 활성 Playing이 아닌 경우는 건너뛴다.
        const ActorRecord* owner = FindActorRecord(component.owner_);
        if (component.state_ != ComponentState::Playing || !component.enabledApplied_ ||
            owner == nullptr || owner->state != ActorState::Playing || !owner->enabledApplied) {
            return false;
        }
        switch (target.hook) {
        case detail::TickHookFixed:
            component.OnFixedTick(context);
            break;
        case detail::TickHookLate:
            component.OnLateTick(context);
            break;
        default:
            component.OnTick(context);
            break;
        }
        return true;
    });
    if (!called) {
        // 우회 파괴된 Component다. 등록을 해제하고 소유 Actor의 간선 정리를 예약한다.
        ++repairedTicks_;
        auto removed = ticks_.Unregister(context.handle);
        (void)removed;
        auto queued = destroyComponents_.TryPushBack(target.object);
        (void)queued;
    }
    if (!called || !called.Value()) {
        ++skippedTicks_;
    }
}

Result<void> World::Tick(double realDeltaSeconds) noexcept {
    if (inTick_ || hookDepth_ != 0) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (!CanMutate()) {
        return Result<void>::Failure(state_ == WorldState::Created ? ErrorCode::InvalidArgument
                                                                   : ErrorCode::ShuttingDown);
    }

    auto plan = clock_.Advance(realDeltaSeconds);
    if (!plan) {
        return Result<void>::Failure(plan.Error());
    }

    const uint64 start = platform::MonotonicNanoseconds();
    inTick_ = true;
    ObjectAccessScope scope(Access());
    HookScope hooks(*this);
    lastStep_ = plan.Value();
    ticks_.BeginFrame();
    skippedTicks_ = 0;
    for (uint64& value : phaseNanoseconds_) {
        value = 0;
    }

    ProcessStarts();

    Result<void> outcome = Result<void>::Success();
    TickPhaseFrame frame;
    frame.deltaSeconds = lastStep_.deltaSeconds;
    frame.unscaledDeltaSeconds = lastStep_.unscaledDeltaSeconds;
    frame.frameIndex = lastStep_.frameIndex;
    // 디버그 단일 스텝 프레임은 pause가 아닌 것처럼 실행한다(일반 게임플레이 Tick도 돈다).
    frame.paused = clock_.IsPaused() && !lastStep_.singleStep;
    for (uint32 phaseIndex = 0; phaseIndex < TickPhaseCount; ++phaseIndex) {
        const TickPhase phase = static_cast<TickPhase>(phaseIndex);
        const uint64 phaseStart = platform::MonotonicNanoseconds();
        if (phase == TickPhase::Physics) {
            TickPhaseFrame fixed = frame;
            fixed.deltaSeconds = lastStep_.fixedStepSeconds;
            fixed.unscaledDeltaSeconds = lastStep_.fixedStepSeconds;
            for (uint32 step = 0; step < lastStep_.fixedSteps; ++step) {
                fixed.fixedStepIndex = lastStep_.firstFixedStepIndex + step;
                auto ran = ticks_.RunPhase(phase, fixed, *this);
                if (!ran && outcome) {
                    outcome = Move(ran);
                }
            }
        } else {
            auto ran = ticks_.RunPhase(phase, frame, *this);
            if (!ran && outcome) {
                outcome = Move(ran);
            }
        }
        phaseNanoseconds_[phaseIndex] += platform::MonotonicNanoseconds() - phaseStart;
    }

    FlushDestroys();
    StepGarbageCollection();
    inTick_ = false;
    tickNanoseconds_ = platform::MonotonicNanoseconds() - start;
    return outcome;
}

// --- 파괴 ---

void World::DestroyComponentNow(ObjectHandle actor, ObjectHandle component,
                                bool ownerDestroyed) noexcept {
    if (!ownerDestroyed) {
        EnableComponent(component, false);
        auto destroying = With<Component>(component, [](Component& value) noexcept {
            const bool awake = value.awake_;
            value.awake_ = false;
            return awake;
        });
        if (destroying && destroying.Value()) {
            auto called =
                With<Component>(component, [](Component& value) noexcept { value.OnDestroy(); });
            (void)called;
        }
    }

    auto unregistering = With<Component>(component, [this](Component& value) noexcept {
        const bool registered = value.registered_;
        value.registered_ = false;
        UnregisterTicks(value.ticks_);
        return registered;
    });
    if (unregistering && unregistering.Value()) {
        auto called =
            With<Component>(component, [](Component& value) noexcept { value.OnUnregister(); });
        (void)called;
    }

    // 계층 분리는 등록 여부·파생 재정의와 무관하게 엔진이 한다.
    auto detachedScene = With<SceneComponent>(
        component, [](SceneComponent& scene) noexcept { scene.DetachForDestroy(); });
    (void)detachedScene;

    // 대표였다면 비운다. 소유 Actor도 정리 중이면 간선은 Actor 정리 끝에 한 번에 비운다(O(n) 회피).
    auto detached = With<Actor>(actor, [&](Actor& owner) noexcept {
        if (owner.rootComponent_ == component) {
            owner.rootComponent_ = {};
        }
        if (ownerDestroyed) {
            return Result<void>::Success();
        }
        return objects_.RemoveReference(Access(), actor, owner.components_, component);
    });
    if (detached && componentCount_ != 0) {
        --componentCount_;
    }

    // Object::Destroy로 우회 파괴되어 접근할 수 없는 Component도 회수 요청은 그대로 한다.
    auto requested = objects_.RequestDestroy(Access(), component);
    (void)requested;
    gcPending_ = true;
    destroyedDuringCycle_ = true;
}

void World::DestroyActorNow(ObjectHandle actor) noexcept {
    ActorRecord* record = FindActorRecord(actor);
    if (record == nullptr) {
        return;
    }

    // 1) 비활성화: Component(역순) → Actor.
    if (record->enabledApplied) {
        EnableActor(actor, false);
    }

    // 2) OnDestroy: Component(역순) → Actor. OnAwake가 성공한 객체만 받는다.
    for (usize index = ComponentCountOf(actor); index > 0; --index) {
        const ObjectHandle component = ComponentAt(actor, index - 1);
        if (component.IsNull()) {
            continue;
        }
        auto destroying = With<Component>(component, [](Component& value) noexcept {
            const bool awake = value.awake_;
            value.awake_ = false;
            return awake;
        });
        if (destroying && destroying.Value()) {
            auto called =
                With<Component>(component, [](Component& value) noexcept { value.OnDestroy(); });
            (void)called;
        }
    }

    record = FindActorRecord(actor);
    if (record != nullptr && record->awake) {
        record->awake = false;
        auto called = With<Actor>(actor, [](Actor& value) noexcept { value.OnDestroy(); });
        (void)called;
    }

    // 3) Component 등록 해제·회수 요청(역순). 파괴 대기 Actor에는 Component를 추가할 수 없으므로
    // 목록은
    //    더 늘지 않는다. 간선은 끝에 한 번에 비운다.
    for (usize index = ComponentCountOf(actor); index > 0; --index) {
        const ObjectHandle component = ComponentAt(actor, index - 1);
        if (!component.IsNull()) {
            DestroyComponentNow(actor, component, true);
        }
    }

    auto cleared = With<Actor>(actor, [&](Actor& owner) noexcept {
        return objects_.ClearReferences(Access(), actor, owner.components_);
    });
    (void)cleared;

    // 4) Level 간선 제거·회수 요청·레코드 제거. Level 전체가 정리 중이면 간선은 Level 회수 때
    // 비운다.
    record = FindActorRecord(actor);
    if (record == nullptr) {
        return;
    }

    UnregisterTicks(record->ticks);
    const ObjectHandle level = record->level;
    LevelRecord* levelRecord = FindLevelRecord(level);
    if (levelRecord != nullptr) {
        if (!levelRecord->pendingDestroy && state_ != WorldState::TearingDown) {
            auto detached = With<Level>(level, [&](Level& owner) noexcept {
                return objects_.RemoveReference(Access(), level, owner.actors_, actor);
            });
            (void)detached;
        }
        if (levelRecord->actorCount != 0) {
            --levelRecord->actorCount;
        }
    }

    auto requested = objects_.RequestDestroy(Access(), actor);
    (void)requested;
    const SlotHandle slot = *actorIndex_.Find(actor.slot);
    actorIndex_.Erase(actor.slot);
    auto removed = actors_.TryRemove(slot);
    (void)removed;
    ++destroyedTotal_;
    gcPending_ = true;
    destroyedDuringCycle_ = true;
}

void World::FlushDestroys() noexcept {
    HookScope hooks(*this);
    // 정리 hook이 다른 파괴를 요청할 수 있으므로 두 큐가 빌 때까지 반복한다. 크기를 매번 다시 읽어
    // 처리 중에 추가된 항목도 이번 정리에서 처리한다.
    while (destroyComponents_.Size() != 0 || destroyActors_.Size() != 0) {
        for (usize index = 0; index < destroyComponents_.Size(); ++index) {
            const ObjectHandle component = destroyComponents_[index];
            auto owner =
                With<Component>(component, [](Component& value) noexcept { return value.owner_; });
            if (owner) {
                // 소유 Actor도 파괴 대기면 Actor 정리에서 함께 처리한다.
                const ActorRecord* record = FindActorRecord(owner.Value());
                if (record == nullptr || record->state != ActorState::PendingDestroy) {
                    DestroyComponentNow(owner.Value(), component, false);
                }
                continue;
            }
            // Object::Destroy로 우회 파괴되어 접근할 수 없으면 소유 Actor를 찾아 간선만 정리한다.
            for (const ActorRecord& record : actors_) {
                auto contains = With<Actor>(record.handle, [component](Actor& actor) noexcept {
                    return actor.components_.Contains(component);
                });
                if (contains && contains.Value()) {
                    const ObjectHandle actor = record.handle;
                    auto detached = With<Actor>(actor, [&](Actor& value) noexcept {
                        return objects_.RemoveReference(Access(), actor, value.components_,
                                                        component);
                    });
                    if (detached && componentCount_ != 0) {
                        --componentCount_;
                    }
                    break;
                }
            }
        }
        destroyComponents_.Clear();

        for (usize index = 0; index < destroyActors_.Size(); ++index) {
            DestroyActorNow(destroyActors_[index]);
        }
        destroyActors_.Clear();
    }

    FinishLevelDestroys();
}

void World::StepGarbageCollection() noexcept {
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

    auto progress = objects_.StepCollection(Access(), gcBudget_);
    if (progress && progress.Value().IsComplete()) {
        // 이번 cycle 시작 뒤에 파괴된 객체가 있으면 다음 cycle이 필요하다.
        gcPending_ = destroyedDuringCycle_;
    }
}

Result<void> World::Shutdown() noexcept {
    if (state_ == WorldState::Destroyed) {
        return Result<void>::Success();
    }

    if (inTick_ || hookDepth_ != 0) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    ObjectAccessScope scope(Access());
    if (state_ != WorldState::TearingDown) {
        const bool initialized = state_ != WorldState::Created;
        state_ = WorldState::TearingDown;
        ticks_.Close();
        DestroyAllActors();
        pendingStart_.Clear();
        pendingComponentStart_.Clear();
        if (initialized) {
            HookScope hooks(*this);
            subsystems_.Deinitialize(*this);
        }
        // 모든 Actor가 정리됐으므로 영구 Level을 포함한 모든 Level 루트를 반환한다.
        while (levels_.Size() != 0) {
            ReleaseLevel(levels_.Size() - 1);
        }
        persistentLevel_ = {};
    }

    // 진행 중인 증분 GC를 끝내거나 취소한 뒤 전체 수집으로 남은 저장소를 회수한다.
    if (objects_.CollectionPhase() == GcPhase::Marking) {
        auto cancelled = objects_.CancelCollection(Access());
        (void)cancelled;
    }

    for (uint32 attempt = 0; attempt < 64 && objects_.CollectionPhase() != GcPhase::Idle;
         ++attempt) {
        auto stepped = objects_.StepCollection(Access(), GcBudget{1u << 20, 0});
        if (!stepped) {
            return Result<void>::Failure(stepped.Error());
        }
    }

    for (uint32 attempt = 0; attempt < 8; ++attempt) {
        auto collected = objects_.Collect(Access());
        if (!collected) {
            return Result<void>::Failure(collected.Error());
        }
        if (collected.Value().reclaimed == 0) {
            break;
        }
    }

    auto closed = objects_.Shutdown(Access());
    if (!closed) {
        return closed;
    }

    gcPending_ = false;
    state_ = WorldState::Destroyed;
    return Result<void>::Success();
}

WorldStats World::Stats() const noexcept {
    WorldStats stats;
    stats.levels = static_cast<uint32>(levels_.Size());
    stats.actors = static_cast<uint32>(actors_.Size());
    stats.components = componentCount_;
    for (const ActorRecord& record : actors_) {
        if (record.state == ActorState::PendingDestroy) {
            ++stats.pendingDestroyActors;
        }
    }

    stats.pendingDestroyComponents = static_cast<uint32>(destroyComponents_.Size());
    stats.waitingToStart = static_cast<uint32>(pendingStart_.Size());
    stats.spawnedTotal = spawnedTotal_;
    stats.destroyedTotal = destroyedTotal_;
    stats.ticks = ticks_.Stats();
    for (uint32 index = 0; index < TickPhaseCount; ++index) {
        stats.phaseNanoseconds[index] = phaseNanoseconds_[index];
    }

    stats.tickNanoseconds = tickNanoseconds_;
    stats.skippedTicks = skippedTicks_;
    stats.repairedTicks = repairedTicks_;
    stats.externalBorrows = objects_.Stats().borrows;
    return stats;
}

} // namespace rocks
