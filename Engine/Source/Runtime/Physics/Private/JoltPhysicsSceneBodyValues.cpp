// Jolt 장면의 실행 중 body 값 갱신이다(Docs/Design/PhysicsModule 7.1.18절 — UE FBodyInstance의 값
// setter: SetEnableGravity·UpdateDampingProperties·SetUseCCD·SetMaxAngularVelocityInRadians·
// SetGyroscopicTorqueEnabled·UpdateMassProperties·SetDOFLock).
#include "JoltPhysicsScene.h"

#include "JoltBodyBuild.h"
#include "JoltConversions.h"
#include "PhysicsLog.h"

#include <Core/Math/MathUtility.h>

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyLock.h>
// clang-format on

namespace rocks::physics {

void JoltPhysicsScene::RebuildMass(BodySlot& slot, const BodyMassValues& mass) noexcept {
    // Static은 운동 값이 없다(UE IsRigidBody 거짓 — 입력만 칸에 둔다).
    if (slot.motion == BodyMotion::Static) {
        slot.mass = mass;
        return;
    }

    // 질량 중심이 옮겨진 때만 감싸기를 바꾼다(형상 교체는 broadphase·접촉 cache를 새로 한다).
    // 깨우지 않는다(UE UpdateMassProperties). weld body는 부분별 질량을 다시 합친다(7.1.22절).
    slot.mass = mass;
    slot.parts[0].mass = mass;
    const BodyMass bodyMass =
        ComputeBodyMass(slot.inner, slot.elementShapes.View(), slot.parts.View(), slot.scale);
    const bool moved = bodyMass.shift != slot.massShift;
    if (moved) {
        system_.GetBodyInterface().SetShape(slot.body, bodyMass.shape.GetPtr(), false,
                                            JPH::EActivation::DontActivate);
    }

    slot.massShift = bodyMass.shift;
    slot.massProperties = bodyMass.properties;
    {
        JPH::BodyLockWrite lock(system_.GetBodyLockInterface(), slot.body);
        if (lock.Succeeded()) {
            lock.GetBody().GetMotionProperties()->SetMassProperties(
                slot.motion == BodyMotion::Dynamic ? slot.dof.allowed : JPH::EAllowedDOFs::All,
                slot.massProperties);
        }
    }

    // 질량 중심이 옮겨졌으면 구속은 다시 붙인다(7.1.16절 — Jolt 구속은 질량 중심 기준 anchor).
    if (moved && slot.dofConstraint != nullptr) {
        DetachDofConstraint(slot);
        ApplyDegreesOfFreedom(slot);
    }
}

void JoltPhysicsScene::ApplyGyroscopic(BodySlot& slot, bool requested) noexcept {
    JPH::BodyLockWrite lock(system_.GetBodyLockInterface(), slot.body);
    if (lock.Succeeded() && lock.GetBody().IsRigidBody()) {
        lock.GetBody().SetApplyGyroscopicForce(requested && !slot.dof.LocksRotation());
    }
}

void JoltPhysicsScene::UpdateBodySettings(PhysicsBodyHandle handle, const BodySettings& settings,
                                          BodySettingsChange change) noexcept {
    AssertNotStepping();
    if (FindSlot(handle) == nullptr) {
        return;
    }

    BodySlot& slot = slots_[handle.index];
    if (change == BodySettingsChange::Mass) {
        const BodyMassValues mass = MassValuesOf(&settings);
        if (AreMassValuesValid(mass)) {
            RebuildMass(slot, mass);
        }

        return;
    }

    // 나머지는 운동 값이다(Static은 없다).
    if (slot.motion == BodyMotion::Static) {
        if (change == BodySettingsChange::DegreesOfFreedom) {
            slot.dof = ResolveDofLock(&settings, defaultDegreesOfFreedom_);
        }

        return;
    }

    JPH::BodyInterface& bodies = system_.GetBodyInterface();
    switch (change) {
    case BodySettingsChange::Gravity:
        bodies.SetGravityFactor(slot.body, settings.enableGravity ? 1.0f : 0.0f);
        // 켜면 깨운다(UE SetEnableGravity → WakeInstance — Dynamic만). 이벤트는 없고 쌓인 것을
        // 지운다(WakeUp_AssumesLocked — 7.1.19절).
        if (settings.enableGravity && slot.motion == BodyMotion::Dynamic) {
            WithoutWakeEvents(slot, true, [&]() noexcept { bodies.ActivateBody(slot.body); });
        }

        break;
    case BodySettingsChange::Damping: {
        JPH::BodyLockWrite lock(system_.GetBodyLockInterface(), slot.body);
        if (lock.Succeeded() && lock.GetBody().IsRigidBody()) {
            JPH::MotionProperties& motion = *lock.GetBody().GetMotionProperties();
            motion.SetLinearDamping(settings.linearDamping);
            motion.SetAngularDamping(settings.angularDamping);
        }

        break;
    }

    case BodySettingsChange::ContinuousCollision:
        bodies.SetMotionQuality(slot.body, settings.continuousCollision
                                               ? JPH::EMotionQuality::LinearCast
                                               : JPH::EMotionQuality::Discrete);
        break;
    case BodySettingsChange::MaxAngularVelocity: {
        const double degrees = settings.overrideMaxAngularVelocity
                                   ? static_cast<double>(settings.maxAngularVelocity)
                                   : maxAngularVelocityDegrees_;
        JPH::BodyLockWrite lock(system_.GetBodyLockInterface(), slot.body);
        if (lock.Succeeded() && lock.GetBody().IsRigidBody()) {
            lock.GetBody().GetMotionProperties()->SetMaxAngularVelocity(
                static_cast<float>(math::DegreesToRadians(degrees)));
        }

        break;
    }

    case BodySettingsChange::GyroscopicTorque:
        ApplyGyroscopic(slot, settings.gyroscopicTorque);
        break;
    case BodySettingsChange::DegreesOfFreedom:
        // UE CreateDOFLock: 옛 구속을 끝내고 새로 만든다(BodyInstance.cpp:855-925).
        DetachDofConstraint(slot);
        slot.dof = ResolveDofLock(&settings, defaultDegreesOfFreedom_);
        ApplyDegreesOfFreedom(slot);
        ApplyGyroscopic(slot, settings.gyroscopicTorque);
        break;
    case BodySettingsChange::Mass:
        break;
    }
}

} // namespace rocks::physics
