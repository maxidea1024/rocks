#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>

namespace rocks {

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
        // 용량은 AddComponent에서 Component 수만큼 확보했다. 준비 대기로 다시 넣으면 처리 중인 옛
        // 항목이 잠시 같이 있어 하나를 넘을 수 있으므로 늘린다(할당 실패만 남는다).
        auto grown = pendingComponentStart_.TryEnsureCapacity(pendingComponentStart_.Size() + 1);
        (void)grown;
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

    // 준비되지 않았으면 다음 프레임에 다시 본다(17.4절 — Actor 시작 중·OnStart 안에서 추가된 것
    // 포함).
    auto ready = With<Component>(component,
                                 [](Component& value) noexcept { return value.IsReadyToStart(); });
    if (ready && !ready.Value()) {
        QueueComponentStart(component);
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

bool World::ComponentsReadyToStart(ObjectHandle actor) noexcept {
    for (usize index = 0;; ++index) {
        const ComponentSlot slot = ComponentSlotAt(actor, index);
        if (index >= slot.count) {
            return true;
        }

        if (slot.component.IsNull()) {
            continue;
        }

        auto ready = With<Component>(slot.component, [](Component& value) noexcept {
            return value.state_ != ComponentState::Initialized || !value.enabledApplied_ ||
                   value.IsReadyToStart();
        });
        if (ready && !ready.Value()) {
            return false;
        }
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

        // 비활성 Actor는 활성화될 때까지 시작을 미룬다(Unity와 같은 의미). 큐에 남기지 않고
        // 활성화(EnableActor)가 다시 넣는다 — 오래 꺼진 Actor를 프레임마다 다시 훑지 않는다.
        if (!record->enabledApplied) {
            FindActorRecord(actor)->startDeferred = true;
            continue;
        }

        auto ready =
            With<Actor>(actor, [](Actor& value) noexcept { return value.IsReadyToStart(); });
        if (!ready) {
            continue;
        }

        // 켜진 Component도 모두 준비돼야 Actor를 시작한다(17.4절 — Actor 시작이 Component 시작을
        // 묶는다).
        if (!ready.Value() || !ComponentsReadyToStart(actor)) {
            pendingStart_[kept++] = actor;
            continue;
        }

        // Component(추가 순서) -> Actor 순으로 OnStart한다.
        for (usize componentIndex = 0;; ++componentIndex) {
            record = FindActorRecord(actor);
            if (record == nullptr || record->state != ActorState::Initialized) {
                break;
            }

            const ComponentSlot slot = ComponentSlotAt(actor, componentIndex);
            if (componentIndex >= slot.count) {
                break;
            }

            if (!slot.component.IsNull()) {
                StartComponent(slot.component);
            }
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
        // 초기 겹침(UE AActor::BeginPlay → UpdateInitialOverlaps — Actor.cpp:4801·2386): 이 Actor
        // 안에서 맨 위인 SceneComponent(부모가 없거나 부모가 다른 Actor 것)마다 갱신하고 자식으로
        // 내려간다(PhysicsModule 7.1.11절). UE는 루트 하나에서 시작하지만 Rocks Actor는 루트를
        // 자동으로 정하지 않아 맨 위 컴포넌트가 여럿일 수 있다. 알림이 목록을 바꾸거나 이 Actor를
        // 파괴할 수 있으므로 칸을 하나씩 다시 읽고 상태를 다시 본다.
        for (usize componentIndex = 0;; ++componentIndex) {
            record = FindActorRecord(actor);
            if (record == nullptr || record->state != ActorState::Playing) {
                break;
            }

            const ComponentSlot slot = ComponentSlotAt(actor, componentIndex);
            if (componentIndex >= slot.count) {
                break;
            }

            (void)With<SceneComponent>(
                slot.component, [this, actor](SceneComponent& scene) noexcept {
                    const ObjectHandle parent = scene.Parent();
                    auto parentOwner = With<Component>(
                        parent, [](Component& value) noexcept { return value.GetOwner(); });
                    if (parent.IsNull() || !parentOwner || parentOwner.Value() != actor) {
                        scene.UpdateOverlaps();
                    }
                });
        }

        updated = FindActorRecord(actor);
        if (updated == nullptr || updated->state != ActorState::Playing) {
            continue;
        }

        // 시작 전에 건 LifeSpan을 지금부터 센다.
        if (updated->pendingLifeSpan > 0.0) {
            const double seconds = updated->pendingLifeSpan;
            updated->pendingLifeSpan = 0.0;
            auto armed = SetActorLifeSpan(actor, seconds);
            (void)armed;
            updated = FindActorRecord(actor);
        }

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
            const ComponentSlot slot = ComponentSlotAt(actor, componentIndex);
            if (componentIndex >= slot.count) {
                break;
            }

            if (!slot.component.IsNull()) {
                StartComponent(slot.component);
                RegisterComponentTicks(slot.component);
            }
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

        // 준비되지 않았으면 StartComponent가 큐 뒤에 다시 넣는다(아래 앞 구간 제거에 걸리지
        // 않는다).
        StartComponent(component);
    }

    if (componentCount != 0) {
        auto removed = pendingComponentStart_.TryRemoveAt(0, componentCount);
        (void)removed;
    }
}

} // namespace rocks
