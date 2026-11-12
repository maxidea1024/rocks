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
        // OnTick 단계는 고정·LateUpdate 단계와 겹치지 않게 제한한다.
        return settings.tick && settings.phase != TickPhase::Physics &&
               settings.phase != TickPhase::LateUpdate &&
               static_cast<uint32>(settings.phase) < TickPhaseCount;
    }
}

} // namespace

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

Result<void> World::Initialize() noexcept {
    if (state_ != WorldState::Created) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto policy = clock_.SetPolicy(timeStep_);
    if (!policy) {
        return policy;
    }

    ObjectAccessScope scope(Access());
    // 영구 Level을 먼저 만든다(Subsystem 초기화가 Actor를 Spawn할 수 있다).
    if (persistentLevel_.IsNull()) {
        auto persistent = CreateLevelInternal("Persistent", true);
        if (!persistent) {
            return Result<void>::Failure(persistent.Error());
        }
        persistentLevel_ = persistent.Value();
    }

    // Subsystem Initialize는 아직 Created 상태라 Spawn을 거부하지 않도록 잠시 Initialized로 둔다.
    state_ = WorldState::Initialized;
    auto initialized = subsystems_.Initialize(subsystemTypes_, *this);
    if (!initialized) {
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

    if (inTick_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    ObjectAccessScope scope(Access());
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

Result<void> World::SetComponentState(ObjectHandle component, ComponentState state) noexcept {
    return With<Component>(component, [state](Component& value) noexcept { value.state_ = state; });
}

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

Result<ObjectHandle> World::GetActorLevel(ObjectHandle actor) const noexcept {
    const ActorRecord* record = FindActorRecord(actor);
    if (record == nullptr) {
        return Result<ObjectHandle>::Failure(ErrorCode::StaleHandle);
    }
    return Result<ObjectHandle>::Success(record->level);
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

    record->pendingDestroy = true;
    auto marked = With<Level>(level, [](Level& value) noexcept { value.pendingDestroy_ = true; });
    (void)marked;
    // 소속 Actor를 모두 파괴 대기로 만든다. 정리 단계가 Actor를 정리한 뒤 Level을 회수한다.
    for (const ActorRecord& actor : actors_) {
        if (actor.level == level && actor.state != ActorState::PendingDestroy) {
            // 파괴 큐 용량은 Spawn에서 확보했다.
            auto queued = destroyActors_.TryPushBack(actor.handle);
            (void)queued;
        }
    }
    for (ActorRecord& actor : actors_) {
        if (actor.level == level && actor.state != ActorState::PendingDestroy) {
            actor.state = ActorState::PendingDestroy;
            UnregisterTicks(actor.ticks);
            const ObjectHandle handle = actor.handle;
            for (usize index = 0;; ++index) {
                const ObjectHandle component = ComponentAt(handle, index);
                if (component.IsNull()) {
                    break;
                }
                auto pending = With<Component>(component, [this](Component& value) noexcept {
                    value.state_ = ComponentState::PendingDestroy;
                    UnregisterTicks(value.ticks_);
                });
                (void)pending;
            }
        }
    }

    // Tick 밖에서 요청했고 소속 Actor가 없으면 바로 회수한다.
    if (!inTick_) {
        FinishLevelDestroys();
    }

    return Result<void>::Success();
}

void World::ReleaseLevel(usize index) noexcept {
    const ObjectHandle handle = levels_[index].handle;
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
        if (!levels_[index - 1].pendingDestroy) {
            continue;
        }
        auto empty = With<Level>(levels_[index - 1].handle,
                                 [](Level& level) noexcept { return level.actors_.IsEmpty(); });
        if (!empty || empty.Value()) {
            ReleaseLevel(index - 1);
        }
    }
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

// --- Spawn ---

Result<ObjectHandle> World::SpawnActorDeferred(TypeId type, const SpawnParams& params) noexcept {
    if (!CanMutate()) {
        return Result<ObjectHandle>::Failure(
            state_ == WorldState::Created ? ErrorCode::InvalidArgument : ErrorCode::ShuttingDown);
    }

    const ObjectHandle level = params.level.IsNull() ? persistentLevel_ : params.level;
    const LevelRecord* levelRecord = FindLevelRecord(level);
    if (levelRecord == nullptr) {
        return Result<ObjectHandle>::Failure(ErrorCode::StaleHandle);
    }
    if (levelRecord->pendingDestroy) {
        return Result<ObjectHandle>::Failure(ErrorCode::Destroyed);
    }

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

    // 정리 단계에서 할당 실패로 시작·파괴 큐에 넣지 못하는 일이 없도록 여유를 먼저 확보한다.
    if (!pendingStart_.TryReserve(pendingStart_.Size() + 1) ||
        !destroyActors_.TryReserve(actors_.Size() + 1)) {
        return rollback(ErrorCode::OutOfMemory);
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
    auto attached = With<Actor>(handle, [this, level](Actor& actor) noexcept {
        actor.world_ = this;
        actor.level_ = level;
    });
    if (!attached) {
        platform::Fatal("Spawned actor is not accessible");
    }

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

    if (record->state != ActorState::Constructed) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ObjectAccessScope scope(Access());

    // 1) Component 엔진 등록(OnRegister). hook이 Component를 추가해도 매번 다시 읽는다.
    for (usize index = 0;; ++index) {
        const ObjectHandle component = ComponentAt(actor, index);
        if (component.IsNull()) {
            break;
        }
        auto state = ReadComponentState(component);
        if (!state || state.Value() != ComponentState::Constructed) {
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

    // 2) Actor OnAwake.
    auto awoke = With<Actor>(actor, [](Actor& value) noexcept { return value.OnAwake(); });
    record = FindActorRecord(actor);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::Destroyed);
    }

    if (!awoke) {
        // OnDestroy 없이 정리한다(awake = false). 등록된 Component는 OnUnregister를 받는다.
        auto destroyed = DestroyActor(actor);
        (void)destroyed;
        return awoke;
    }

    record->awake = true;
    if (record->state == ActorState::PendingDestroy) {
        // OnAwake 안에서 스스로 파괴를 요청했다. Spawn은 성공이며 정리 단계에서 OnDestroy를 받는다.
        return Result<void>::Success();
    }

    // 3) Component OnAwake. 실패한 Component만 정리한다.
    for (usize index = 0;; ++index) {
        record = FindActorRecord(actor);
        if (record == nullptr || record->state == ActorState::PendingDestroy) {
            break;
        }
        const ObjectHandle component = ComponentAt(actor, index);
        if (component.IsNull()) {
            break;
        }
        auto eligible = With<Component>(component, [](Component& value) noexcept {
            return value.registered_ && !value.awake_ &&
                   value.state_ == ComponentState::Constructed;
        });
        if (!eligible || !eligible.Value()) {
            continue;
        }
        auto componentAwoke =
            With<Component>(component, [](Component& value) noexcept { return value.OnAwake(); });
        if (componentAwoke) {
            auto marked = With<Component>(component, [](Component& value) noexcept {
                value.awake_ = true;
                if (value.state_ == ComponentState::Constructed) {
                    value.state_ = ComponentState::Initialized;
                }
            });
            (void)marked;
        } else {
            auto destroyed = DestroyComponent(component);
            (void)destroyed;
        }
    }

    record = FindActorRecord(actor);
    if (record == nullptr || record->state == ActorState::PendingDestroy) {
        return Result<void>::Success();
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
    return Result<void>::Success();
}

Result<void> World::DestroyActor(ObjectHandle actor) noexcept {
    ActorRecord* record = FindActorRecord(actor);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (record->state == ActorState::PendingDestroy) {
        return Result<void>::Success();
    }

    auto queued = destroyActors_.TryPushBack(actor);
    if (!queued) {
        return queued;
    }

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

    if (!destroyComponents_.TryReserve(componentCount_ + 1) ||
        !pendingComponentStart_.TryReserve(pendingComponentStart_.Size() + 1)) {
        return Result<ObjectHandle>::Failure(ErrorCode::OutOfMemory);
    }

    auto created = objects_.NewObject<Component>(Access(), type);
    if (!created) {
        return Result<ObjectHandle>::Failure(created.Error());
    }

    const ObjectHandle handle = created.Value().Handle();
    auto attached = With<Component>(handle, [this, actor](Component& component) noexcept {
        component.world_ = this;
        component.owner_ = actor;
    });
    if (!attached) {
        platform::Fatal("Added component is not accessible");
    }

    // Actor의 강한 간선으로 소유를 넘긴다. 이후 임시 루트를 놓아도 Actor가 살아 있는 동안 보존된다.
    auto owned = With<Actor>(actor, [&](Actor& owner) noexcept {
        return objects_.AddReference(Access(), actor, owner.components_, handle);
    });
    if (!owned) {
        auto requested = objects_.RequestDestroy(Access(), handle);
        (void)requested;
        created.Value().Reset();
        return Result<ObjectHandle>::Failure(owned.Error());
    }

    created.Value().Reset();
    ++componentCount_;
    record = FindActorRecord(actor);
    if (record != nullptr &&
        (record->state == ActorState::Initialized || record->state == ActorState::Playing)) {
        ObjectAccessScope scope(Access());
        auto initialized = InitializeComponent(actor, handle);
        if (!initialized) {
            return Result<ObjectHandle>::Failure(initialized.Error());
        }
    }

    return Result<ObjectHandle>::Success(handle);
}

Result<void> World::InitializeComponent(ObjectHandle actor, ObjectHandle component) noexcept {
    auto registered =
        With<Component>(component, [](Component& value) noexcept { return value.OnRegister(); });
    if (!registered) {
        auto destroyed = DestroyComponent(component);
        (void)destroyed;
        return registered;
    }

    auto marked =
        With<Component>(component, [](Component& value) noexcept { value.registered_ = true; });
    (void)marked;
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
            return false;
        }
        value.state_ = ComponentState::Initialized;
        return value.enabled_;
    });
    if (!initialized) {
        return Result<void>::Failure(initialized.Error());
    }

    const ActorRecord* owner = FindActorRecord(actor);
    if (owner != nullptr && owner->enabledApplied && initialized.Value()) {
        EnableComponent(component, true);
    }

    owner = FindActorRecord(actor);
    if (owner != nullptr && owner->state == ActorState::Playing) {
        // 시작은 다음 프레임 시작에서 한다(다른 객체의 hook 실행 중 OnStart 재진입 방지).
        auto queued = pendingComponentStart_.TryPushBack(component);
        (void)queued;
    }

    return Result<void>::Success();
}

Result<void> World::DestroyComponent(ObjectHandle component) noexcept {
    auto state = ReadComponentState(component);
    if (!state) {
        return Result<void>::Failure(
            state.Error() == ErrorCode::Destroyed ? ErrorCode::Destroyed : ErrorCode::StaleHandle);
    }

    if (state.Value() == ComponentState::PendingDestroy) {
        // 소유 Actor의 파괴로 이미 대기 중이면 Actor 정리에서 함께 처리한다.
        return Result<void>::Success();
    }

    auto queued = destroyComponents_.TryPushBack(component);
    if (!queued) {
        return queued;
    }

    auto marked = With<Component>(component, [this](Component& value) noexcept {
        value.state_ = ComponentState::PendingDestroy;
        UnregisterTicks(value.ticks_);
    });
    return marked;
}

// --- 활성 ---

void World::EnableActor(ObjectHandle actor, bool enabled) noexcept {
    ActorRecord* record = FindActorRecord(actor);
    if (record == nullptr || record->enabledApplied == enabled || !record->awake) {
        return;
    }

    if (enabled) {
        record->enabledApplied = true;
        auto tickEnabled =
            With<Actor>(actor, [](Actor& value) noexcept { return value.tickEnabled_; });
        SetTicksEnabled(record->ticks, tickEnabled && tickEnabled.Value());
        auto called = With<Actor>(actor, [](Actor& value) noexcept { value.OnEnable(); });
        (void)called;
        for (usize index = 0;; ++index) {
            record = FindActorRecord(actor);
            if (record == nullptr || !record->enabledApplied) {
                break;
            }
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
        return;
    }

    // 비활성화는 Component(역순) → Actor 순이다.
    for (usize index = ComponentCountOf(actor); index > 0; --index) {
        const ObjectHandle component = ComponentAt(actor, index - 1);
        if (!component.IsNull()) {
            EnableComponent(component, false);
        }
    }

    record = FindActorRecord(actor);
    if (record == nullptr || !record->enabledApplied) {
        return;
    }

    record->enabledApplied = false;
    SetTicksEnabled(record->ticks, false);
    auto called = With<Actor>(actor, [](Actor& value) noexcept { value.OnDisable(); });
    (void)called;
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
}

Result<void> World::SetActorEnabled(ObjectHandle actor, bool enabled) noexcept {
    ActorRecord* record = FindActorRecord(actor);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (record->state == ActorState::Initialized || record->state == ActorState::Playing) {
        ObjectAccessScope scope(Access());
        EnableActor(actor, enabled);
    }

    return Result<void>::Success();
}

Result<void> World::SetComponentEnabled(ObjectHandle component, bool enabled) noexcept {
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

    // 소유 Actor가 비활성이면 Component 자체 플래그만 바뀌고 Actor 활성화 때 반영된다.
    if (!enabled || record->enabledApplied) {
        ObjectAccessScope scope(Access());
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
        auto registered = ticks_.Register(registration);
        // 잘못된 interval이나 할당 실패는 그 hook을 등록하지 않는다(객체는 Tick 없이 동작한다).
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
    auto read = With<Component>(component, [&](Component& value) noexcept {
        settings = value.tickSettings_;
        enabled = value.enabledApplied_ && value.tickEnabled_;
        owner = value.owner_;
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

    TickHandle handles[detail::TickHookCount];
    auto existing = With<Component>(component, [&](Component& value) noexcept {
        for (uint32 hook = 0; hook < detail::TickHookCount; ++hook) {
            handles[hook] = value.ticks_[hook];
        }
    });
    if (!existing) {
        return;
    }

    RegisterTicks(component, detail::TickKindComponent, settings, enabled, handles);
    // 같은 단계의 소유 Actor Tick 뒤에 실행되게 한다(UE와 같은 기본 순서). 단계가 다르면 단계
    // 순서로 이미 정해진다.
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

// --- 프레임 ---

void World::ProcessStarts() noexcept {
    if (state_ != WorldState::Playing) {
        return;
    }

    // Component 하나를 시작하고 Tick을 등록한다. 시작 대상이 아니면 아무것도 하지 않는다.
    auto startComponent = [this](ObjectHandle component) noexcept {
        auto state = ReadComponentState(component);
        if (!state || state.Value() != ComponentState::Initialized) {
            return;
        }
        auto started =
            With<Component>(component, [](Component& value) noexcept { value.OnStart(); });
        (void)started;
        auto playing = With<Component>(component, [](Component& value) noexcept {
            if (value.state_ != ComponentState::Initialized) {
                return false;
            }
            value.state_ = ComponentState::Playing;
            return true;
        });
        // Tick 등록은 소유 Actor가 Playing이 된 뒤에 한다(Actor Tick 뒤에 오도록 선행 의존을 건다).
        if (playing && playing.Value()) {
            RegisterComponentTicks(component);
        }
    };

    // 1) 초기화된 Actor 중 준비된 것을 시작한다. hook이 새 Actor를 큐 뒤에 추가할 수 있으므로 시작
    // 시점의
    //    개수만 처리하고, 준비되지 않은 항목은 앞쪽으로 모아 남긴다.
    const usize count = pendingStart_.Size();
    usize kept = 0;
    for (usize index = 0; index < count; ++index) {
        const ObjectHandle actor = pendingStart_[index];
        const ActorRecord* record = FindActorRecord(actor);
        if (record == nullptr || record->state != ActorState::Initialized) {
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
            startComponent(component);
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
        // Actor Tick을 등록한 뒤 이미 시작한 Component의 Tick을 등록한다.
        for (usize componentIndex = 0;; ++componentIndex) {
            const ObjectHandle component = ComponentAt(actor, componentIndex);
            if (component.IsNull()) {
                break;
            }
            RegisterComponentTicks(component);
        }
    }

    // 처리한 구간을 지우고 hook이 뒤에 추가한 항목을 앞으로 당긴다.
    if (count > kept) {
        auto removed = pendingStart_.TryRemoveAt(kept, count - kept);
        (void)removed;
    }

    // 2) 이미 Playing인 Actor에 나중에 추가된 Component를 시작한다. 소유 Actor가 아직 시작 전이면
    //    Actor 시작 때 함께 시작하므로 여기서는 건너뛴다.
    const usize componentCount = pendingComponentStart_.Size();
    for (usize index = 0; index < componentCount; ++index) {
        const ObjectHandle component = pendingComponentStart_[index];
        auto owner =
            With<Component>(component, [](Component& value) noexcept { return value.owner_; });
        const ActorRecord* record = owner ? FindActorRecord(owner.Value()) : nullptr;
        if (record == nullptr || record->state != ActorState::Playing) {
            continue;
        }
        startComponent(component);
    }

    if (componentCount != 0) {
        auto removed = pendingComponentStart_.TryRemoveAt(0, componentCount);
        (void)removed;
    }
}

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
            ++skippedTicks_;
        }
        return;
    }

    auto called = With<Component>(target.object, [&](Component& component) noexcept {
        // 파괴 대기·비활성 Component는 건너뛴다(같은 단계 안에서 파괴 요청된 경우 포함).
        if (component.state_ != ComponentState::Playing || !component.enabledApplied_) {
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
    if (!called || !called.Value()) {
        ++skippedTicks_;
    }
}

Result<void> World::Tick(double realDeltaSeconds) noexcept {
    if (inTick_) {
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

    // 소유 Actor의 강한 간선에서 빼고(대표였다면 비우고) 저장소 회수를 요청한다. Object::Destroy로
    // 우회 파괴되어 접근할 수 없는 Component도 간선 제거와 회수 요청은 그대로 진행한다.
    auto detached = With<Actor>(actor, [&](Actor& owner) noexcept {
        if (owner.rootComponent_ == component) {
            owner.rootComponent_ = {};
        }
        return objects_.RemoveReference(Access(), actor, owner.components_, component);
    });
    if (detached) {
        --componentCount_;
    }

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

    // 3) Component 등록 해제·간선 제거·회수 요청(역순). 접근할 수 없는 Actor면 간선은 Actor와 함께
    // 회수된다.
    for (usize guard = ComponentCountOf(actor) + 1; guard > 0; --guard) {
        const usize remaining = ComponentCountOf(actor);
        if (remaining == 0) {
            break;
        }
        DestroyComponentNow(actor, ComponentAt(actor, remaining - 1), true);
    }

    // 4) Actor 회수 요청과 레코드 제거.
    record = FindActorRecord(actor);
    if (record == nullptr) {
        return;
    }

    UnregisterTicks(record->ticks);
    const ObjectHandle level = record->level;
    auto detached = With<Level>(level, [&](Level& owner) noexcept {
        return objects_.RemoveReference(Access(), level, owner.actors_, actor);
    });
    (void)detached;
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
    // 정리 hook이 다른 파괴를 요청할 수 있으므로 두 큐가 빌 때까지 반복한다. 크기를 매번 다시 읽어
    // 처리 중에 추가된 항목도 이번 정리에서 처리한다.
    while (destroyComponents_.Size() != 0 || destroyActors_.Size() != 0) {
        for (usize index = 0; index < destroyComponents_.Size(); ++index) {
            const ObjectHandle component = destroyComponents_[index];
            auto owner =
                With<Component>(component, [](Component& value) noexcept { return value.owner_; });
            if (owner) {
                DestroyComponentNow(owner.Value(), component, false);
                continue;
            }
            // Object::Destroy로 우회 파괴되어 접근할 수 없으면 소유 Actor를 찾아 간선만 정리한다.
            for (const ActorRecord& record : actors_) {
                auto contains = With<Actor>(record.handle, [component](Actor& actor) noexcept {
                    return actor.components_.Contains(component);
                });
                if (contains && contains.Value()) {
                    DestroyComponentNow(record.handle, component, true);
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

    if (inTick_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    ObjectAccessScope scope(Access());
    if (state_ != WorldState::TearingDown) {
        const bool initialized = state_ != WorldState::Created;
        state_ = WorldState::TearingDown;
        ticks_.Close();
        // 모든 Actor를 파괴 대기로 만들고(같은 규칙: Tick 해제·Component 대기) 한 번에 정리한다.
        for (usize guard = actors_.Size() + 1; guard > 0; --guard) {
            ObjectHandle next;
            for (const ActorRecord& record : actors_) {
                if (record.state != ActorState::PendingDestroy) {
                    next = record.handle;
                    break;
                }
            }
            if (next.IsNull()) {
                break;
            }
            auto destroyed = DestroyActor(next);
            (void)destroyed;
        }
        FlushDestroys();
        pendingStart_.Clear();
        pendingComponentStart_.Clear();
        if (initialized) {
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
    return stats;
}

} // namespace rocks
