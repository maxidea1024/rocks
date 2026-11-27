// Jolt 장면의 힘·충격량·속도 접점이다(Docs/Design/PhysicsModule 2.6절·7.1.17절 — UE FBodyInstance
// Add…·SetLinearVelocity·SetAngularVelocityInRadians, Chaos ChaosEngineInterface.cpp:846-1110).
#include "JoltPhysicsScene.h"

#include "JoltConversions.h"
#include "PhysicsLog.h"

#include <Core/Math/MathUtility.h>

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyLock.h>
// clang-format on

namespace rocks::physics {

namespace {

// 방사 힘의 중심 일치 거리(cm)다: 이보다 가까우면 방향을 +X로 둔다(UE AddRadialForce — Chaos
// ChaosEngineInterface.cpp:991).
constexpr double RadialForceCoincidentDistance = 1.0e-4;

bool IsFiniteVector(const Vec3d& value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

bool IsFiniteForce(const BodyForce& force) noexcept {
    return IsFiniteVector(force.value) && IsFiniteVector(force.location) &&
           math::IsFinite(force.radius) && math::IsFinite(force.strength);
}

// Jolt float 값이 유한하고 제곱 길이도 유한한지다(Jolt 속도 상한 자르기가 제곱 길이를 assert한다 —
// MotionProperties.inl:28).
bool IsFiniteJolt(JPH::Vec3Arg value) noexcept {
    return math::IsFinite(value.GetX()) && math::IsFinite(value.GetY()) &&
           math::IsFinite(value.GetZ()) && math::IsFinite(value.LengthSq());
}

void WarnNonFinite(PhysicsBodyHandle body) noexcept {
    Log::Warning(LogPhysics,
                 "Physics body #{Slot} received a non-finite force or velocity ({Code})",
                 body.index, DiagNonFiniteBodyInput);
}

// 방사 충격량(UE AddRadialImpulse — ChaosEngineInterface.cpp:883-918): 질량 중심이 범위 안(엄격)
// 이면 참이고 impulse에 값을 쓴다. 중심과 일치하면 +X × strength다(감쇠와 상관없이).
bool RadialImpulseAt(const BodyForce& force, const Vec3d& centerOfMass, Vec3d& impulse) noexcept {
    const Vec3d offset = centerOfMass - force.location;
    const double distance = offset.Length();
    if (!(force.radius > 0.0) || !(distance < force.radius)) {
        return false;
    }

    if (!(distance > 0.0)) {
        impulse = Vec3d::UnitX() * force.strength;
        return true;
    }

    const Vec3d direction = offset * (1.0 / distance);
    const double scale =
        force.falloff == RadialFalloff::Linear ? (force.radius - distance) / force.radius : 1.0;
    impulse = direction * (force.strength * scale);
    return true;
}

// 방사 힘(UE AddRadialForce — ChaosEngineInterface.cpp:976-1018): 질량 중심이 범위 밖(경계 제외)이
// 아니면 참이다. 중심에서 1e-4 cm 안이면 방향은 +X다.
bool RadialForceAt(const BodyForce& force, const Vec3d& centerOfMass, Vec3d& out) noexcept {
    const Vec3d offset = centerOfMass - force.location;
    const double distance = offset.Length();
    if (!(force.radius > 0.0) || distance > force.radius) {
        return false;
    }

    const Vec3d direction =
        distance < RadialForceCoincidentDistance ? Vec3d::UnitX() : offset * (1.0 / distance);
    const double scale =
        force.falloff == RadialFalloff::Linear ? (force.radius - distance) / force.radius : 1.0;
    out = direction * (force.strength * scale);
    return true;
}

// body 하나에 줄 몫이다(Jolt 단위): 다음 step에 더할 속도 변화와 이번 step의 누적 힘·토크.
struct ForceEffect {
    bool impulse = false;
    JPH::Vec3 linearVelocity = JPH::Vec3::sZero();
    JPH::Vec3 angularVelocity = JPH::Vec3::sZero();
    bool force = false;
    JPH::Vec3 linearForce = JPH::Vec3::sZero();
    JPH::Vec3 torque = JPH::Vec3::sZero();
};

// 월드 관성 × 벡터다(Chaos GetWorldInertia — 칸 관성을 body 회전으로 돌린 R·I·Rᵀ).
JPH::Vec3 MultiplyWorldInertia(const JPH::MassProperties& mass, JPH::QuatArg rotation,
                               JPH::Vec3Arg vector) noexcept {
    const JPH::Mat44 turn = JPH::Mat44::sRotation(rotation);
    return turn.Multiply3x3(mass.mInertia.Multiply3x3(turn.Multiply3x3Transposed(vector)));
}

} // namespace

void JoltPhysicsScene::DropPendingImpulse(BodySlot& slot) noexcept {
    // 목록에 남은 칸은 다음 step·EndFrame이 표지를 보고 건너뛴다.
    slot.queuedImpulse = false;
    slot.pendingLinearVelocity = JPH::Vec3::sZero();
    slot.pendingAngularVelocity = JPH::Vec3::sZero();
    slot.queuedFrameForce = false;
    slot.frameForce = JPH::Vec3::sZero();
    slot.frameTorque = JPH::Vec3::sZero();
}

void JoltPhysicsScene::EndFrame() noexcept {
    for (uint32 index : frameForces_) {
        BodySlot& slot = slots_[index];
        slot.queuedFrameForce = false;
        slot.frameForce = JPH::Vec3::sZero();
        slot.frameTorque = JPH::Vec3::sZero();
    }

    frameForces_.Clear();
}

void JoltPhysicsScene::QueueImpulse(uint32 index, JPH::MotionProperties& motion,
                                    JPH::Vec3Arg linear, JPH::Vec3Arg angular) noexcept {
    BodySlot& slot = slots_[index];
    slot.pendingLinearVelocity += linear;
    slot.pendingAngularVelocity += angular;
    if (slot.queuedImpulse) {
        return;
    }

    if (pendingImpulses_.TryPushBack(index)) {
        slot.queuedImpulse = true;
        return;
    }

    // 목록을 늘리지 못하면 그 자리에서 더한다(할당 실패 한정 — 7.1.17절).
    motion.SetLinearVelocityClamped(motion.GetLinearVelocity() + slot.pendingLinearVelocity);
    motion.SetAngularVelocityClamped(motion.GetAngularVelocity() + slot.pendingAngularVelocity);
    DropPendingImpulse(slot);
}

void JoltPhysicsScene::ApplyPendingImpulses() noexcept {
    // step 직전 게임 thread 하나만 body를 만진다(Jolt job이 아직 돌지 않는다).
    const JPH::BodyLockInterface& locks = system_.GetBodyLockInterfaceNoLock();
    for (uint32 index : pendingImpulses_) {
        BodySlot& slot = slots_[index];
        if (!slot.queuedImpulse) {
            continue;
        }

        if (slot.live && slot.motion == BodyMotion::Dynamic) {
            JPH::BodyLockWrite lock(locks, slot.body);
            if (lock.Succeeded() && lock.GetBody().IsDynamic()) {
                JPH::MotionProperties& motion = *lock.GetBody().GetMotionProperties();
                motion.SetLinearVelocityClamped(motion.GetLinearVelocity() +
                                                slot.pendingLinearVelocity);
                motion.SetAngularVelocityClamped(motion.GetAngularVelocity() +
                                                 slot.pendingAngularVelocity);
            }
        }

        slot.queuedImpulse = false;
        slot.pendingLinearVelocity = JPH::Vec3::sZero();
        slot.pendingAngularVelocity = JPH::Vec3::sZero();
    }

    pendingImpulses_.Clear();
    // 프레임 힘은 스텝마다 다시 넣는다(Jolt 누적 힘은 Update 한 번 뒤 지워진다 —
    // PhysicsSystem.cpp:2506). 목록은 EndFrame이 비운다.
    for (uint32 index : frameForces_) {
        BodySlot& slot = slots_[index];
        if (!slot.queuedFrameForce || !slot.live || slot.motion != BodyMotion::Dynamic) {
            continue;
        }

        JPH::BodyLockWrite lock(locks, slot.body);
        if (lock.Succeeded() && lock.GetBody().IsDynamic()) {
            lock.GetBody().AddForce(slot.frameForce);
            lock.GetBody().AddTorque(slot.frameTorque);
        }
    }
}

void JoltPhysicsScene::AddStepForce(uint32 index, JPH::Body& body, JPH::Vec3Arg force,
                                    JPH::Vec3Arg torque, bool everyFixedStep) noexcept {
    BodySlot& slot = slots_[index];
    // 프레임 힘은 칸에 쌓아 step마다 넣는다(step하지 않는 장면은 다음 step이 없으므로 그 자리 —
    // 할당 실패도 그 자리에서 한 번).
    if (everyFixedStep && simulates_) {
        if (slot.queuedFrameForce || frameForces_.TryPushBack(index)) {
            slot.queuedFrameForce = true;
            slot.frameForce += force;
            slot.frameTorque += torque;
            return;
        }
    }

    body.AddForce(force);
    body.AddTorque(torque);
}

void JoltPhysicsScene::ApplyBodyForce(PhysicsBodyHandle handle, const BodyForce& force) noexcept {
    AssertNotStepping();
    if (FindSlot(handle) == nullptr) {
        return;
    }

    BodySlot& slot = slots_[handle.index];
    // Dynamic에만 효력이 있다(UE IsBodyDynamic — Static·Kinematic은 무시).
    if (slot.motion != BodyMotion::Dynamic) {
        return;
    }

    if (!IsFiniteForce(force)) {
        WarnNonFinite(handle);
        return;
    }

    bool applied = false;
    bool invalid = false;
    {
        JPH::BodyLockWrite lock(system_.GetBodyLockInterface(), slot.body);
        if (!lock.Succeeded() || !lock.GetBody().IsDynamic()) {
            return;
        }

        JPH::Body& body = lock.GetBody();
        JPH::MotionProperties& motion = *body.GetMotionProperties();
        const Vec3d centerOfMass = FromJoltPosition(body.GetCenterOfMassPosition(), origin_);
        const double mass = static_cast<double>(slot.massProperties.mMass);
        const float inverseMass = motion.GetInverseMass();
        const JPH::Quat rotation = body.GetRotation();
        // 위치 충격량의 속도 변화다(선형 J/m, 각 I⁻¹·((위치 − 질량 중심) × J) — Chaos
        // AddImpulseAtLocation).
        const auto impulseAt = [&](const Vec3d& impulse, const Vec3d& location, ForceEffect& out) {
            out.impulse = true;
            out.linearVelocity = ToJoltLinearImpulse(impulse) * inverseMass;
            out.angularVelocity = motion.MultiplyWorldSpaceInverseInertiaByVector(
                rotation, ToJoltAngularImpulse(Cross(location - centerOfMass, impulse)));
        };
        const auto impulseAtCenter = [&](const Vec3d& value, ForceEffect& out) {
            out.impulse = true;
            out.linearVelocity = force.massIndependent ? ToJoltLinearVelocity(value)
                                                       : ToJoltLinearImpulse(value) * inverseMass;
        };
        const auto forceAt = [&](const Vec3d& value, const Vec3d& location, ForceEffect& out) {
            out.force = true;
            out.linearForce = ToJoltForce(value);
            out.torque = ToJoltTorque(Cross(location - centerOfMass, value));
        };

        ForceEffect effect;
        switch (force.kind) {
        case BodyForceKind::Impulse:
            impulseAtCenter(force.value, effect);
            break;
        case BodyForceKind::AngularImpulse:
            effect.impulse = true;
            effect.angularVelocity = force.massIndependent
                                         ? ToJoltAngularVelocity(force.value)
                                         : motion.MultiplyWorldSpaceInverseInertiaByVector(
                                               rotation, ToJoltAngularImpulse(force.value));
            break;
        case BodyForceKind::ImpulseAtLocation:
            // 질량 무관이면 J = m·Δv다(UE AddVelocityChangeImpulseAtLocation).
            impulseAt(force.massIndependent ? force.value * mass : force.value, force.location,
                      effect);
            break;
        case BodyForceKind::RadialImpulse: {
            Vec3d impulse;
            if (RadialImpulseAt(force, centerOfMass, impulse)) {
                impulseAtCenter(impulse, effect);
            }

            break;
        }

        case BodyForceKind::Force:
            effect.force = true;
            effect.linearForce =
                ToJoltForce(force.massIndependent ? force.value * mass : force.value);
            break;
        case BodyForceKind::ForceAtLocation:
            forceAt(force.value, force.location, effect);
            break;
        case BodyForceKind::ForceAtLocationLocal: {
            // body 원점 transform으로 월드로 바꾼다(질량 중심 아님 — Chaos GetActorWorldTransform).
            const Quatd turn = FromJoltRotation(rotation);
            const Vec3d bodyOrigin = FromJoltPosition(body.GetPosition(), origin_);
            forceAt(turn.RotateVector(force.value), bodyOrigin + turn.RotateVector(force.location),
                    effect);
            break;
        }

        case BodyForceKind::Torque:
            effect.force = true;
            effect.torque = force.massIndependent
                                ? MultiplyWorldInertia(slot.massProperties, rotation,
                                                       ToJoltAngularVelocity(force.value))
                                : ToJoltTorque(force.value);
            break;
        case BodyForceKind::RadialForce: {
            Vec3d radial;
            if (RadialForceAt(force, centerOfMass, radial)) {
                effect.force = true;
                effect.linearForce = ToJoltForce(force.massIndependent ? radial * mass : radial);
            }

            break;
        }
        }

        // float로 바꾼 값도 유한해야 한다(큰 double은 Jolt float에서 inf — 7.1.17절).
        if (!IsFiniteJolt(effect.linearVelocity) || !IsFiniteJolt(effect.angularVelocity) ||
            !IsFiniteJolt(effect.linearForce) || !IsFiniteJolt(effect.torque)) {
            invalid = true;
        } else {
            if (effect.impulse) {
                QueueImpulse(handle.index, motion, effect.linearVelocity, effect.angularVelocity);
            }

            if (effect.force) {
                AddStepForce(handle.index, body, effect.linearForce, effect.torque,
                             force.everyFixedStep);
            }
        }

        applied = !invalid && (effect.impulse || effect.force);
    }

    if (invalid) {
        WarnNonFinite(handle);
        return;
    }

    // 효력이 있으면 깨운다(Chaos 외부 handle이 값과 상관없이 Dynamic 상태로 바꾼다).
    if (applied) {
        system_.GetBodyInterface().ActivateBody(slot.body);
    }
}

void JoltPhysicsScene::SetBodyLinearVelocity(PhysicsBodyHandle handle, const Vec3d& velocity,
                                             bool addToCurrent) noexcept {
    AssertNotStepping();
    if (FindSlot(handle) == nullptr) {
        return;
    }

    BodySlot& slot = slots_[handle.index];
    // Kinematic은 무시한다(Jolt Kinematic은 속도로 움직인다 — UE Chaos Kinematic은 목표 없이 속도로
    // 움직이지 않는다, 7.1.17절).
    if (slot.motion != BodyMotion::Dynamic) {
        return;
    }

    if (!IsFiniteVector(velocity)) {
        WarnNonFinite(handle);
        return;
    }

    bool wake = false;
    bool invalid = false;
    {
        JPH::BodyLockWrite lock(system_.GetBodyLockInterface(), slot.body);
        if (!lock.Succeeded() || !lock.GetBody().IsDynamic()) {
            return;
        }

        JPH::MotionProperties& motion = *lock.GetBody().GetMotionProperties();
        const Vec3d target =
            addToCurrent ? FromJoltLinearVelocity(motion.GetLinearVelocity()) + velocity : velocity;
        const JPH::Vec3 joltTarget = ToJoltLinearVelocity(target);
        if (!IsFiniteJolt(joltTarget)) {
            invalid = true;
        } else {
            motion.SetLinearVelocityClamped(joltTarget);
        }

        // 잠든 body는 거의 0이 아닌 속도에 깨운다(UE SetVBase — IsNearlyZero 성분 기준).
        wake = !invalid && !lock.GetBody().IsActive() &&
               !target.IsNearlyZero(math::KindaSmallNumber<double>);
    }

    if (invalid) {
        WarnNonFinite(handle);
        return;
    }

    if (wake) {
        system_.GetBodyInterface().ActivateBody(slot.body);
    }
}

void JoltPhysicsScene::SetBodyAngularVelocity(PhysicsBodyHandle handle,
                                              const Vec3d& velocityRadians,
                                              bool addToCurrent) noexcept {
    AssertNotStepping();
    if (FindSlot(handle) == nullptr) {
        return;
    }

    BodySlot& slot = slots_[handle.index];
    if (slot.motion != BodyMotion::Dynamic) {
        return;
    }

    if (!IsFiniteVector(velocityRadians)) {
        WarnNonFinite(handle);
        return;
    }

    bool wake = false;
    bool invalid = false;
    {
        JPH::BodyLockWrite lock(system_.GetBodyLockInterface(), slot.body);
        if (!lock.Succeeded() || !lock.GetBody().IsDynamic()) {
            return;
        }

        JPH::MotionProperties& motion = *lock.GetBody().GetMotionProperties();
        const Vec3d target =
            addToCurrent ? FromJoltAngularVelocity(motion.GetAngularVelocity()) + velocityRadians
                         : velocityRadians;
        const JPH::Vec3 joltTarget = ToJoltAngularVelocity(target);
        if (!IsFiniteJolt(joltTarget)) {
            invalid = true;
        } else {
            motion.SetAngularVelocityClamped(joltTarget);
        }

        wake = !invalid && !lock.GetBody().IsActive() &&
               !target.IsNearlyZero(math::KindaSmallNumber<double>);
    }

    if (invalid) {
        WarnNonFinite(handle);
        return;
    }

    if (wake) {
        system_.GetBodyInterface().ActivateBody(slot.body);
    }
}

} // namespace rocks::physics
