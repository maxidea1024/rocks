#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>

namespace rocks {

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
        // 꺼진 채 시작을 기다리던 Actor는 다시 시작 큐에 넣는다(ProcessStarts가 뺐다 — 용량은
        // Spawn에서 Actor마다 확보했다).
        if (record->startDeferred && record->state == ActorState::Initialized) {
            record->startDeferred = false;
            auto queued = pendingStart_.TryPushBack(actor);
            (void)queued;
        }

        auto tickEnabled =
            With<Actor>(actor, [](Actor& value) noexcept { return value.tickEnabled_; });
        SetTicksEnabled(record->ticks, tickEnabled && tickEnabled.Value());
        auto called = With<Actor>(actor, [](Actor& value) noexcept { value.OnEnable(); });
        (void)called;
        for (usize index = 0;; ++index) {
            const ComponentSlot slot = ComponentSlotAt(actor, index);
            if (index >= slot.count) {
                break;
            }

            const ObjectHandle component = slot.component;
            if (component.IsNull()) {
                continue;
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
        // 비활성화는 Component(역순) -> Actor 순이다.
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

} // namespace rocks
