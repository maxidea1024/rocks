// PrimitiveComponent의 힘·충격량·속도 API다(Docs/Design/PhysicsModule 2.6절·7.1.17절 — UE
// PrimitiveComponentPhysics.cpp:146-490).
#include <Base/Compiler.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogContext.h>
#include <Core/Math/MathUtility.h>
#include <World/Physics/PhysicsScene.h>
#include <World/Primitives/PrimitiveComponent.h>
#include <World/World.h>
#include <World/WorldLog.h>

namespace rocks {

namespace {

Vec3d DegreesToRadians(const Vec3d& degrees) noexcept {
    return Vec3d(math::DegreesToRadians(degrees.x), math::DegreesToRadians(degrees.y),
                 math::DegreesToRadians(degrees.z));
}

Vec3d RadiansToDegrees(const Vec3d& radians) noexcept {
    return Vec3d(math::RadiansToDegrees(radians.x), math::RadiansToDegrees(radians.y),
                 math::RadiansToDegrees(radians.z));
}

} // namespace

void PrimitiveComponent::WarnInvalidPhysicsOperation(const char* action,
                                                     bool checkBody) const noexcept {
    // 모든 물리 호출은 Movable이어야 한다(UE CheckStaticMobilityAndWarn — 경고는 CheckMovable).
    if (!CheckMovable(action)) {
        return;
    }

#if !ROCKS_BUILD_RELEASE
    if (!checkBody || !HasWorld()) {
        return;
    }

    if (!bodySettings_.simulatePhysics) {
        LogContextScope context("World", GetWorld().Name());
        Log::Warning(LogWorld,
                     "Primitive component #{Slot} must simulate physics to {Action} ({Code})",
                     Handle().slot, StringView(action), DiagPhysicsOperationWithoutSimulation);
        return;
    }

    auto resolved = TryResolveCollision();
    if (resolved && (resolved.Value().collisionEnabled == CollisionEnabled::NoCollision ||
                     resolved.Value().collisionEnabled == CollisionEnabled::QueryOnly)) {
        LogContextScope context("World", GetWorld().Name());
        Log::Warning(
            LogWorld, "Primitive component #{Slot} needs physics collision to {Action} ({Code})",
            Handle().slot, StringView(action), DiagPhysicsOperationWithoutPhysicsCollision);
    }
#endif
}

void PrimitiveComponent::ApplyBodyForce(const BodyForce& force) noexcept {
    if (bodyHandle_.IsNull()) {
        return;
    }

    World& world = GetWorld();
    if (PhysicsScene* scene = world.GetPhysicsScene(); scene != nullptr) {
        // 고정 스텝 밖의 힘은 그 프레임의 모든 고정 스텝에 준다(UE AddForce의 bAllowSubstepping 참
        // — 7.1.17절). Fixed hook 안이면 그 스텝에만 준다.
        BodyForce command = force;
        command.everyFixedStep = !world.InFixedSteps();
        scene->ApplyBodyForce(bodyHandle_, command);
    }
}

void PrimitiveComponent::AddImpulse(const Vec3d& impulse, bool velocityChange) noexcept {
    WarnInvalidPhysicsOperation("AddImpulse", true);
    BodyForce force;
    force.kind = BodyForceKind::Impulse;
    force.value = impulse;
    force.massIndependent = velocityChange;
    ApplyBodyForce(force);
}

void PrimitiveComponent::AddAngularImpulseInRadians(const Vec3d& impulse,
                                                    bool velocityChange) noexcept {
    WarnInvalidPhysicsOperation("AddAngularImpulse", true);
    BodyForce force;
    force.kind = BodyForceKind::AngularImpulse;
    force.value = impulse;
    force.massIndependent = velocityChange;
    ApplyBodyForce(force);
}

void PrimitiveComponent::AddAngularImpulseInDegrees(const Vec3d& impulse,
                                                    bool velocityChange) noexcept {
    AddAngularImpulseInRadians(DegreesToRadians(impulse), velocityChange);
}

void PrimitiveComponent::AddImpulseAtLocation(const Vec3d& impulse,
                                              const Vec3d& location) noexcept {
    WarnInvalidPhysicsOperation("AddImpulseAtLocation", true);
    BodyForce force;
    force.kind = BodyForceKind::ImpulseAtLocation;
    force.value = impulse;
    force.location = location;
    ApplyBodyForce(force);
}

void PrimitiveComponent::AddVelocityChangeImpulseAtLocation(const Vec3d& velocityChange,
                                                            const Vec3d& location) noexcept {
    // UE도 같은 동작 이름으로 경고한다(PrimitiveComponentPhysics.cpp:251).
    WarnInvalidPhysicsOperation("AddImpulseAtLocation", true);
    BodyForce force;
    force.kind = BodyForceKind::ImpulseAtLocation;
    force.value = velocityChange;
    force.location = location;
    force.massIndependent = true;
    ApplyBodyForce(force);
}

void PrimitiveComponent::AddRadialImpulse(const Vec3d& origin, double radius, double strength,
                                          RadialFalloff falloff, bool velocityChange) noexcept {
    if (ignoreRadialImpulse_) {
        return;
    }

    BodyForce force;
    force.kind = BodyForceKind::RadialImpulse;
    force.location = origin;
    force.radius = radius;
    force.strength = strength;
    force.falloff = falloff;
    force.massIndependent = velocityChange;
    ApplyBodyForce(force);
}

void PrimitiveComponent::AddForce(const Vec3d& force, bool accelerationChange) noexcept {
    WarnInvalidPhysicsOperation("AddForce", true);
    BodyForce command;
    command.kind = BodyForceKind::Force;
    command.value = force;
    command.massIndependent = accelerationChange;
    ApplyBodyForce(command);
}

void PrimitiveComponent::AddForceAtLocation(const Vec3d& force, const Vec3d& location) noexcept {
    WarnInvalidPhysicsOperation("AddForceAtLocation", true);
    BodyForce command;
    command.kind = BodyForceKind::ForceAtLocation;
    command.value = force;
    command.location = location;
    ApplyBodyForce(command);
}

void PrimitiveComponent::AddForceAtLocationLocal(const Vec3d& force,
                                                 const Vec3d& location) noexcept {
    WarnInvalidPhysicsOperation("AddForceAtLocationLocal", true);
    BodyForce command;
    command.kind = BodyForceKind::ForceAtLocationLocal;
    command.value = force;
    command.location = location;
    ApplyBodyForce(command);
}

void PrimitiveComponent::AddRadialForce(const Vec3d& origin, double radius, double strength,
                                        RadialFalloff falloff, bool accelerationChange) noexcept {
    if (ignoreRadialForce_) {
        return;
    }

    BodyForce command;
    command.kind = BodyForceKind::RadialForce;
    command.location = origin;
    command.radius = radius;
    command.strength = strength;
    command.falloff = falloff;
    command.massIndependent = accelerationChange;
    ApplyBodyForce(command);
}

void PrimitiveComponent::AddTorqueInRadians(const Vec3d& torque, bool accelerationChange) noexcept {
    WarnInvalidPhysicsOperation("AddTorque", true);
    BodyForce command;
    command.kind = BodyForceKind::Torque;
    command.value = torque;
    command.massIndependent = accelerationChange;
    ApplyBodyForce(command);
}

void PrimitiveComponent::AddTorqueInDegrees(const Vec3d& torque, bool accelerationChange) noexcept {
    AddTorqueInRadians(DegreesToRadians(torque), accelerationChange);
}

void PrimitiveComponent::SetPhysicsLinearVelocity(const Vec3d& velocity,
                                                  bool addToCurrent) noexcept {
    WarnInvalidPhysicsOperation("SetPhysicsLinearVelocity", false);
    if (bodyHandle_.IsNull()) {
        return;
    }

    if (PhysicsScene* scene = GetWorld().GetPhysicsScene(); scene != nullptr) {
        scene->SetBodyLinearVelocity(bodyHandle_, velocity, addToCurrent);
    }
}

Vec3d PrimitiveComponent::GetPhysicsLinearVelocity() const noexcept {
    if (bodyHandle_.IsNull()) {
        return Vec3d::Zero();
    }

    PhysicsScene* scene = GetWorld().GetPhysicsScene();
    auto read = scene != nullptr ? scene->ReadKinematics(bodyHandle_)
                                 : Result<BodyKinematics>::Failure(ErrorCode::StaleHandle);
    return read ? read.Value().linearVelocity : Vec3d::Zero();
}

Vec3d PrimitiveComponent::GetPhysicsLinearVelocityAtPoint(const Vec3d& point) const noexcept {
    if (bodyHandle_.IsNull()) {
        return Vec3d::Zero();
    }

    PhysicsScene* scene = GetWorld().GetPhysicsScene();
    auto read = scene != nullptr ? scene->ReadKinematics(bodyHandle_)
                                 : Result<BodyKinematics>::Failure(ErrorCode::StaleHandle);
    if (!read) {
        return Vec3d::Zero();
    }

    const BodyKinematics& kinematics = read.Value();
    return kinematics.linearVelocity +
           Cross(kinematics.angularVelocityRadians, point - kinematics.centerOfMass);
}

void PrimitiveComponent::SetPhysicsAngularVelocityInRadians(const Vec3d& velocity,
                                                            bool addToCurrent) noexcept {
    WarnInvalidPhysicsOperation("SetPhysicsAngularVelocity", false);
    if (bodyHandle_.IsNull()) {
        return;
    }

    if (PhysicsScene* scene = GetWorld().GetPhysicsScene(); scene != nullptr) {
        scene->SetBodyAngularVelocity(bodyHandle_, velocity, addToCurrent);
    }
}

void PrimitiveComponent::SetPhysicsAngularVelocityInDegrees(const Vec3d& velocity,
                                                            bool addToCurrent) noexcept {
    SetPhysicsAngularVelocityInRadians(DegreesToRadians(velocity), addToCurrent);
}

Vec3d PrimitiveComponent::GetPhysicsAngularVelocityInRadians() const noexcept {
    if (bodyHandle_.IsNull()) {
        return Vec3d::Zero();
    }

    PhysicsScene* scene = GetWorld().GetPhysicsScene();
    auto read = scene != nullptr ? scene->ReadKinematics(bodyHandle_)
                                 : Result<BodyKinematics>::Failure(ErrorCode::StaleHandle);
    return read ? read.Value().angularVelocityRadians : Vec3d::Zero();
}

Vec3d PrimitiveComponent::GetPhysicsAngularVelocityInDegrees() const noexcept {
    return RadiansToDegrees(GetPhysicsAngularVelocityInRadians());
}

} // namespace rocks
