#include <Object/ConstructionContext.h>
#include <World/World.h>

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
        value.state_ = ComponentState::PendingDestroy;
        UnregisterTicks(value.ticks_);
    });
}

} // namespace rocks
