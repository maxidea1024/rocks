#pragma once

#include <Base/Types.h>

namespace rocks {

// 실행 중 바뀐 body 값 묶음이다(PhysicsScene::UpdateBodySettings —
// Docs/Design/PhysicsModule 7.1.18절).
// - Gravity: enableGravity(켜면 Dynamic body를 깨운다 — UE SetEnableGravity).
// - Damping: linearDamping·angularDamping(UE UpdateDampingProperties).
// - ContinuousCollision: continuousCollision(UE SetUseCCD).
// - MaxAngularVelocity: overrideMaxAngularVelocity·maxAngularVelocity(UE
//   SetMaxAngularVelocityInRadians).
// - GyroscopicTorque: gyroscopicTorque(UE SetGyroscopicTorqueEnabled).
// - Mass: massScale·overrideMass·massInKgOverride·centerOfMassOffset·inertiaTensorScale(UE
//   UpdateMassProperties — 깨우지 않는다).
// - DegreesOfFreedom: dofMode와 잠금 표지(UE SetDOFLock).
enum class BodySettingsChange : uint8 {
    Gravity,
    Damping,
    ContinuousCollision,
    MaxAngularVelocity,
    GyroscopicTorque,
    Mass,
    DegreesOfFreedom
};

} // namespace rocks
