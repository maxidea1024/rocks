// body 형상·층 만들기(Docs/Design/PhysicsJoltAdapter.ko.md 4절, PhysicsModule 6.1절·7.1.3절).
#include "JoltBodyBuild.h"

#include "JoltConversions.h"
#include "JoltLayers.h"

#include <Collision/CollisionTypes.h>
#include <Core/Math/MathUtility.h>

// clang-format off
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
// clang-format on

namespace rocks::physics {

namespace {

// 0인 scale 축을 바꾸는 값이다(UE KINDA_SMALL_NUMBER — PrimitiveComponent.cpp:909-925, Rocks는 어느
// 한 축이라도 0이면 그 축을 바꾼다 — 2.3절).
double SafeScaleAxis(double axis) noexcept {
    const double magnitude = math::Abs(axis);
    return magnitude < math::KindaSmallNumber<double> ? math::KindaSmallNumber<double> : magnitude;
}

Result<JPH::RefConst<JPH::Shape>> Made(JPH::Shape* shape) noexcept {
    return Result<JPH::RefConst<JPH::Shape>>::Success(JPH::RefConst<JPH::Shape>(shape));
}

Result<JPH::RefConst<JPH::Shape>> Invalid() noexcept {
    return Result<JPH::RefConst<JPH::Shape>>::Failure(ErrorCode::InvalidArgument);
}

} // namespace

Result<JPH::RefConst<JPH::Shape>> MakeJoltShape(const BodyGeometry& geometry,
                                                const Vec3d& scale) noexcept {
    const bool finite = math::IsFinite(scale.x) && math::IsFinite(scale.y) &&
                        math::IsFinite(scale.z) && math::IsFinite(geometry.halfExtent.x) &&
                        math::IsFinite(geometry.halfExtent.y) &&
                        math::IsFinite(geometry.halfExtent.z) && math::IsFinite(geometry.radius) &&
                        math::IsFinite(geometry.halfHeight);
    if (!finite) {
        return Invalid();
    }

    // 크기 0·음수는 거부하지 않고 UE처럼 clamp한다(형상 setter는 0을 허용한다 — UE도 body를 만든다,
    // PrimitiveComponent.cpp:909-925·ChaosInterfaceUtils.cpp:139-177).
    const Vec3d magnitude(SafeScaleAxis(scale.x), SafeScaleAxis(scale.y), SafeScaleAxis(scale.z));
    const double tiny = math::KindaSmallNumber<double>;
    switch (geometry.kind) {
    case BodyGeometryKind::Box: {
        const Vec3d half(math::Max(math::Abs(geometry.halfExtent.x) * magnitude.x, tiny),
                         math::Max(math::Abs(geometry.halfExtent.y) * magnitude.y, tiny),
                         math::Max(math::Abs(geometry.halfExtent.z) * magnitude.z, tiny));
        // convex radius는 Jolt 기본값(5 cm)이고 BoxShape가 가장 작은 반폭으로 줄인다.
        return Made(new JPH::BoxShape(ToJoltHalfExtents(half)));
    }

    case BodyGeometryKind::Sphere: {
        const double radius =
            math::Max(math::Abs(geometry.radius) *
                          math::Min(magnitude.x, math::Min(magnitude.y, magnitude.z)),
                      tiny);
        return Made(new JPH::SphereShape(ToJoltLength(radius)));
    }

    case BodyGeometryKind::Capsule: {
        // UE FKSphylElem(BodySetup.cpp:2167-2182): 반높이(반구 포함) × |Z|(최소 0.1), 반경 ×
        // max(|X|, |Y|)를 [0.1, 반높이], 원통 길이 2·(반높이 − 반경)(최소 0.1).
        const double halfHeight =
            math::Max(math::Abs(geometry.halfHeight) * magnitude.z, MinCapsuleDimension);
        const double radius =
            math::Clamp(math::Abs(geometry.radius) * math::Max(magnitude.x, magnitude.y),
                        MinCapsuleDimension, halfHeight);
        const double cylinderHalf = math::Max(halfHeight - radius, MinCapsuleCylinderLength * 0.5);
        // 축: Rocks +Z → Jolt +Y(A)라 형상 회전이 없다(어댑터 경계 4절).
        return Made(new JPH::CapsuleShape(ToJoltLength(cylinderHalf), ToJoltLength(radius)));
    }

    case BodyGeometryKind::None:
        break;
    }

    return Invalid();
}

JPH::ObjectLayer ObjectLayerFor(const ResolvedCollision& collision, BodyMotion motion,
                                uint32 responseSet) noexcept {
    uint32 participation = 0;
    if (CollisionEnabledHasPhysics(collision.collisionEnabled)) {
        participation |= LayerParticipationPhysics;
    }

    if (CollisionEnabledHasQuery(collision.collisionEnabled)) {
        participation |= LayerParticipationQuery;
    }

    if (CollisionEnabledHasProbe(collision.collisionEnabled)) {
        participation |= LayerParticipationProbe;
    }

    const bool simulates =
        (participation & (LayerParticipationPhysics | LayerParticipationProbe)) != 0;
    const LayerMotion group = !simulates                     ? LayerMotion::QueryOnly
                              : motion == BodyMotion::Static ? LayerMotion::Static
                                                             : LayerMotion::Moving;
    return MakeObjectLayer(static_cast<uint32>(collision.objectType), group, participation,
                           responseSet);
}

JPH::EMotionType ToJoltMotion(BodyMotion motion) noexcept {
    switch (motion) {
    case BodyMotion::Kinematic:
        return JPH::EMotionType::Kinematic;
    case BodyMotion::Dynamic:
        return JPH::EMotionType::Dynamic;
    case BodyMotion::Static:
        break;
    }

    return JPH::EMotionType::Static;
}

} // namespace rocks::physics
