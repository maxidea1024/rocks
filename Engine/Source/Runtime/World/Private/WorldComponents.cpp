#include <Object/ConstructionContext.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <World/WorldLog.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogContext.h>

namespace rocks {

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

    // Component 하나는 각 큐에 많아야 한 번 들어간다. Actor 레코드의 Component 목록도 먼저 늘린다.
    if (!destroyComponents_.TryEnsureCapacity(componentCount_ + 1) ||
        !pendingComponentStart_.TryEnsureCapacity(componentCount_ + 1) ||
        !record->components.TryEnsureCapacity(record->components.Size() + 1)) {
        return Result<ObjectHandle>::Failure(ErrorCode::OutOfMemory);
    }

    ObjectAccessScope scope(Access());
    // Actor가 Level 클러스터 소속이면 Component도 같은 클러스터에 가입한다(13.3절 — Actor가 소속이
    // 아니면 Component의 소유 간선이 클러스터 밖이라 넣지 않는다).
    Result<RootRef<Object>> created = Result<RootRef<Object>>::Failure(ErrorCode::InvalidArgument);
    {
        ConstructionContext transaction(objects_, Access());
        auto pending = transaction.Create<Component>(type);
        if (!pending) {
            return Result<ObjectHandle>::Failure(pending.Error());
        }

        if (record->clustered) {
            auto joined = transaction.JoinCluster(pending.Value(), record->level);
            if (!joined) {
                return Result<ObjectHandle>::Failure(joined.Error());
            }
        }

        created = transaction.Commit();
        if (!created) {
            return Result<ObjectHandle>::Failure(created.Error());
        }
    }

    const ObjectHandle handle = created.Value().Handle();
    auto rollback = [&](ErrorCode error) noexcept {
        componentRecords_.Erase(handle.slot);
        RequestDestroyOwned(handle);
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

    // 타입 규칙·EditorOnly(23.2절) — 등록 전이라 hook을 하나도 부르지 않는다.
    if (auto allowed = CheckComponentRules(actor, handle); !allowed) {
        return rollback(allowed.Error());
    }

    ComponentRecord entry;
    entry.component = handle;
    entry.actor = actor;
    auto indexed = componentRecords_.Insert(handle.slot, entry);
    if (!indexed) {
        return rollback(indexed.Error());
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
    // 용량을 위에서 확보했으므로 실패하지 않는다.
    record = FindActorRecord(actor);
    auto listed = record->components.TryPushBack(handle);
    (void)listed;

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
        if (auto registered = RegisterComponentNow(component); !registered) {
            auto destroyed = DestroyComponent(component);
            (void)destroyed;
            return registered;
        }
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
    // hook이 Component를 추가하면 배열이 길어지므로 크기를 매번 다시 읽는다. null 원소는 건너뛴다.
    for (usize index = 0;; ++index) {
        const ActorRecord* record = FindActorRecord(actor);
        if (record == nullptr || record->state == ActorState::PendingDestroy) {
            return;
        }

        const ComponentSlot slot = ComponentSlotAt(actor, index);
        if (index >= slot.count) {
            return;
        }

        if (slot.component.IsNull()) {
            continue;
        }

        auto advanced = AdvanceComponent(actor, slot.component);
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

    // 이 registry에 있지만 AddComponent를 거치지 않은 객체(직접 생성·그래프 로드)는 수명 기록과
    // 소유 Actor가 없어 정리할 수 없다(정리 단계의 계층 분리가 World를 요구한다).
    auto ledger = With<Component>(
        component, [this](Component& value) noexcept { return value.world_ == this; });
    if (!ledger || !ledger.Value()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
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
        value.endReason_ = EndPlayReason::Destroyed;
        value.state_ = ComponentState::PendingDestroy;
        UnregisterTicks(value.ticks_);
    });
}

const char* SystemStateKindName(SystemStateKind kind) noexcept {
    switch (kind) {
    case SystemStateKind::Render:
        return "Render";
    case SystemStateKind::Physics:
        return "Physics";
    case SystemStateKind::Audio:
        return "Audio";
    case SystemStateKind::Count:
        break;
    }

    return "Unknown";
}

Result<void> Component::MarkSystemStateDirty(SystemStateKind kind) noexcept {
    const uint32 index = static_cast<uint32>(kind);
    if (index >= SystemStateKindCount) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 등록 전·해제 뒤·파괴 대기·그 종류 없음은 할 일이 없다.
    if (world_ == nullptr || !registered_ || state_ == ComponentState::PendingDestroy ||
        (systemStates_ & (1u << index)) == 0) {
        return Result<void>::Success();
    }

    // 처음 표시할 때만 목록에 넣는다(한 Component는 목록에 많아야 한 번 — 23.2절).
    if (dirtySystemStates_ == 0) {
        if (auto queued = world_->systemStateUpdates_.TryPushBack(Handle()); !queued) {
            return queued;
        }
    }

    dirtySystemStates_ = static_cast<uint8>(dirtySystemStates_ | (1u << index));
    return Result<void>::Success();
}

Result<void> World::RegisterComponentNow(ObjectHandle component) noexcept {
    auto registered =
        With<Component>(component, [](Component& value) noexcept { return value.OnRegister(); });
    if (!registered) {
        return registered;
    }

    // 시스템 상태(23.2절): Render → Physics → Audio. 실패하면 만든 것을 역순으로 지우고
    // OnUnregister.
    return With<Component>(component, [](Component& value) noexcept -> Result<void> {
        for (uint32 index = 0; index < SystemStateKindCount; ++index) {
            const auto kind = static_cast<SystemStateKind>(index);
            if (!value.WantsSystemState(kind)) {
                continue;
            }

            if (auto created = value.CreateSystemState(kind); !created) {
                for (uint32 back = index; back > 0; --back) {
                    const auto made = static_cast<SystemStateKind>(back - 1);
                    if (value.HasSystemState(made)) {
                        value.DestroySystemState(made);
                    }
                }

                value.systemStates_ = 0;
                value.OnUnregister();
                return created;
            }

            value.systemStates_ = static_cast<uint8>(value.systemStates_ | (1u << index));
        }

        value.registered_ = true;
        return Result<void>::Success();
    });
}

void World::DestroySystemStates(Component& component) noexcept {
    for (uint32 index = SystemStateKindCount; index > 0; --index) {
        const auto kind = static_cast<SystemStateKind>(index - 1);
        if (component.HasSystemState(kind)) {
            component.DestroySystemState(kind);
        }
    }

    component.systemStates_ = 0;
}

void World::FlushSystemStateUpdates() noexcept {
    if (systemStateUpdates_.Size() == 0) {
        return;
    }

    HookScope hooks(*this);
    // hook이 다른 Component를 표시할 수 있어 크기를 매번 다시 읽는다(이번 지점에서 함께 처리한다).
    for (usize position = 0; position < systemStateUpdates_.Size(); ++position) {
        const ObjectHandle component = systemStateUpdates_[position];
        auto updated = With<Component>(component, [&](Component& value) noexcept {
            const uint8 dirty = value.dirtySystemStates_;
            value.dirtySystemStates_ = 0;
            if (!value.registered_ || value.state_ == ComponentState::PendingDestroy) {
                return;
            }

            for (uint32 index = 0; index < SystemStateKindCount; ++index) {
                const auto kind = static_cast<SystemStateKind>(index);
                if ((dirty & (1u << index)) == 0 || !value.HasSystemState(kind)) {
                    continue;
                }

                value.DestroySystemState(kind);
                value.systemStates_ = static_cast<uint8>(value.systemStates_ & ~(1u << index));
                if (auto created = value.CreateSystemState(kind); created) {
                    value.systemStates_ = static_cast<uint8>(value.systemStates_ | (1u << index));
                } else {
                    LogContextScope context("World", Name());
                    Log::Warning(LogWorld,
                                 "Component system state {Kind} could not be recreated: {Error} "
                                 "({Code})",
                                 SystemStateKindName(kind), ErrorCodeName(created.Error()),
                                 DiagSystemStateRecreateFailed);
                }
            }
        });
        (void)updated;
    }

    systemStateUpdates_.Clear();
}

Result<void> World::CheckComponentRules(ObjectHandle actor, ObjectHandle component) noexcept {
    struct Candidate {
        TypeId type;
        ComponentTypeRules rules;
        bool editorOnly = false;
    };

    auto read = With<Component>(component, [](Component& value) noexcept {
        Candidate candidate;
        candidate.type = value.GetType().Id();
        candidate.rules = value.TypeRules();
        candidate.editorOnly = value.IsEditorOnly();
        return candidate;
    });
    if (!read) {
        return Result<void>::Failure(read.Error());
    }

    const Candidate added = read.Value();
    // 편집 전용은 게임 World에 만들지 않는다(UE 게임 World에 EditorOnly Component가 없다).
    if (added.editorOnly && (Kind() == WorldKind::Game || Kind() == WorldKind::Tryout)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    const ComponentTypeRules& rules = added.rules;
    if (rules.required.Size() > MaxRequiredComponentTypes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const ActorRecord* record = FindActorRecord(actor);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    bool missing[MaxRequiredComponentTypes] = {};
    const usize requiredCount = rules.required.Size();
    for (usize index = 0; index < requiredCount; ++index) {
        missing[index] = true;
    }

    Result<void> verdict = Result<void>::Success();
    for (ObjectHandle existing : record->components) {
        if (existing == component || !verdict) {
            continue;
        }

        auto checked = With<Component>(existing, [&](Component& value) noexcept -> Result<void> {
            if (value.state_ == ComponentState::PendingDestroy) {
                return Result<void>::Success();
            }

            // unique는 같은 타입과 그 파생을 한 묶음으로 본다(Unity DisallowMultipleComponent —
            // 기존이 새 타입의 파생이거나 새 것이 기존 타입의 파생이면 겹친다). 기존 Component의
            // unique도 본다.
            const TypeLease type = value.GetType();
            const TypeId existingType = type.Id();
            const bool existingUnique = value.TypeRules().unique;
            if (rules.unique || existingUnique) {
                auto derived =
                    With<Component>(component, [existingType](Component& candidate) noexcept {
                        return candidate.GetType().IsA(existingType);
                    });
                if (type.IsA(added.type) || (derived && derived.Value())) {
                    return Result<void>::Failure(ErrorCode::AlreadyExists);
                }
            }

            for (usize index = 0; index < requiredCount; ++index) {
                if (type.IsA(rules.required[index])) {
                    missing[index] = false;
                }
            }

            for (const TypeId& forbidden : rules.excluded) {
                if (type.IsA(forbidden)) {
                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }
            }

            // 기존 Component의 excluded에 새 타입이 걸리는지는 새 객체의 타입으로 본다.
            const ComponentTypeRules other = value.TypeRules();
            for (const TypeId& forbidden : other.excluded) {
                auto hit = With<Component>(component, [&](Component& candidate) noexcept {
                    return candidate.GetType().IsA(forbidden);
                });
                if (hit && hit.Value()) {
                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }
            }

            return Result<void>::Success();
        });
        if (!checked) {
            verdict = Move(checked);
        }
    }

    if (!verdict) {
        return verdict;
    }

    for (usize index = 0; index < requiredCount; ++index) {
        if (missing[index]) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }
    }

    return Result<void>::Success();
}

} // namespace rocks
