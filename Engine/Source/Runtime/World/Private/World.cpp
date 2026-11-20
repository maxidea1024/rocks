#include "WorldInternal.h"

#include <Config/ConfigContext.h>
#include <Core/Assert.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Math/MathUtility.h>
#include <Platform/Fatal.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <World/World.module.generated.h>
#include <World/WorldLog.h>

namespace rocks {

namespace {

// World Registry는 Level 클러스터를 쓴다(Docs/Design/GarbageCollectionScale.ko.md 13.3절).
RegistryOptions WorldRegistryOptions(const WorldDesc& desc) noexcept {
    RegistryOptions options = desc.objects;
    options.clusters = options.clusters || desc.levelClusters;
    return options;
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

Result<void> RegisterWorldTypes(ModuleRegistration& registration) noexcept {
    // 생성 집계 함수가 의존 모듈(CoreTypeBindings)부터 base 먼저 등록하고, 이미 들어온 모듈·이미
    // 등록된 같은 C++ 타입은 lease로 붙잡아 성공한다(멱등). 실패하면 이 호출의 기록을 되돌린다.
    return generated::World::RegisterModuleTypes(registration);
}

World::World(Allocator& allocator, TypeRegistry& types, const WorldDesc& desc) noexcept
    : allocator_(allocator), objects_(allocator, types, desc.context, WorldRegistryOptions(desc)),
      name_(allocator), ticks_(allocator), subsystems_(allocator), subsystemTypes_(desc.subsystems),
      timeStep_(desc.timeStep), actors_(allocator), actorIndex_(allocator), levels_(allocator),
      levelSlots_(allocator), componentRecords_(allocator), pendingStart_(allocator),
      pendingComponentStart_(allocator), destroyActors_(allocator), destroyComponents_(allocator),
      gcBudget_(desc.gcBudget), gcScheduler_(desc.gcScheduler), settings_(desc.settings),
      gcSchedule_(desc.gcSchedule) {
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

void World::ReleaseCVarScope() noexcept {
    // 범위를 파괴한다(그 범위의 값은 버리고 자식은 Global에 다시 붙는다 — CVarSystem 8절). 실패는
    // 조용히 버리지 않는다: 다른 스레드에서 World를 끝냈거나(WrongThread) 누가 범위를 먼저
    // 파괴했다(NotFound — 범위는 World가 소유한다). Warning을 남기고 Debug 빌드에서 assert한다.
    if (settings_ == nullptr || cvarScope_.IsGlobal()) {
        return;
    }

    auto destroyed = settings_->DestroyScope(cvarScope_);
    if (!destroyed) {
        Log::Warning(LogWorld, "World {World} could not destroy its CVar scope {Scope}: {Error}",
                     name_.View(), cvarScope_.value, ErrorCodeName(destroyed.Error()));
        if constexpr (IsDebugBuild) {
            ROCKS_ASSERT(destroyed.IsOk());
        }
    }

    cvarScope_ = {};
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

    // 설정 문맥이 있으면 이 World의 CVar 범위를 만든다(Global의 자식 — CVarSystem 14.2절).
    if (settings_ != nullptr) {
        auto created = settings_->CreateScope();
        if (!created) {
            return Result<void>::Failure(created.Error());
        }

        cvarScope_ = created.Value();
    }

    ObjectAccessScope scope(Access());
    auto persistent = CreateLevelInternal("Persistent", true);
    if (!persistent) {
        ReleaseCVarScope();
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
        ReleaseCVarScope();
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

World::ComponentSlot World::ComponentSlotAt(ObjectHandle actor, usize index) noexcept {
    auto found = With<Actor>(actor, [index](Actor& owner) noexcept {
        ComponentSlot slot;
        slot.count = owner.components_.Size();
        if (index < slot.count) {
            slot.component = owner.components_[index];
        }

        return slot;
    });
    return found ? found.Value() : ComponentSlot{};
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
        ReleaseCVarScope();
    }

    // 진행 중인 증분 GC를 끝내거나 취소한 뒤 전체 수집으로 남은 저장소를 회수한다.
    if (objects_.CollectionPhase() == GcPhase::Marking) {
        auto cancelled = objects_.CancelCollection(Access());
        (void)cancelled;
    }

    for (uint32 attempt = 0;
         attempt < detail::FullCollectionFinishSteps && objects_.CollectionPhase() != GcPhase::Idle;
         ++attempt) {
        auto stepped =
            objects_.StepCollection(Access(), detail::UnboundedFinishBudget, gcScheduler_);
        if (!stepped) {
            return Result<void>::Failure(stepped.Error());
        }
    }

    for (uint32 attempt = 0; attempt < 8; ++attempt) {
        auto collected = objects_.Collect(Access(), gcScheduler_);
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
    stats.repairedBypasses = repairedBypasses_;
    stats.externalBorrows = objects_.Stats().borrows;
    return stats;
}

} // namespace rocks
