#include <Core/Math/MathUtility.h>
#include <Core/Math/VectorUtility.h>
#include <World/Actor.generated.h>
#include <World/Actor.h>
#include <World/Component.generated.h>
#include <World/Movement/MovementComponent.generated.h>
#include <World/Movement/RotatingMovementComponent.generated.h>
#include <World/Movement/MovementComponent.h>
#include <World/Movement/RotatingMovementComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/SceneComponent.h>
#include <World/World.h>
#include <World/ComponentValueChecks.h>

// 이동 컴포넌트 기반·회전·발사체다(Docs/Design/EngineRuntime 26.3–26.5절, D4). 충돌 sweep 없이
// 움직인다.
namespace rocks {

Result<void> RotatingMovementComponent::SetRotationRate(const Rotator<double>& rate) noexcept {
    if (!math::IsFinite(rate.pitch) || !math::IsFinite(rate.yaw) || !math::IsFinite(rate.roll)) {
        return component_checks::Invalid();
    }

    rotationRate_ = rate;
    return Result<void>::Success();
}

Result<void> RotatingMovementComponent::SetPivotTranslation(const Vec3d& pivot) noexcept {
    if (!component_checks::FiniteVector(pivot)) {
        return component_checks::Invalid();
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

} // namespace rocks
