#include <Core/Math/MathUtility.h>
#include <Core/Math/VectorUtility.h>
#include <World/Actor.generated.h>
#include <World/Actor.h>
#include <World/Component.generated.h>
#include <World/MovementComponent.generated.h>
#include <World/MovementComponent.h>
#include <World/ProjectileMovementComponent.generated.h>
#include <World/ProjectileMovementComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/SceneComponent.h>
#include <World/World.h>

// 이동 컴포넌트 기반·회전·발사체다(Docs/Design/EngineRuntime 26.3–26.5절, D4). 충돌 sweep 없이
// 움직인다.
namespace rocks {

namespace {

Result<void> InvalidMovementValue() noexcept {
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

bool IsFiniteMovementVector(const Vec3d& value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

Vec3d SafeMovementNormal(const Vec3d& value) noexcept {
    auto normal = value.TryNormalize();
    return normal ? normal.Value() : Vec3d();
}

// UE FQuat::Equals(Quat.h:994): q와 −q를 같은 회전으로 본다.
bool SameRotation(const Quatd& a, const Quatd& b, double tolerance) noexcept {
    const bool same = math::Abs(a.x - b.x) <= tolerance && math::Abs(a.y - b.y) <= tolerance &&
                      math::Abs(a.z - b.z) <= tolerance && math::Abs(a.w - b.w) <= tolerance;
    const bool opposite = math::Abs(a.x + b.x) <= tolerance && math::Abs(a.y + b.y) <= tolerance &&
                          math::Abs(a.z + b.z) <= tolerance && math::Abs(a.w + b.w) <= tolerance;
    return same || opposite;
}

} // namespace

// --- MovementComponent ---

MovementComponent::MovementComponent(Allocator& allocator) noexcept {
    (void)allocator;
    // UE TG_PrePhysics(MovementComponent.cpp:25).
    TickSettings& tick = PrimaryTick();
    tick.tick = true;
    tick.phase = TickPhase::PrePhysics;
}

Result<void> MovementComponent::SetUpdatedComponent(ObjectHandle component) noexcept {
    if (!component.IsNull()) {
        if (!HasWorld()) {
            return InvalidMovementValue();
        }

        // UE IsValid(MovementComponent.cpp:70): 파괴 대기 대상은 받지 않는다.
        auto scene = GetWorld().With<SceneComponent>(component, [](SceneComponent& value) noexcept {
            return value.GetState() != ComponentState::PendingDestroy;
        });
        if (!scene) {
            return Result<void>::Failure(scene.Error());
        }

        if (!scene.Value()) {
            return InvalidMovementValue();
        }
    }

    // UE SetUpdatedComponent(MovementComponent.cpp:52): 옛 대상의 간선을 떼고 새 대상에 건다.
    if (HasWorld() && !updatedComponent_.IsNull()) {
        (void)GetWorld().RemoveTickPrerequisite({updatedComponent_}, {Handle()},
                                                TickEdgeSource::Framework);
    }

    updatedComponent_ = component;
    if (HasWorld() && !component.IsNull()) {
        // 대상이 OnTick을 켜지 않았으면 InvalidArgument다 — 간선이 필요 없다.
        (void)GetWorld().AddTickPrerequisite({component}, {Handle()}, TickEdgeSource::Framework);
    }

    if (autoUpdateTickRegistration_) {
        (void)SetTickEnabled(!updatedComponent_.IsNull());
    }

    if (snapToPlaneAtStart_ && !updatedComponent_.IsNull()) {
        (void)SnapUpdatedComponentToPlane();
    }

    return Result<void>::Success();
}

void MovementComponent::AutoRegisterUpdatedComponent() noexcept {
    if (!updatedComponent_.IsNull() || !autoRegisterUpdatedComponent_ || !HasWorld()) {
        return;
    }

    auto root = GetWorld().With<Actor>(GetOwner(),
                                       [](Actor& owner) noexcept { return owner.RootComponent(); });
    if (root && !root.Value().IsNull()) {
        (void)SetUpdatedComponent(root.Value());
    }
}

Result<void> MovementComponent::OnRegister() noexcept {
    // UE OnRegister·RegisterComponentTickFunctions: 소유 root를 대상으로, 소유 Actor Tick을 뒤로.
    AutoRegisterUpdatedComponent();
    if (tickBeforeOwner_) {
        (void)GetWorld().AddTickPrerequisite({GetOwner()}, {Handle()}, TickEdgeSource::Framework);
    }

    if (autoUpdateTickRegistration_ && updatedComponent_.IsNull()) {
        (void)SetTickEnabled(false);
    }

    return Result<void>::Success();
}

Result<void> MovementComponent::OnAwake() noexcept {
    // UE InitializeComponent: root가 늦게 생겼으면 다시 본다, 그다음 평면에 붙인다.
    AutoRegisterUpdatedComponent();
    if (snapToPlaneAtStart_) {
        (void)SnapUpdatedComponentToPlane();
    }

    return Result<void>::Success();
}

void MovementComponent::OnStart() noexcept {
    // PreAwake 뒤 root가 정해지는 Actor를 위해 시작 때도 본다(26.3절).
    AutoRegisterUpdatedComponent();
}

void MovementComponent::OnTick(const TickContext&) noexcept {
    // UE TickComponent: 대상이 사라졌으면 해제한다.
    if (!updatedComponent_.IsNull() &&
        !GetWorld().With<SceneComponent>(updatedComponent_, [](SceneComponent&) noexcept {})) {
        (void)SetUpdatedComponent({});
    }
}

Result<void> MovementComponent::SetVelocity(const Vec3d& velocity) noexcept {
    if (!IsFiniteMovementVector(velocity)) {
        return InvalidMovementValue();
    }

    velocity_ = velocity;
    return Result<void>::Success();
}

void MovementComponent::UpdateComponentVelocity() noexcept {
    if (updatedComponent_.IsNull() || !HasWorld()) {
        return;
    }

    const Vec3d velocity = velocity_;
    (void)GetWorld().With<SceneComponent>(
        updatedComponent_,
        [velocity](SceneComponent& scene) noexcept { scene.SetComponentVelocity(velocity); });
}

void MovementComponent::StopMovementImmediately() noexcept {
    velocity_ = Vec3d();
    UpdateComponentVelocity();
}

bool MovementComponent::IsExceedingMaxSpeed(double maxSpeed) const noexcept {
    const double limit = math::Max(0.0, maxSpeed);
    return velocity_.LengthSquared() > limit * limit * MaxSpeedSlack;
}

Result<void> MovementComponent::SetPlaneConstraintAxisSetting(PlaneConstraintAxis axis) noexcept {
    if (static_cast<uint8>(axis) >
        static_cast<uint8>(PlaneConstraintAxis::UseGlobalPhysicsSetting)) {
        return InvalidMovementValue();
    }

    planeConstraintAxis_ = axis;
    return Result<void>::Success();
}

Vec3d MovementComponent::PlaneConstraintNormal() const noexcept {
    switch (planeConstraintAxis_) {
    case PlaneConstraintAxis::X:
        return Vec3d(1.0, 0.0, 0.0);
    case PlaneConstraintAxis::Y:
        return Vec3d(0.0, 1.0, 0.0);
    case PlaneConstraintAxis::Z:
        return Vec3d(0.0, 0.0, 1.0);
    case PlaneConstraintAxis::Custom:
        // 로드·편집 값도 단위 법선으로 쓴다(UE OnRegister 정규화 — MovementComponent.cpp:155).
        return SafeMovementNormal(planeConstraintNormal_);
    case PlaneConstraintAxis::UseGlobalPhysicsSetting:
    default:
        // 전역 자유도 설정이 없다 — 제약 없음(26.3절).
        return Vec3d();
    }
}

Result<void> MovementComponent::SetPlaneConstraintNormal(const Vec3d& normal) noexcept {
    if (!IsFiniteMovementVector(normal)) {
        return InvalidMovementValue();
    }

    planeConstraintNormal_ = SafeMovementNormal(normal);
    planeConstraintAxis_ = PlaneConstraintAxis::Custom;
    return Result<void>::Success();
}

Result<void> MovementComponent::SetPlaneConstraintFromVectors(const Vec3d& forward,
                                                              const Vec3d& up) noexcept {
    return SetPlaneConstraintNormal(Cross(up, forward));
}

Result<void> MovementComponent::SetPlaneConstraintOrigin(const Vec3d& origin) noexcept {
    if (!IsFiniteMovementVector(origin)) {
        return InvalidMovementValue();
    }

    planeConstraintOrigin_ = origin;
    return Result<void>::Success();
}

Vec3d MovementComponent::ConstrainDirectionToPlane(const Vec3d& direction) const noexcept {
    return constrainToPlane_ ? math::VectorPlaneProject(direction, PlaneConstraintNormal())
                             : direction;
}

Vec3d MovementComponent::ConstrainLocationToPlane(const Vec3d& location) const noexcept {
    return constrainToPlane_
               ? math::PointPlaneProject(location, planeConstraintOrigin_, PlaneConstraintNormal())
               : location;
}

Vec3d MovementComponent::ConstrainNormalToPlane(const Vec3d& normal) const noexcept {
    return constrainToPlane_
               ? SafeMovementNormal(math::VectorPlaneProject(normal, PlaneConstraintNormal()))
               : normal;
}

Result<void> MovementComponent::SnapUpdatedComponentToPlane() noexcept {
    if (!constrainToPlane_ || updatedComponent_.IsNull() || !HasWorld()) {
        return Result<void>::Success();
    }

    const Vec3d target = ConstrainLocationToPlane(UpdatedLocation());
    const Quatd rotation = UpdatedRotation();
    return GetWorld().With<SceneComponent>(updatedComponent_, [&](SceneComponent& scene) noexcept {
        return scene.SetWorldLocationAndRotation(target, rotation);
    });
}

Vec3d MovementComponent::UpdatedLocation() const noexcept {
    if (updatedComponent_.IsNull() || !HasWorld()) {
        return Vec3d();
    }

    auto location = GetWorld().With<SceneComponent>(
        updatedComponent_, [](SceneComponent& scene) noexcept { return scene.WorldLocation(); });
    return location ? location.Value() : Vec3d();
}

Quatd MovementComponent::UpdatedRotation() const noexcept {
    if (updatedComponent_.IsNull() || !HasWorld()) {
        return Quatd::Identity();
    }

    auto rotation = GetWorld().With<SceneComponent>(
        updatedComponent_, [](SceneComponent& scene) noexcept { return scene.WorldQuaternion(); });
    return rotation ? rotation.Value() : Quatd::Identity();
}

Result<void> MovementComponent::MoveUpdatedComponent(const Vec3d& delta,
                                                     const Quatd& rotation) noexcept {
    if (updatedComponent_.IsNull() || !HasWorld() || !IsFiniteMovementVector(delta)) {
        return InvalidMovementValue();
    }

    // UE MoveUpdatedComponentImpl → MoveComponentImpl(sweep 없음): 평면 제약한 delta로 순간 이동.
    const Vec3d constrained = ConstrainDirectionToPlane(delta);
    return GetWorld().With<SceneComponent>(updatedComponent_, [&](SceneComponent& scene) noexcept {
        if (constrained.LengthSquared() == 0.0 &&
            SameRotation(scene.WorldQuaternion(), rotation, QuaternionTolerance)) {
            return Result<void>::Success();
        }

        return scene.SetWorldLocationAndRotation(scene.WorldLocation() + constrained, rotation);
    });
}

bool MovementComponent::ShouldSkipUpdate() const noexcept {
    if (updatedComponent_.IsNull() || !HasWorld()) {
        return true;
    }

    auto movable =
        GetWorld().With<SceneComponent>(updatedComponent_, [](SceneComponent& scene) noexcept {
            return scene.Mobility() == ComponentMobility::Movable;
        });
    return !movable || !movable.Value();
}

// --- RotatingMovementComponent ---

Result<void> RotatingMovementComponent::SetRotationRate(const Rotator<double>& rate) noexcept {
    if (!math::IsFinite(rate.pitch) || !math::IsFinite(rate.yaw) || !math::IsFinite(rate.roll)) {
        return InvalidMovementValue();
    }

    rotationRate_ = rate;
    return Result<void>::Success();
}

Result<void> RotatingMovementComponent::SetPivotTranslation(const Vec3d& pivot) noexcept {
    if (!IsFiniteMovementVector(pivot)) {
        return InvalidMovementValue();
    }

    pivotTranslation_ = pivot;
    return Result<void>::Success();
}

void RotatingMovementComponent::OnTick(const TickContext& context) noexcept {
    // UE RotatingMovementComponent.cpp:20–51. 기반 Tick이 먼저 사라진 대상을 푼다(UE는 파괴 대기
    // 대상이 ShouldSkipUpdate를 통과해 Super가 푼다).
    MovementComponent::OnTick(context);
    if (ShouldSkipUpdate()) {
        return;
    }

    const double dt = context.deltaSeconds;
    const Quatd oldRotation = UpdatedRotation();
    const Quatd deltaRotation =
        Rotator<double>(rotationRate_.pitch * dt, rotationRate_.yaw * dt, rotationRate_.roll * dt)
            .ToQuaternion();
    auto normalized =
        (rotationInLocalSpace_ ? oldRotation * deltaRotation : deltaRotation * oldRotation)
            .TryNormalize();
    const Quatd newRotation = normalized ? normalized.Value() : oldRotation;
    Vec3d deltaLocation;
    if (!(pivotTranslation_.LengthSquared() == 0.0)) {
        deltaLocation = oldRotation.RotateVector(pivotTranslation_) -
                        newRotation.RotateVector(pivotTranslation_);
    }

    (void)MoveUpdatedComponent(deltaLocation, newRotation);
}

// --- ProjectileMovementComponent ---

ProjectileMovementComponent::ProjectileMovementComponent(Allocator& allocator) noexcept
    : MovementComponent(allocator) {
    // UE 생성자(ProjectileMovementComponent.cpp:18): Velocity = (1, 0, 0).
    velocity_ = Vec3d(1.0, 0.0, 0.0);
}

Result<void> ProjectileMovementComponent::SetInitialSpeed(double speed) noexcept {
    if (!math::IsFinite(speed) || speed < 0.0) {
        return InvalidMovementValue();
    }

    initialSpeed_ = speed;
    return Result<void>::Success();
}

Result<void> ProjectileMovementComponent::SetMaxSpeed(double speed) noexcept {
    if (!math::IsFinite(speed) || speed < 0.0) {
        return InvalidMovementValue();
    }

    maxSpeed_ = speed;
    return Result<void>::Success();
}

Result<void> ProjectileMovementComponent::SetProjectileGravityScale(double scale) noexcept {
    if (!math::IsFinite(scale)) {
        return InvalidMovementValue();
    }

    projectileGravityScale_ = scale;
    return Result<void>::Success();
}

Result<void>
ProjectileMovementComponent::SetHomingAccelerationMagnitude(double magnitude) noexcept {
    if (!math::IsFinite(magnitude) || magnitude < 0.0) {
        return InvalidMovementValue();
    }

    homingAccelerationMagnitude_ = magnitude;
    return Result<void>::Success();
}

Result<void> ProjectileMovementComponent::SetMaxSimulationTimeStep(double seconds) noexcept {
    if (!math::IsFinite(seconds) || seconds < MinSimulationTimeStep ||
        seconds > MaxSimulationTimeStepLimit) {
        return InvalidMovementValue();
    }

    maxSimulationTimeStep_ = seconds;
    return Result<void>::Success();
}

Result<void> ProjectileMovementComponent::SetMaxSimulationIterations(int32 iterations) noexcept {
    if (iterations < 1 || iterations > MaxSimulationIterationsLimit) {
        return InvalidMovementValue();
    }

    maxSimulationIterations_ = iterations;
    return Result<void>::Success();
}

Result<void> ProjectileMovementComponent::SetBounciness(double bounciness) noexcept {
    if (!math::IsFinite(bounciness) || bounciness < 0.0) {
        return InvalidMovementValue();
    }

    bounciness_ = bounciness;
    return Result<void>::Success();
}

Result<void> ProjectileMovementComponent::SetFriction(double friction) noexcept {
    if (!math::IsFinite(friction) || friction < 0.0) {
        return InvalidMovementValue();
    }

    friction_ = friction;
    return Result<void>::Success();
}

double ProjectileMovementComponent::GravityZ() const noexcept {
    // UE GetGravityZ(ProjectileMovementComponent.cpp:533).
    return projectileGravityScale_ != 0.0 ? MovementComponent::GravityZ() * projectileGravityScale_
                                          : 0.0;
}

void ProjectileMovementComponent::AddForce(const Vec3d& force) noexcept {
    if (IsFiniteMovementVector(force)) {
        pendingForce_ = pendingForce_ + force;
    }
}

void ProjectileMovementComponent::ClearPendingForce(bool clearImmediateForce) noexcept {
    pendingForce_ = Vec3d();
    if (clearImmediateForce) {
        pendingForceThisUpdate_ = Vec3d();
    }
}

Vec3d ProjectileMovementComponent::ComputeAcceleration(const Vec3d&) const noexcept {
    // UE ComputeAcceleration(:495): 중력 + 이번 갱신 힘 + homing.
    Vec3d acceleration(0.0, 0.0, GravityZ());
    acceleration = acceleration + pendingForceThisUpdate_;
    if (homing_ && !homingTarget_.IsNull() && HasWorld()) {
        auto target = GetWorld().With<SceneComponent>(
            homingTarget_, [](SceneComponent& scene) noexcept { return scene.WorldLocation(); });
        if (target) {
            acceleration = acceleration + SafeMovementNormal(target.Value() - UpdatedLocation()) *
                                              homingAccelerationMagnitude_;
        }
    }

    return acceleration;
}

Vec3d ProjectileMovementComponent::ComputeVelocity(const Vec3d& initialVelocity,
                                                   double deltaSeconds) const noexcept {
    // UE ComputeVelocity → LimitVelocity(:459–478).
    Vec3d velocity = initialVelocity + ComputeAcceleration(initialVelocity) * deltaSeconds;
    if (MaxSpeed() > 0.0) {
        velocity = math::ClampedToMaxSize(velocity, MaxSpeed());
    }

    return ConstrainDirectionToPlane(velocity);
}

Vec3d ProjectileMovementComponent::ComputeMoveDelta(const Vec3d& initialVelocity,
                                                    double deltaSeconds) const noexcept {
    // UE ComputeMoveDelta(:480): velocity Verlet.
    const Vec3d newVelocity = ComputeVelocity(initialVelocity, deltaSeconds);
    return initialVelocity * deltaSeconds + (newVelocity - initialVelocity) * (0.5 * deltaSeconds);
}

double ProjectileMovementComponent::SimulationTimeStep(double remainingSeconds,
                                                       int32 iteration) const noexcept {
    // UE GetSimulationTimeStep(:693): 최대 단계보다 길면 마지막 반복 전까지 반으로(최대 단계 이하).
    if (remainingSeconds > maxSimulationTimeStep_ && iteration < maxSimulationIterations_) {
        remainingSeconds = math::Min(maxSimulationTimeStep_, remainingSeconds * 0.5);
    }

    return math::Max(MinTickTime, remainingSeconds);
}

bool ProjectileMovementComponent::ShouldUseSubStepping() const noexcept {
    // UE HomingTargetComponent.IsValid(): 사라진 대상은 없는 것으로 본다.
    const bool target =
        homing_ && !homingTarget_.IsNull() && HasWorld() &&
        GetWorld().With<SceneComponent>(homingTarget_, [](SceneComponent&) noexcept {}).IsOk();
    return forceSubStepping_ || GravityZ() != 0.0 || target;
}

bool ProjectileMovementComponent::HasStoppedSimulation() const noexcept {
    return UpdatedComponent().IsNull() || !IsEnabled();
}

void ProjectileMovementComponent::StopSimulating() noexcept {
    // UE StopSimulating(:540).
    velocity_ = Vec3d();
    pendingForce_ = Vec3d();
    pendingForceThisUpdate_ = Vec3d();
    UpdateComponentVelocity();
    (void)SetUpdatedComponent({});
    OnProjectileStop();
}

Result<void> ProjectileMovementComponent::OnAwake() noexcept {
    if (auto base = MovementComponent::OnAwake(); !base) {
        return base;
    }

    // UE InitializeComponent(:83–124).
    if (velocity_.LengthSquared() > 0.0) {
        if (initialSpeed_ > 0.0) {
            velocity_ = SafeMovementNormal(velocity_) * initialSpeed_;
        }

        if (initialVelocityInLocalSpace_) {
            velocity_ = UpdatedRotation().RotateVector(velocity_);
        }

        if (rotationFollowsVelocity_ && !UpdatedComponent().IsNull()) {
            Rotator<double> facing = math::ToOrientationRotator(velocity_);
            if (rotationRemainsVertical_) {
                facing = Rotator<double>(0.0, Rotator<double>::NormalizeAxis(facing.yaw), 0.0);
            }

            const Vec3d location = UpdatedLocation();
            (void)GetWorld().With<SceneComponent>(
                UpdatedComponent(), [&](SceneComponent& scene) noexcept {
                    return scene.SetWorldLocationAndRotation(location, facing.ToQuaternion());
                });
        }

        UpdateComponentVelocity();
    }

    return Result<void>::Success();
}

void ProjectileMovementComponent::OnTick(const TickContext& context) noexcept {
    // UE TickComponent(:141–334), 충돌 분기 제외.
    pendingForceThisUpdate_ = pendingForce_;
    pendingForce_ = Vec3d();
    // 기반 Tick이 먼저 사라진 대상을 푼다(Rotating과 같은 이유).
    MovementComponent::OnTick(context);
    if (HasStoppedSimulation() || ShouldSkipUpdate() || !simulationEnabled_) {
        return;
    }

    double remaining = context.deltaSeconds;
    int32 iterations = 0;
    while (simulationEnabled_ && remaining >= MinTickTime &&
           iterations < maxSimulationIterations_ && !HasStoppedSimulation()) {
        ++iterations;
        const double step =
            ShouldUseSubStepping() ? SimulationTimeStep(remaining, iterations) : remaining;
        remaining -= step;
        const Vec3d oldVelocity = velocity_;
        const Vec3d moveDelta = ComputeMoveDelta(oldVelocity, step);
        Quatd newRotation = UpdatedRotation();
        if (rotationFollowsVelocity_ && !oldVelocity.IsNearlyZero(RotationVelocityTolerance)) {
            newRotation = math::ToOrientationQuat(oldVelocity);
        }

        if (rotationFollowsVelocity_ && rotationRemainsVertical_) {
            auto rotator = Rotator<double>::TryFromQuaternion(newRotation);
            const double yaw = rotator ? Rotator<double>::NormalizeAxis(rotator.Value().yaw) : 0.0;
            newRotation = Rotator<double>(0.0, yaw, 0.0).ToQuaternion();
        }

        (void)MoveUpdatedComponent(moveDelta, newRotation);
        if (HasStoppedSimulation()) {
            return;
        }

        // 충돌이 없다(sweep 없음): 속도가 그대로면 이 단계의 속도로 적분한다.
        if (velocity_ == oldVelocity) {
            velocity_ = ComputeVelocity(velocity_, step);
        }
    }

    UpdateComponentVelocity();
}

} // namespace rocks
