// PrimitiveComponent의 힘·충격량·속도 API다(Docs/Design/PhysicsModule 2.6절·7.1.17절 — UE
// PrimitiveComponentPhysics.cpp:146-490).
#include <Base/Compiler.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogContext.h>
#include <Core/Math/MathUtility.h>
#include <World/Physics/BodyKinematics.h>
#include <World/Component.generated.h>
#include <World/Physics/PhysicsScene.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/SceneComponent.generated.h>
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

template <class Call>
bool PrimitiveComponent::ForwardToWeldRoot(Call&& call) const noexcept {
    if (!IsWelded()) {
        return false;
    }

    // 루트를 빌리지 못하면(파괴 중) 아무것도 하지 않는다 — weld 상태는 루트 상태 파괴가 먼저 푼다.
    (void)GetWorld().With<PrimitiveComponent>(
        weldRoot_, [&](PrimitiveComponent& root) noexcept { call(root); });
    return true;
}

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

    // body 검사는 weld면 루트 것이고 경고는 부른 컴포넌트로 남긴다(UE
    // WarnInvalidPhysicsOperations_Internal — GetBodyInstance(BoneName), 7.1.23절).
    bool simulating = bodySettings_.simulatePhysics;
    auto resolved = TryResolveCollision();
    (void)ForwardToWeldRoot([&](PrimitiveComponent& root) noexcept {
        simulating = root.bodySettings_.simulatePhysics;
        resolved = root.TryResolveCollision();
    });
    if (!simulating) {
        LogContextScope context("World", GetWorld().Name());
        Log::Warning(LogWorld,
                     "Primitive component #{Slot} must simulate physics to {Action} ({Code})",
                     Handle().slot, StringView(action), DiagPhysicsOperationWithoutSimulation);
        return;
    }

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
    if (PhysicsBody().IsNull()) {
        return;
    }

    World& world = GetWorld();
    if (PhysicsScene* scene = world.GetPhysicsScene(); scene != nullptr) {
        // 고정 스텝 밖의 힘은 그 프레임의 모든 고정 스텝에 준다(UE AddForce의 bAllowSubstepping 참
        // — 7.1.17절). Fixed hook 안이면 그 스텝에만 준다.
        BodyForce command = force;
        command.everyFixedStep = !world.InFixedSteps();
        scene->ApplyBodyForce(PhysicsBody(), command);
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
    // weld면 루트 body에 한다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    if (ForwardToWeldRoot([&](PrimitiveComponent& root) noexcept {
            root.SetPhysicsLinearVelocity(velocity, addToCurrent);
        })) {
        return;
    }

    WarnInvalidPhysicsOperation("SetPhysicsLinearVelocity", false);
    if (bodyHandle_.IsNull()) {
        return;
    }

    if (PhysicsScene* scene = GetWorld().GetPhysicsScene(); scene != nullptr) {
        scene->SetBodyLinearVelocity(bodyHandle_, velocity, addToCurrent);
    }
}

Vec3d PrimitiveComponent::GetPhysicsLinearVelocity() const noexcept {
    // weld면 루트 body 값이다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    Vec3d forwarded = Vec3d::Zero();
    if (ForwardToWeldRoot([&](PrimitiveComponent& root) noexcept {
            forwarded = root.GetPhysicsLinearVelocity();
        })) {
        return forwarded;
    }

    if (bodyHandle_.IsNull()) {
        return Vec3d::Zero();
    }

    PhysicsScene* scene = GetWorld().GetPhysicsScene();
    auto read = scene != nullptr ? scene->ReadKinematics(bodyHandle_)
                                 : Result<BodyKinematics>::Failure(ErrorCode::StaleHandle);
    return read ? read.Value().linearVelocity : Vec3d::Zero();
}

Vec3d PrimitiveComponent::GetPhysicsLinearVelocityAtPoint(const Vec3d& point) const noexcept {
    // weld면 루트 body 값이다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    Vec3d forwarded = Vec3d::Zero();
    if (ForwardToWeldRoot([&](PrimitiveComponent& root) noexcept {
            forwarded = root.GetPhysicsLinearVelocityAtPoint(point);
        })) {
        return forwarded;
    }

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
    // weld면 루트 body에 한다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    if (ForwardToWeldRoot([&](PrimitiveComponent& root) noexcept {
            root.SetPhysicsAngularVelocityInRadians(velocity, addToCurrent);
        })) {
        return;
    }

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
    // weld면 루트 body 값이다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    Vec3d forwarded = Vec3d::Zero();
    if (ForwardToWeldRoot([&](PrimitiveComponent& root) noexcept {
            forwarded = root.GetPhysicsAngularVelocityInRadians();
        })) {
        return forwarded;
    }

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

void PrimitiveComponent::WakeRigidBody() noexcept {
    // weld면 루트 body에 한다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    if (ForwardToWeldRoot([&](PrimitiveComponent& root) noexcept { root.WakeRigidBody(); })) {
        return;
    }

    if (bodyHandle_.IsNull()) {
        return;
    }

    if (PhysicsScene* scene = GetWorld().GetPhysicsScene(); scene != nullptr) {
        scene->SetBodyAwake(bodyHandle_, true);
    }
}

void PrimitiveComponent::PutRigidBodyToSleep() noexcept {
    // weld면 루트 body에 한다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    if (ForwardToWeldRoot([&](PrimitiveComponent& root) noexcept { root.PutRigidBodyToSleep(); })) {
        return;
    }

    if (bodyHandle_.IsNull()) {
        return;
    }

    if (PhysicsScene* scene = GetWorld().GetPhysicsScene(); scene != nullptr) {
        scene->SetBodyAwake(bodyHandle_, false);
    }
}

bool PrimitiveComponent::RigidBodyIsAwake() const noexcept {
    // weld면 루트 body 값이다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    bool forwarded = false;
    if (ForwardToWeldRoot(
            [&](PrimitiveComponent& root) noexcept { forwarded = root.RigidBodyIsAwake(); })) {
        return forwarded;
    }

    if (PhysicsBodyMotion() != BodyMotion::Dynamic) {
        return false;
    }

    BodyKinematics kinematics;
    return ReadBodyKinematics(kinematics) && kinematics.awake;
}

void PrimitiveComponent::PushBodySettings(BodySettingsChange change) noexcept {
    if (bodyHandle_.IsNull()) {
        return;
    }

    if (PhysicsScene* scene = GetWorld().GetPhysicsScene(); scene != nullptr) {
        scene->UpdateBodySettings(bodyHandle_, bodySettings_, change);
    }
}

bool PrimitiveComponent::ReadBodyKinematics(BodyKinematics& out) const noexcept {
    if (bodyHandle_.IsNull()) {
        return false;
    }

    PhysicsScene* scene = GetWorld().GetPhysicsScene();
    if (scene == nullptr) {
        return false;
    }

    auto read = scene->ReadKinematics(bodyHandle_);
    if (!read) {
        return false;
    }

    out = read.Value();
    return true;
}

void PrimitiveComponent::WarnInvalidBodyValue(const char* property) const noexcept {
    const auto emit = [&]() noexcept {
        Log::Warning(LogWorld,
                     "Primitive component #{Slot} ignored an invalid {Property} value ({Code})",
                     Handle().slot, StringView(property), DiagInvalidBodyValue);
    };
    if (!HasWorld()) {
        emit();
        return;
    }

    LogContextScope context("World", GetWorld().Name());
    emit();
}

float PrimitiveComponent::GetMass() const noexcept {
    // weld면 루트 body 값이다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    float forwarded = 0.0f;
    if (ForwardToWeldRoot([&](PrimitiveComponent& root) noexcept { forwarded = root.GetMass(); })) {
        return forwarded;
    }

    WarnInvalidPhysicsOperation("GetMass", true);
    BodyKinematics kinematics;
    return ReadBodyKinematics(kinematics) ? static_cast<float>(kinematics.mass) : 0.0f;
}

void PrimitiveComponent::SetMassOverrideInKg(float massInKg, bool overrideMass) noexcept {
    // weld면 루트 body에 한다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    if (ForwardToWeldRoot([&](PrimitiveComponent& root) noexcept {
            root.SetMassOverrideInKg(massInKg, overrideMass);
        })) {
        return;
    }

    WarnInvalidPhysicsOperation("SetMassOverrideInKg", false);
    if (!math::IsFinite(massInKg)) {
        WarnInvalidBodyValue("massInKgOverride");
        return;
    }

    bodySettings_.overrideMass = overrideMass;
    bodySettings_.massInKgOverride = massInKg;
    PushBodySettings(BodySettingsChange::Mass);
}

void PrimitiveComponent::SetMassScale(float scale) noexcept {
    // weld면 루트 body에 한다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    if (ForwardToWeldRoot([&](PrimitiveComponent& root) noexcept { root.SetMassScale(scale); })) {
        return;
    }

    WarnInvalidPhysicsOperation("SetMassScale", false);
    if (!math::IsFinite(scale)) {
        WarnInvalidBodyValue("massScale");
        return;
    }

    bodySettings_.massScale = scale;
    PushBodySettings(BodySettingsChange::Mass);
}

Vec3d PrimitiveComponent::GetCenterOfMass() const noexcept {
    // weld면 루트 body 값이다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    Vec3d forwarded = Vec3d::Zero();
    if (ForwardToWeldRoot(
            [&](PrimitiveComponent& root) noexcept { forwarded = root.GetCenterOfMass(); })) {
        return forwarded;
    }

    BodyKinematics kinematics;
    return ReadBodyKinematics(kinematics) ? kinematics.centerOfMass : Vec3d::Zero();
}

void PrimitiveComponent::SetCenterOfMass(const Vec3d& offset) noexcept {
    // weld면 루트 body에 한다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    if (ForwardToWeldRoot(
            [&](PrimitiveComponent& root) noexcept { root.SetCenterOfMass(offset); })) {
        return;
    }

    WarnInvalidPhysicsOperation("SetCenterOfMass", false);
    if (!math::IsFinite(offset.x) || !math::IsFinite(offset.y) || !math::IsFinite(offset.z)) {
        WarnInvalidBodyValue("centerOfMassOffset");
        return;
    }

    bodySettings_.centerOfMassOffset = offset;
    PushBodySettings(BodySettingsChange::Mass);
}

Vec3d PrimitiveComponent::GetInertiaTensor() const noexcept {
    // weld면 루트 body 값이다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    Vec3d forwarded = Vec3d::Zero();
    if (ForwardToWeldRoot(
            [&](PrimitiveComponent& root) noexcept { forwarded = root.GetInertiaTensor(); })) {
        return forwarded;
    }

    BodyKinematics kinematics;
    return ReadBodyKinematics(kinematics) ? kinematics.inertia : Vec3d::Zero();
}

void PrimitiveComponent::SetPhysicsMaxAngularVelocityInDegrees(float maxDegrees,
                                                               bool addToCurrent) noexcept {
    // weld면 루트 body에 한다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    if (ForwardToWeldRoot([&](PrimitiveComponent& root) noexcept {
            root.SetPhysicsMaxAngularVelocityInDegrees(maxDegrees, addToCurrent);
        })) {
        return;
    }

    WarnInvalidPhysicsOperation("SetPhysicsMaxAngularVelocity", false);
    // UE는 physics actor가 있을 때만 지금 값에 더한다(BodyInstance.cpp:3677-3686).
    BodyKinematics kinematics;
    const bool hasBody = ReadBodyKinematics(kinematics);
    const float target =
        addToCurrent && hasBody ? GetPhysicsMaxAngularVelocityInDegrees() + maxDegrees : maxDegrees;
    // Jolt는 음수 상한을 assert한다(MotionProperties::SetMaxAngularVelocity).
    if (!math::IsFinite(target) || target < 0.0f) {
        WarnInvalidBodyValue("maxAngularVelocity");
        return;
    }

    bodySettings_.maxAngularVelocity = target;
    bodySettings_.overrideMaxAngularVelocity = true;
    PushBodySettings(BodySettingsChange::MaxAngularVelocity);
}

void PrimitiveComponent::SetPhysicsMaxAngularVelocityInRadians(float maxRadians,
                                                               bool addToCurrent) noexcept {
    SetPhysicsMaxAngularVelocityInDegrees(math::RadiansToDegrees(maxRadians), addToCurrent);
}

float PrimitiveComponent::GetPhysicsMaxAngularVelocityInDegrees() const noexcept {
    // weld면 루트 body 값이다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    float forwarded = 0.0f;
    if (ForwardToWeldRoot([&](PrimitiveComponent& root) noexcept {
            forwarded = root.GetPhysicsMaxAngularVelocityInDegrees();
        })) {
        return forwarded;
    }

    if (bodySettings_.overrideMaxAngularVelocity) {
        return bodySettings_.maxAngularVelocity;
    }

    const PhysicsScene* scene = HasWorld() ? GetWorld().GetPhysicsScene() : nullptr;
    return scene != nullptr ? static_cast<float>(scene->DefaultMaxAngularVelocityDegrees())
                            : DefaultMaxAngularVelocityDegrees;
}

float PrimitiveComponent::GetPhysicsMaxAngularVelocityInRadians() const noexcept {
    return math::DegreesToRadians(GetPhysicsMaxAngularVelocityInDegrees());
}

void PrimitiveComponent::SetEnableGravity(bool enable) noexcept {
    // weld면 루트 body에 한다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    if (ForwardToWeldRoot(
            [&](PrimitiveComponent& root) noexcept { root.SetEnableGravity(enable); })) {
        return;
    }

    // UE는 값이 같으면 아무것도 하지 않는다(깨우지도 않는다 — BodyInstance.cpp:3953).
    if (bodySettings_.enableGravity == enable) {
        return;
    }

    bodySettings_.enableGravity = enable;
    PushBodySettings(BodySettingsChange::Gravity);
}

void PrimitiveComponent::SetLinearDamping(float damping) noexcept {
    // weld면 루트 body에 한다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    if (ForwardToWeldRoot(
            [&](PrimitiveComponent& root) noexcept { root.SetLinearDamping(damping); })) {
        return;
    }

    if (!math::IsFinite(damping) || damping < 0.0f) {
        WarnInvalidBodyValue("linearDamping");
        return;
    }

    bodySettings_.linearDamping = damping;
    PushBodySettings(BodySettingsChange::Damping);
}

void PrimitiveComponent::SetAngularDamping(float damping) noexcept {
    // weld면 루트 body에 한다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    if (ForwardToWeldRoot(
            [&](PrimitiveComponent& root) noexcept { root.SetAngularDamping(damping); })) {
        return;
    }

    if (!math::IsFinite(damping) || damping < 0.0f) {
        WarnInvalidBodyValue("angularDamping");
        return;
    }

    bodySettings_.angularDamping = damping;
    PushBodySettings(BodySettingsChange::Damping);
}

void PrimitiveComponent::SetUseCCD(bool useCcd) noexcept {
    // weld면 루트 body에 한다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    if (ForwardToWeldRoot([&](PrimitiveComponent& root) noexcept { root.SetUseCCD(useCcd); })) {
        return;
    }

    if (bodySettings_.continuousCollision == useCcd) {
        return;
    }

    bodySettings_.continuousCollision = useCcd;
    PushBodySettings(BodySettingsChange::ContinuousCollision);
}

void PrimitiveComponent::SetGyroscopicTorqueEnabled(bool enable) noexcept {
    // weld면 루트 body에 한다(UE GetBodyInstance() 기본 weld — 7.1.23절).
    if (ForwardToWeldRoot(
            [&](PrimitiveComponent& root) noexcept { root.SetGyroscopicTorqueEnabled(enable); })) {
        return;
    }

    if (bodySettings_.gyroscopicTorque == enable) {
        return;
    }

    bodySettings_.gyroscopicTorque = enable;
    PushBodySettings(BodySettingsChange::GyroscopicTorque);
}

void PrimitiveComponent::SetConstraintMode(DofMode mode) noexcept {
    if (static_cast<uint8>(mode) > static_cast<uint8>(DofMode::None)) {
        WarnInvalidBodyValue("dofMode");
        return;
    }

    // UE SetDOFLock은 같은 방식이어도 잠금을 다시 만든다(BodyInstance.cpp:849-853).
    bodySettings_.dofMode = mode;
    PushBodySettings(BodySettingsChange::DegreesOfFreedom);
}

float PrimitiveComponent::GetMassScale() const noexcept {
    float forwarded = 0.0f;
    if (ForwardToWeldRoot(
            [&](PrimitiveComponent& root) noexcept { forwarded = root.GetMassScale(); })) {
        return forwarded;
    }

    return bodySettings_.massScale;
}

bool PrimitiveComponent::IsGravityEnabled() const noexcept {
    bool forwarded = false;
    if (ForwardToWeldRoot(
            [&](PrimitiveComponent& root) noexcept { forwarded = root.IsGravityEnabled(); })) {
        return forwarded;
    }

    return bodySettings_.enableGravity;
}

float PrimitiveComponent::GetLinearDamping() const noexcept {
    float forwarded = 0.0f;
    if (ForwardToWeldRoot(
            [&](PrimitiveComponent& root) noexcept { forwarded = root.GetLinearDamping(); })) {
        return forwarded;
    }

    return bodySettings_.linearDamping;
}

float PrimitiveComponent::GetAngularDamping() const noexcept {
    float forwarded = 0.0f;
    if (ForwardToWeldRoot(
            [&](PrimitiveComponent& root) noexcept { forwarded = root.GetAngularDamping(); })) {
        return forwarded;
    }

    return bodySettings_.angularDamping;
}

bool PrimitiveComponent::GetGyroscopicTorqueEnabled() const noexcept {
    bool forwarded = false;
    if (ForwardToWeldRoot([&](PrimitiveComponent& root) noexcept {
            forwarded = root.GetGyroscopicTorqueEnabled();
        })) {
        return forwarded;
    }

    return bodySettings_.gyroscopicTorque;
}

} // namespace rocks
