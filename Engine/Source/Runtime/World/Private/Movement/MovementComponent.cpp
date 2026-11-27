#include <Core/Math/MathUtility.h>
#include <Core/Math/VectorUtility.h>
#include <World/Actor.generated.h>
#include <World/Actor.h>
#include <World/Component.generated.h>
#include <World/Movement/MovementComponent.generated.h>
#include <World/Movement/MovementComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/SceneComponent.h>
#include <World/ComponentValueChecks.h>
#include <World/Physics/WorldCollision.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/Primitives/PrimitiveComponent.h>
#include <World/World.h>

// 이동 컴포넌트 기반·회전·발사체다(Docs/Design/EngineRuntime 26.3–26.5절, D4). 기본 파생은 sweep
// 없이 움직이고, sweep 이동·침투 해소 도우미는 Docs/Design/PhysicsModule 7.1.13절이다.
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

MovementComponent::MovementComponent(Allocator& allocator) noexcept
    : Component(allocator), overlapIgnoreActors_(allocator) {
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

Result<bool> MovementComponent::MoveUpdatedComponent(const Vec3d& delta, const Quatd& rotation,
                                                     bool sweep, HitResult* outHit,
                                                     TeleportMode teleport) noexcept {
    MoveOptions options;
    options.teleport = teleport;
    return MoveUpdatedComponentWith(delta, rotation, sweep, outHit, options);
}

Result<bool> MovementComponent::MoveUpdatedComponentWith(const Vec3d& delta, const Quatd& rotation,
                                                         bool sweep, HitResult* outHit,
                                                         const MoveOptions& options) noexcept {
    if (updatedComponent_.IsNull() || !HasWorld() || !component_checks::FiniteVector(delta)) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    // UE MoveUpdatedComponentImpl: 평면 제약한 delta로 대상의 MoveComponent를 부른다.
    const Vec3d constrained = ConstrainDirectionToPlane(delta);
    World& world = GetWorld();
    auto swept = world.With<PrimitiveComponent>(
        updatedComponent_, [&](PrimitiveComponent& primitive) noexcept {
            return primitive.MoveComponent(constrained, rotation, sweep, outHit, options);
        });
    if (swept || swept.Error() != ErrorCode::TypeMismatch) {
        return swept;
    }

    // Primitive가 아닌 대상은 sweep 없이 옮긴다(UE USceneComponent::MoveComponentImpl).
    return world.With<SceneComponent>(
        updatedComponent_, [&](SceneComponent& scene) noexcept -> Result<bool> {
            const Vec3d start = scene.WorldLocation();
            if (outHit != nullptr) {
                *outHit = HitResult{};
                outHit->traceStart = start;
                outHit->traceEnd = start + constrained;
            }

            if (constrained.LengthSquared() == 0.0 &&
                SameRotation(scene.WorldQuaternion(), rotation, QuaternionTolerance)) {
                return Result<bool>::Success(true);
            }

            auto moved = scene.SetWorldLocationAndRotation(start + constrained, rotation);
            if (!moved) {
                return Result<bool>::Failure(moved.Error());
            }

            return Result<bool>::Success(true);
        });
}

Vec3d MovementComponent::GetPenetrationAdjustment(const HitResult& hit) const noexcept {
    if (!hit.startPenetrating) {
        return Vec3d::Zero();
    }

    const double depth =
        hit.penetrationDepth > 0.0 ? hit.penetrationDepth : DefaultPenetrationDepth;
    return ConstrainDirectionToPlane(hit.normal * (depth + math::Abs(PenetrationPullbackDistance)));
}

bool MovementComponent::OverlapTest(const Vec3d& location, const Quatd& rotation,
                                    double inflation) noexcept {
    // 질의를 마치지 못하면(대상을 빌리지 못함·충돌 해석 실패·할당 실패) 막힌 것으로 본다: teleport
    // 대신 sweep으로 가므로 벽 안으로 순간 이동하지 않는다.
    World& world = GetWorld();
    bool blocked = true;
    (void)world.With<PrimitiveComponent>(
        updatedComponent_, [&](PrimitiveComponent& primitive) noexcept {
            auto collision = primitive.TryResolveCollision();
            if (!collision) {
                return;
            }

            // UE InitCollisionParams → InitSweepCollisionParams: 대상 응답, 이동 무시 목록과 소유
            // Actor를 뺀다.
            const ObjectHandle owner = primitive.GetOwner();
            const Span<const ObjectHandle> moveIgnore = primitive.MoveIgnoreActors();
            overlapIgnoreActors_.Clear();
            if (!overlapIgnoreActors_.TryAppend(moveIgnore) ||
                !overlapIgnoreActors_.TryPushBack(owner)) {
                return;
            }

            QueryParams params;
            params.ignoreActors = overlapIgnoreActors_.View();
            params.ignoreComponents = primitive.MoveIgnoreComponents();
            blocked = OverlapBlocking(
                world, location, rotation, primitive.GetCollisionShape(inflation),
                QueryFilter::ByChannel(collision.Value().objectType, collision.Value().responses),
                params);
        });
    return blocked;
}

Result<bool> MovementComponent::ResolvePenetration(const Vec3d& adjustment, const HitResult& hit,
                                                   const Quatd& rotation) noexcept {
    const Vec3d constrained = ConstrainDirectionToPlane(adjustment);
    if (constrained.LengthSquared() == 0.0 || updatedComponent_.IsNull() || !HasWorld()) {
        return Result<bool>::Success(false);
    }

    // SceneComponent는 박힐 수 없다 — Primitive이고 소유 Actor가 있을 때만 한다(UE).
    World& world = GetWorld();
    auto owner = world.With<PrimitiveComponent>(
        updatedComponent_,
        [](PrimitiveComponent& primitive) noexcept { return primitive.GetOwner(); });
    if (!owner || owner.Value().IsNull()) {
        return Result<bool>::Success(false);
    }

    MoveOptions teleport;
    teleport.teleport = TeleportMode::TeleportPhysics;
    // 밀어낸 자리가 비었으면 sweep 없이 옮긴다(겹침 시험은 조금 부풀려 더 엄격히 — UE).
    if (!OverlapTest(hit.traceStart + constrained, rotation, PenetrationOverlapCheckInflation)) {
        auto moved = MoveUpdatedComponentWith(constrained, rotation, false, nullptr, teleport);
        if (!moved) {
            return moved;
        }

        return Result<bool>::Success(true);
    }

    // 막혔으면 sweep으로 할 수 있는 만큼 나온다(neverIgnoreBlockingOverlaps 없이 — 빠져나가는 시작
    // 겹침은 무시된다).
    HitResult sweepOut;
    auto moved = MoveUpdatedComponentWith(constrained, rotation, true, &sweepOut, teleport);
    if (!moved) {
        return moved;
    }

    bool done = moved.Value();
    if (!done && sweepOut.startPenetrating) {
        // 두 MTD를 합쳐 여러 면에서 나오는 방향을 찾는다.
        const Vec3d second = GetPenetrationAdjustment(sweepOut);
        const Vec3d combined = constrained + second;
        if (!(second == constrained) && combined.LengthSquared() != 0.0) {
            auto retried = MoveUpdatedComponentWith(combined, rotation, true, nullptr, teleport);
            if (!retried) {
                return retried;
            }

            done = retried.Value();
        }
    }

    if (!done) {
        // 조정량 + 원래 이동, 그다음 (같은 쪽이면) 원래 이동만 sweep한다.
        const Vec3d moveDelta = ConstrainDirectionToPlane(hit.traceEnd - hit.traceStart);
        if (moveDelta.LengthSquared() != 0.0) {
            auto adjusted = MoveUpdatedComponentWith(constrained + moveDelta, rotation, true,
                                                     nullptr, teleport);
            if (!adjusted) {
                return adjusted;
            }

            done = adjusted.Value();
            if (!done && Dot(moveDelta, constrained) > 0.0) {
                auto original =
                    MoveUpdatedComponentWith(moveDelta, rotation, true, nullptr, teleport);
                if (!original) {
                    return original;
                }

                done = original.Value();
            }
        }
    }

    return Result<bool>::Success(done);
}

Result<bool> MovementComponent::SafeMoveUpdatedComponent(const Vec3d& delta, const Quatd& rotation,
                                                         bool sweep, HitResult& outHit,
                                                         TeleportMode teleport) noexcept {
    if (updatedComponent_.IsNull() || !HasWorld()) {
        outHit = HitResult{};
        return Result<bool>::Success(false);
    }

    // 막는 시작 겹침을 무시하지 않고 그 hit를 발행하지 않는다(UE CVar
    // p.MoveIgnoreFirstBlockingOverlap 기본 0 — 먼저 밀어낸다).
    MoveOptions first;
    first.teleport = teleport;
    first.neverIgnoreBlockingOverlaps = true;
    first.disableBlockingOverlapDispatch = true;
    auto moved = MoveUpdatedComponentWith(delta, rotation, sweep, &outHit, first);
    if (!moved) {
        return moved;
    }

    if (!outHit.startPenetrating) {
        return moved;
    }

    auto resolved = ResolvePenetration(GetPenetrationAdjustment(outHit), outHit, rotation);
    if (!resolved) {
        return resolved;
    }

    if (!resolved.Value()) {
        return moved;
    }

    // 원래 이동을 다시 한다(기본 표지).
    return MoveUpdatedComponent(delta, rotation, sweep, &outHit, teleport);
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
