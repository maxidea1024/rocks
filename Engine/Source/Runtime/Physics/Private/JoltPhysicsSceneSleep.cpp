// Jolt 장면의 잠듦·깨움 접점과 이벤트다(Docs/Design/PhysicsModule 2.7절·7.1.19절 — UE
// WakeInstance·PutInstanceToSleep, Chaos EWakeEventEntry·OnSyncBodies).
#include "JoltPhysicsScene.h"

#include "PhysicsLog.h"

#include <Core/Algorithms.h>

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyLock.h>
// clang-format on

namespace rocks::physics {

void JoltPhysicsScene::CollectWakeTransitions() noexcept {
    transitionData_.Clear();
    transitionAwake_.Clear();
    if (const uint64 dropped = activations_.TakeTransitionsInto(transitionData_, transitionAwake_);
        dropped != 0) {
        Log::Warning(LogPhysics,
                     "Physics scene lost {Count} body wake or sleep notices; those wake events "
                     "were not reported",
                     dropped);
    }

    for (usize index = 0; index < transitionData_.Size(); ++index) {
        const JPH::uint64 userData = transitionData_[index];
        const PhysicsBodyHandle handle{UserDataIndex(userData), UserDataGeneration(userData)};
        // 지운 body·재사용 칸(세대가 다름)·Kinematic은 이벤트가 없다(Chaos Kinematic은 잠들지
        // 않는다).
        if (FindSlot(handle) == nullptr) {
            continue;
        }

        BodySlot& slot = slots_[handle.index];
        if (slot.motion != BodyMotion::Dynamic) {
            continue;
        }

        // 마지막 전이가 이긴다(Chaos 입자마다 값 하나).
        slot.wakeEvent = transitionAwake_[index] ? WakeTransition::Awake : WakeTransition::Sleep;
        if (!slot.queuedWakeEvent) {
            if (wakeEventBodies_.TryPushBack(handle)) {
                slot.queuedWakeEvent = true;
            } else {
                slot.wakeEvent = WakeTransition::None;
            }
        }
    }
}

Span<const WakeEvent> JoltPhysicsScene::TakeWakeEvents() noexcept {
    AssertNotStepping();
    CollectWakeTransitions();
    wakeEvents_.Clear();
    if (!wakeEvents_.TryReserve(wakeEventBodies_.Size())) {
        return Span<const WakeEvent>();
    }

    algo::Sort(wakeEventBodies_.View(),
               [](const PhysicsBodyHandle& a, const PhysicsBodyHandle& b) noexcept {
                   return a.index < b.index;
               });
    for (const PhysicsBodyHandle& handle : wakeEventBodies_) {
        if (FindSlot(handle) == nullptr) {
            continue;
        }

        BodySlot& slot = slots_[handle.index];
        slot.queuedWakeEvent = false;
        if (slot.wakeEvent != WakeTransition::None && slot.motion == BodyMotion::Dynamic) {
            (void)wakeEvents_.TryPushBack(
                WakeEvent{slot.component, slot.wakeEvent == WakeTransition::Awake});
        }

        slot.wakeEvent = WakeTransition::None;
    }

    wakeEventBodies_.Clear();
    return wakeEvents_.View();
}

void JoltPhysicsScene::SetBodyAwake(PhysicsBodyHandle handle, bool awake) noexcept {
    AssertNotStepping();
    if (FindSlot(handle) == nullptr) {
        return;
    }

    BodySlot& slot = slots_[handle.index];
    // Dynamic에만 효력이 있다(UE IsDynamic && !IsKinematic — BodyInstance.cpp:3598-3614).
    if (slot.motion != BodyMotion::Dynamic) {
        return;
    }

    JPH::BodyInterface& bodies = system_.GetBodyInterface();
    if (awake) {
        // 이벤트 없이 깨우고 쌓인 이벤트를 지운다(UE WakeUp_AssumesLocked의 ClearEvents).
        WithoutWakeEvents(slot, true, [&]() noexcept { bodies.ActivateBody(slot.body); });
        return;
    }

    // 이벤트 없이 재운다(쌓인 이벤트는 둔다 — UE PutToSleep_AssumesLocked). Jolt 비활성화가
    // 속도를 0으로 둔다(BodyManager.cpp:565-567). 쌓인 충격량·힘은 버린다(Chaos가 dynamics 표지를
    // 지운다 — ParticleHandle.h:3762-3766).
    WithoutWakeEvents(slot, false, [&]() noexcept { bodies.DeactivateBody(slot.body); });
    DropPendingImpulse(slot);
    JPH::BodyLockWrite lock(system_.GetBodyLockInterface(), slot.body);
    if (lock.Succeeded() && lock.GetBody().IsDynamic()) {
        lock.GetBody().ResetForce();
        lock.GetBody().ResetTorque();
    }
}

} // namespace rocks::physics
