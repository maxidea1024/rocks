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

        if (!ready.Value()) {
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

        StartComponent(component);
    }

    if (componentCount != 0) {
        auto removed = pendingComponentStart_.TryRemoveAt(0, componentCount);
        (void)removed;
    }
}

} // namespace rocks
