#include <Core/Math/MathUtility.h>
#include <Core/Math/VectorUtility.h>
#include <World/Actor.generated.h>
#include <World/Actor.h>
#include <World/Component.generated.h>
#include <World/Movement/MovementComponent.generated.h>
#include <World/Movement/MovementComponent.h>
#include <World/Movement/ProjectileMovementComponent.generated.h>
#include <World/Movement/ProjectileMovementComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/SceneComponent.h>
#include <World/World.h>
#include "../ComponentValueChecks.h"

// 이동 컴포넌트 기반·회전·발사체다(Docs/Design/EngineRuntime 26.3–26.5절, D4). 충돌 sweep 없이
// 움직인다.
namespace rocks {

ProjectileMovementComponent::ProjectileMovementComponent(Allocator& allocator) noexcept
    : MovementComponent(allocator) {
    // UE 생성자(ProjectileMovementComponent.cpp:18): Velocity = (1, 0, 0).
    velocity_ = Vec3d(1.0, 0.0, 0.0);
}

Result<void> ProjectileMovementComponent::SetInitialSpeed(double speed) noexcept {
    if (!math::IsFinite(speed) || speed < 0.0) {
        return component_checks::Invalid();
    }

    initialSpeed_ = speed;
    return Result<void>::Success();
}

Result<void> ProjectileMovementComponent::SetMaxSpeed(double speed) noexcept {
    if (!math::IsFinite(speed) || speed < 0.0) {
        return component_checks::Invalid();
    }

    maxSpeed_ = speed;
    return Result<void>::Success();
}

Result<void> ProjectileMovementComponent::SetProjectileGravityScale(double scale) noexcept {
    if (!math::IsFinite(scale)) {
        return component_checks::Invalid();
    }

    projectileGravityScale_ = scale;
    return Result<void>::Success();
}

Result<void>
ProjectileMovementComponent::SetHomingAccelerationMagnitude(double magnitude) noexcept {
    if (!math::IsFinite(magnitude) || magnitude < 0.0) {
        return component_checks::Invalid();
    }

    homingAccelerationMagnitude_ = magnitude;
    return Result<void>::Success();
}

Result<void> ProjectileMovementComponent::SetMaxSimulationTimeStep(double seconds) noexcept {
    if (!math::IsFinite(seconds) || seconds < MinSimulationTimeStep ||
        seconds > MaxSimulationTimeStepLimit) {
        return component_checks::Invalid();
    }

    maxSimulationTimeStep_ = seconds;
    return Result<void>::Success();
}

Result<void> ProjectileMovementComponent::SetMaxSimulationIterations(int32 iterations) noexcept {
    if (iterations < 1 || iterations > MaxSimulationIterationsLimit) {
        return component_checks::Invalid();
    }

    maxSimulationIterations_ = iterations;
    return Result<void>::Success();
}

Result<void> ProjectileMovementComponent::SetBounciness(double bounciness) noexcept {
    if (!math::IsFinite(bounciness) || bounciness < 0.0) {
        return component_checks::Invalid();
    }

    bounciness_ = bounciness;
    return Result<void>::Success();
}

Result<void> ProjectileMovementComponent::SetFriction(double friction) noexcept {
    if (!math::IsFinite(friction) || friction < 0.0) {
        return component_checks::Invalid();
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
    if (component_checks::FiniteVector(force)) {
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
            acceleration =
                acceleration + component_checks::SafeNormal(target.Value() - UpdatedLocation()) *
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
            velocity_ = component_checks::SafeNormal(velocity_) * initialSpeed_;
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
