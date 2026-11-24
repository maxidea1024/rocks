#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/SceneComponent.h>
#include <World/World.h>

namespace rocks {

// --- 파괴 ---

void World::ClearRootComponent(ObjectHandle actor, Actor& owner, ObjectHandle component) noexcept {
    if (component.IsNull() || owner.rootComponent_.Handle() != component) {
        return;
    }

    // null 대입은 대상 검증·장벽이 없고 Borrow를 쥔 Alive owner면 실패하지 않는다. owner가 이미
    // 파괴 요청됐으면 실패하지만 그 Actor도 회수되므로 root가 살릴 생존 경로가 없다.
    auto cleared = objects_.SetReference(Access(), actor, owner.rootComponent_, ObjectHandle{});
    (void)cleared;
}

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
            auto called = With<Component>(
                component, [](Component& value) noexcept { value.OnDestroy(value.endReason_); });
            (void)called;
        }
    }

    auto unregistering = With<Component>(component, [this](Component& value) noexcept {
        const bool registered = value.registered_;
        value.registered_ = false;
        UnregisterTicks(value.ticks_);
        // 시스템 상태는 OnUnregister 전에 역순으로 지운다(23.2절).
        if (registered) {
            DestroySystemStates(value);
        }

        value.dirtySystemStates_ = 0;
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

    // 대표였다면 비운다(강한 참조가 파괴된 객체의 저장소를 붙잡지 않게 — 좀비 방지). 소유 Actor도
    // 정리 중이면 목록 간선은 Actor 정리 끝에 한 번에 비운다(O(n) 회피).
    auto detached = With<Actor>(actor, [&](Actor& owner) noexcept {
        ClearRootComponent(actor, owner, component);
        if (ownerDestroyed) {
            return Result<void>::Success();
        }

        return objects_.RemoveReference(Access(), actor, owner.components_, component);
    });
    // 소유 Actor가 우회 파괴되어 Borrow로 읽을 수 없어도 정리 중(ownerDestroyed)이면 센다.
    if ((detached || ownerDestroyed) && componentCount_ != 0) {
        --componentCount_;
    }

    if (!ownerDestroyed) {
        if (ActorRecord* record = FindActorRecord(actor)) {
            const usize at = record->components.Find(component);
            if (at != Array<ObjectHandle>::NotFound) {
                auto removed = record->components.TryRemoveAt(at);
                (void)removed;
            }
        }
    }

    // Object::Destroy로 우회 파괴되어 접근할 수 없는 Component도 회수 요청은 그대로 한다. 그 전에
    // 색인에서 빼 복구가 World의 요청을 우회 파괴로 보지 않게 한다(사본 Tick 등록도 해제 — 우회
    // 파괴된 Component는 ticks_를 읽을 수 없다).
    ForgetComponent(component);
    RequestDestroyOwned(component);
    gcPending_ = true;
    destroyedDuringCycle_ = true;
}

void World::DestroyActorNow(ObjectHandle actor) noexcept {
    ActorRecord* record = FindActorRecord(actor);
    if (record == nullptr) {
        return;
    }

    const EndPlayReason reason = record->endReason;
    // 0) 프레임워크 EndPlay(UE Destroyed·EndPlay의 엔진 부분 — 17.8절). PreAwake가 성공한 Actor만.
    if (record->frameworkAwake) {
        record->frameworkAwake = false;
        auto ended = With<Actor>(
            actor, [reason](Actor& value) noexcept { value.OnFrameworkEndPlay(reason); });
        (void)ended;
        record = FindActorRecord(actor);
        if (record == nullptr) {
            return;
        }
    }

    // 1) 비활성화: Component(역순) -> Actor.
    if (record->enabledApplied) {
        EnableActor(actor, false);
    }

    // Component는 Actor 레코드의 목록으로 찾는다(Actor가 World API 밖에서 파괴돼 Borrow로 목록을
    // 읽을 수 없어도 빠짐없이 — 13.3절).
    auto recordedCount = [this, actor]() noexcept -> usize {
        const ActorRecord* found = FindActorRecord(actor);
        return found != nullptr ? found->components.Size() : 0;
    };
    auto recordedAt = [this, actor](usize index) noexcept -> ObjectHandle {
        const ActorRecord* found = FindActorRecord(actor);
        return found != nullptr && index < found->components.Size() ? found->components[index]
                                                                    : ObjectHandle{};
    };

    // 2) OnDestroy: Component(역순) -> Actor. OnAwake가 성공한 객체만 받는다.
    for (usize index = recordedCount(); index > 0; --index) {
        const ObjectHandle component = recordedAt(index - 1);
        if (component.IsNull()) {
            continue;
        }

        auto destroying = With<Component>(component, [](Component& value) noexcept {
            const bool awake = value.awake_;
            value.awake_ = false;
            return awake;
        });
        if (destroying && destroying.Value()) {
            // Actor와 함께 정리되면 Actor 이유가 Component에 적혀 있다(먼저 단독 파괴된 것은 그
            // 이유).
            auto called = With<Component>(
                component, [](Component& value) noexcept { value.OnDestroy(value.endReason_); });
            (void)called;
        }
    }

    record = FindActorRecord(actor);
    if (record != nullptr && record->awake) {
        record->awake = false;
        auto called =
            With<Actor>(actor, [reason](Actor& value) noexcept { value.OnDestroy(reason); });
        (void)called;
    }

    // 3) Component 등록 해제·회수 요청(역순). 파괴 대기 Actor에는 Component를 추가할 수 없으므로
    // 목록은
    //    더 늘지 않는다. 간선은 끝에 한 번에 비운다.
    for (usize index = recordedCount(); index > 0; --index) {
        const ObjectHandle component = recordedAt(index - 1);
        if (!component.IsNull()) {
            DestroyComponentNow(actor, component, true);
        }
    }

    if (ActorRecord* found = FindActorRecord(actor)) {
        found->components.Clear();
    }

    auto cleared = With<Actor>(actor, [&](Actor& owner) noexcept {
        ClearRootComponent(actor, owner, owner.rootComponent_.Handle());
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
    ForgetTickDeclarations(actor);
    const ObjectHandle level = record->level;
    LevelRecord* levelRecord = FindLevelRecord(level);
    if (levelRecord != nullptr) {
        if (!levelRecord->pendingDestroy && state_ != WorldState::TearingDown) {
            DetachFromLevel(level, *record);
        }

        if (levelRecord->actorCount != 0) {
            --levelRecord->actorCount;
        }
    }

    RequestDestroyOwned(actor);
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

            // World API 밖에서 파괴되어 접근할 수 없으면 Component 색인으로 소유 Actor를 찾아
            // 간선과 Tick 등록만 정리한다(Actor를 훑지 않는다). 색인에 없으면 이미 정리됐다(중복
            // 항목).
            const ComponentRecord* entry = componentRecords_.Find(component.slot);
            if (entry == nullptr || entry->component != component) {
                continue;
            }

            const ObjectHandle actor = entry->actor;
            const ActorRecord* ownerRecord = FindActorRecord(actor);
            if (ownerRecord != nullptr && ownerRecord->state == ActorState::PendingDestroy) {
                // 소유 Actor 정리가 레코드의 목록으로 함께 정리한다.
                continue;
            }

            ForgetComponent(component);
            // 대표였다면 함께 비운다(우회 파괴된 객체를 root가 붙잡아 좀비로 남지 않게).
            auto detached = With<Actor>(actor, [&](Actor& value) noexcept {
                ClearRootComponent(actor, value, component);
                return objects_.RemoveReference(Access(), actor, value.components_, component);
            });
            if (detached && componentCount_ != 0) {
                --componentCount_;
            }

            if (ActorRecord* owner = FindActorRecord(actor)) {
                const usize at = owner->components.Find(component);
                if (at != Array<ObjectHandle>::NotFound) {
                    auto removed = owner->components.TryRemoveAt(at);
                    (void)removed;
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

} // namespace rocks
