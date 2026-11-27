// body 형상·층 만들기(Docs/Design/PhysicsJoltAdapter.ko.md 4절, PhysicsModule 6.1절·7.1.3절).
#include "JoltBodyBuild.h"

#include "JoltConversions.h"
#include "JoltLayers.h"

#include <Collision/CollisionTypes.h>
#include <Core/Math/MathUtility.h>
#include <DeterministicMath/DeterministicMath.h>

// clang-format off
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
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

// 형상 밀도다(재질 밀도 g/cm³ → kg/m³, 하한 수소 0.09 kg/m³ — UE BodyUtils.cpp:37-45).
constexpr float MinDensityKilogramsPerCubicMeter = 0.09f;

float MaterialDensity() noexcept {
    const float density = ToJoltDensity(DefaultPhysicalMaterialDensity);
    return density > MinDensityKilogramsPerCubicMeter ? density : MinDensityKilogramsPerCubicMeter;
}

// Rocks 축 r(0 x, 1 y, 2 z)의 Jolt 축 번호다(축 A — Rocks X → Jolt Z, Y → X, Z → Y).
constexpr int JoltAxisOf[3] = {2, 0, 1};

// 3×3 관성(Jolt 행렬)의 Rocks 축 값이다.
void InertiaToRocks(const JPH::Mat44& jolt, double (&rocks)[3][3]) noexcept {
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            rocks[row][column] = static_cast<double>(jolt(JoltAxisOf[row], JoltAxisOf[column]));
        }
    }
}

void InertiaFromRocks(const double (&rocks)[3][3], JPH::Mat44& jolt) noexcept {
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            jolt(JoltAxisOf[row], JoltAxisOf[column]) = static_cast<float>(rocks[row][column]);
        }
    }
}

} // namespace

Result<JPH::RefConst<JPH::Shape>> MakeJoltShape(const BodyGeometry& geometry, const Vec3d& scale,
                                                const BodyShapeOptions& options) noexcept {
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
        // convex radius = UE Chaos 상자 margin(7.1.15절 — 둥근 모서리, 바깥 크기는 그대로)이고
        // 가장 작은 반폭을 넘지 않는다.
        const double minHalf = math::Min(half.x, math::Min(half.y, half.z));
        const double margin = math::Min(
            math::Min(2.0 * minHalf * options.collisionMarginFraction, options.collisionMarginMax),
            minHalf);
        auto* box = new JPH::BoxShape(ToJoltHalfExtents(half), ToJoltLength(margin));
        box->SetDensity(MaterialDensity());
        return Made(box);
    }

    case BodyGeometryKind::Sphere: {
        const double radius =
            math::Max(math::Abs(geometry.radius) *
                          math::Min(magnitude.x, math::Min(magnitude.y, magnitude.z)),
                      tiny);
        auto* sphere = new JPH::SphereShape(ToJoltLength(radius));
        sphere->SetDensity(MaterialDensity());
        return Made(sphere);
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
        auto* capsule = new JPH::CapsuleShape(ToJoltLength(cylinderHalf), ToJoltLength(radius));
        capsule->SetDensity(MaterialDensity());
        return Made(capsule);
    }

    case BodyGeometryKind::None:
        break;
    }

    return Invalid();
}

BodyMassValues MassValuesOf(const BodySettings* settings) noexcept {
    BodyMassValues values;
    if (settings == nullptr) {
        return values;
    }

    values.overrideMass = settings->overrideMass;
    values.massOverride = static_cast<double>(settings->massInKgOverride);
    values.massScale = static_cast<double>(settings->massScale);
    values.centerOfMassOffset = settings->centerOfMassOffset;
    values.inertiaTensorScale = settings->inertiaTensorScale;
    return values;
}

bool AreMassValuesValid(const BodyMassValues& mass) noexcept {
    auto finite = [](const Vec3d& value) noexcept {
        return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
    };
    return math::IsFinite(mass.massOverride) && math::IsFinite(mass.massScale) &&
           finite(mass.centerOfMassOffset) && finite(mass.inertiaTensorScale);
}

Vec3d ScaledCenterOfMassOffset(const BodyMassValues& mass, const Vec3d& scale) noexcept {
    return Vec3d(mass.centerOfMassOffset.x * scale.x, mass.centerOfMassOffset.y * scale.y,
                 mass.centerOfMassOffset.z * scale.z);
}

Result<BodyShape> MakeBodyShape(const BodyGeometry& geometry, const Vec3d& scale,
                                const BodyShapeOptions& options,
                                const BodyMassValues& mass) noexcept {
    auto inner = MakeJoltShape(geometry, scale, options);
    if (!inner) {
        return Result<BodyShape>::Failure(inner.Error());
    }

    BodyShape built;
    built.inner = inner.Value();
    built.shape = inner.Value();
    // 질량 중심 이동은 컴포넌트 scale을 곱한다(UE MassModifierTransform —
    // BodyInstance.cpp:3488-3491).
    const Vec3d offset = ScaledCenterOfMassOffset(mass, scale);
    if (!(offset == Vec3d::Zero())) {
        built.shape =
            new JPH::OffsetCenterOfMassShape(inner.Value().GetPtr(), ToJoltDisplacement(offset));
    }

    return Result<BodyShape>::Success(built);
}

JPH::MassProperties ComputeBodyMass(const JPH::Shape& inner, const BodyMassValues& mass,
                                    const Vec3d& scale) noexcept {
    JPH::MassProperties properties = inner.GetMassProperties();
    const double raw = static_cast<double>(properties.mMass);
    double target = 0.0;
    if (!mass.overrideMass) {
        // 균일 밀도 질량을 거듭제곱해 표면 쪽이 무거운 물체를 흉내 낸다(UE RaiseMassToPower —
        // 결정적 Pow라 모든 플랫폼이 같은 질량이다).
        const double power = math::Clamp(DefaultPhysicalMaterialRaiseMassToPower,
                                         math::KindaSmallNumber<double>, 1.0);
        target = math::Max(mass.massScale * deterministic::Pow(raw, power), MinBodyMassKilograms);
    } else {
        target = math::Max(mass.massOverride, MinBodyMassKilograms);
    }

    const double ratio = raw > 0.0 ? target / raw : 1.0;
    properties.mMass = static_cast<float>(target);
    double inertia[3][3];
    InertiaToRocks(properties.mInertia, inertia);
    for (auto& row : inertia) {
        for (double& value : row) {
            value *= ratio;
        }
    }

    // 관성 배율(Chaos ScaleInertia, bScaleMass 거짓 — Utilities.h:537-557, Rocks 축).
    const Vec3d& factor = mass.inertiaTensorScale;
    if (math::Abs(factor.x - 1.0) > InertiaScaleTolerance ||
        math::Abs(factor.y - 1.0) > InertiaScaleTolerance ||
        math::Abs(factor.z - 1.0) > InertiaScaleTolerance) {
        const double s[3] = {math::Abs(factor.x), math::Abs(factor.y), math::Abs(factor.z)};
        const double half = 0.5 * (inertia[0][0] + inertia[1][1] + inertia[2][2]);
        double squared[3];
        for (int axis = 0; axis < 3; ++axis) {
            squared[axis] = (half - inertia[axis][axis]) * s[axis] * s[axis];
        }

        const double xy = inertia[0][1] * s[0] * s[1];
        const double xz = inertia[0][2] * s[0] * s[2];
        const double yz = inertia[1][2] * s[1] * s[2];
        inertia[0][0] = squared[1] + squared[2];
        inertia[1][1] = squared[0] + squared[2];
        inertia[2][2] = squared[0] + squared[1];
        inertia[0][1] = inertia[1][0] = xy;
        inertia[0][2] = inertia[2][0] = xz;
        inertia[1][2] = inertia[2][1] = yz;
    }

    // 질량 중심 이동의 축별 대각 보정(UE BodyUtils.cpp:101-111 — m·d², m 단위).
    const Vec3d offset = ScaledCenterOfMassOffset(mass, scale);
    const double meters[3] = {math::CentimetersToMeters(offset.x),
                              math::CentimetersToMeters(offset.y),
                              math::CentimetersToMeters(offset.z)};
    for (int axis = 0; axis < 3; ++axis) {
        inertia[axis][axis] += target * meters[axis] * meters[axis];
    }

    // 0이 된 주축(관성 배율 0 등)은 아주 작은 값으로 둔다(Jolt 역관성 무한대 방지 — 위 상수).
    const double largest = math::Max(inertia[0][0], math::Max(inertia[1][1], inertia[2][2]));
    const double floor = math::Max(largest * MinPrincipalInertiaRatio,
                                   math::KindaSmallNumber<double> * MinBodyMassKilograms);
    for (int axis = 0; axis < 3; ++axis) {
        inertia[axis][axis] = math::Max(inertia[axis][axis], floor);
    }

    InertiaFromRocks(inertia, properties.mInertia);
    return properties;
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
