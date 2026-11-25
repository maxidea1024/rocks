#include <Core/Math/MathUtility.h>
#include <Core/Math/VectorUtility.h>
#include <World/Actor.generated.h>
#include <World/Actor.h>
#include <World/Component.generated.h>
#include <World/Movement/MovementComponent.generated.h>
#include <World/Movement/MovementComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/SceneComponent.h>
#include <World/World.h>
#include "../ComponentValueChecks.h"

// 이동 컴포넌트 기반·회전·발사체다(Docs/Design/EngineRuntime 26.3–26.5절, D4). 충돌 sweep 없이
// 움직인다.
namespace rocks {

namespace {

// UE FQuat::Equals(Quat.h:994): q와 −q를 같은 회전으로 본다.
bool SameRotation(const Quatd& a, const Quatd& b, double tolerance) noexcept {
    const bool same = math::Abs(a.x - b.x) <= tolerance && math::Abs(a.y - b.y) <= tolerance &&
                      math::Abs(a.z - b.z) <= tolerance && math::Abs(a.w - b.w) <= tolerance;
    const bool opposite = math::Abs(a.x + b.x) <= tolerance && math::Abs(a.y + b.y) <= tolerance &&
                          math::Abs(a.z + b.z) <= tolerance && math::Abs(a.w + b.w) <= tolerance;
    return same || opposite;
}

} // namespace

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
            return component_checks::Invalid();
        }

        // UE IsValid(MovementComponent.cpp:70): 파괴 대기 대상은 받지 않는다.
        auto scene = GetWorld().With<SceneComponent>(component, [](SceneComponent& value) noexcept {
            return value.GetState() != ComponentState::PendingDestroy;
        });
        if (!scene) {
            return Result<void>::Failure(scene.Error());
        }

        if (!scene.Value()) {
            return component_checks::Invalid();
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
    if (!component_checks::FiniteVector(velocity)) {
        return component_checks::Invalid();
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
        return component_checks::Invalid();
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
        return component_checks::SafeNormal(planeConstraintNormal_);
    case PlaneConstraintAxis::UseGlobalPhysicsSetting:
    default:
        // 전역 자유도 설정이 없다 — 제약 없음(26.3절).
        return Vec3d();
    }
}

Result<void> MovementComponent::SetPlaneConstraintNormal(const Vec3d& normal) noexcept {
    if (!component_checks::FiniteVector(normal)) {
        return component_checks::Invalid();
    }

    planeConstraintNormal_ = component_checks::SafeNormal(normal);
    planeConstraintAxis_ = PlaneConstraintAxis::Custom;
    return Result<void>::Success();
}

Result<void> MovementComponent::SetPlaneConstraintFromVectors(const Vec3d& forward,
                                                              const Vec3d& up) noexcept {
    return SetPlaneConstraintNormal(Cross(up, forward));
}

Result<void> MovementComponent::SetPlaneConstraintOrigin(const Vec3d& origin) noexcept {
    if (!component_checks::FiniteVector(origin)) {
        return component_checks::Invalid();
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
    return constrainToPlane_ ? component_checks::SafeNormal(
                                   math::VectorPlaneProject(normal, PlaneConstraintNormal()))
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
    if (updatedComponent_.IsNull() || !HasWorld() || !component_checks::FiniteVector(delta)) {
        return component_checks::Invalid();
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

} // namespace rocks
