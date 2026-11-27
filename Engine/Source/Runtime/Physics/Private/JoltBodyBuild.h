#pragma once

#include "JoltConversions.h"

#include <Collision/BodySettings.h>
#include <Collision/CollisionProfileTable.h>
#include <Collision/CollisionShape.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/Vector3.h>
#include <Core/Result.h>
#include <Physics/PhysicsSettings.h>
#include <World/Physics/BodyGeometry.h>
#include <World/Physics/BodyMotion.h>

// Jolt.h가 Jolt의 모든 헤더보다 먼저 와야 한다(Core.h의 설정).
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Reference.h>
#include <Jolt/Physics/Body/MotionType.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Body/MassProperties.h>
// clang-format on

namespace rocks::physics {

// UE 캡슐 반높이·반경 하한과 원통 길이 하한(cm — BodySetup.cpp:2167-2182의 0.1)이다.
inline constexpr double MinCapsuleDimension = 0.1;
inline constexpr double MinCapsuleCylinderLength = 0.1;

// UE UPhysicalMaterial 기본값이다(PhysicalMaterial.cpp:45-49 — 재질 에셋 전 모든 body, 7.1.15절):
// 마찰·반발·밀도(g/cm³)·질량 거듭제곱(RaiseMassToPower).
inline constexpr float DefaultPhysicalMaterialFriction = 0.7f;
inline constexpr float DefaultPhysicalMaterialRestitution = 0.3f;
inline constexpr double DefaultPhysicalMaterialDensity = 1.0;
inline constexpr double DefaultPhysicalMaterialRaiseMassToPower = 0.75;
// UE 질량 하한(kg — 1 g, BodyUtils.cpp:67·71)과 관성 배율을 적용하는 차이(성분, :88)다.
inline constexpr double MinBodyMassKilograms = 0.001;
inline constexpr double InertiaScaleTolerance = 1.0e-3;
// 주축 관성 하한(가장 큰 주축값에 대한 비 — 관성 배율 0 등으로 0이 된 축이 Jolt 역관성 무한대가
// 되지 않게)이다. UE Chaos는 0 관성을 잠긴 축으로 다루지만 Jolt는 역수를 내므로 아주 작은 값으로
// 둔다.
inline constexpr double MinPrincipalInertiaRatio = 1.0e-6;

// body 형상을 만드는 장면 값이다(물리 설정 — 7.1.15절).
struct BodyShapeOptions {
    // 상자 margin = min(2 × 가장 작은 반폭 × 비율, 상한 cm)(UE ChaosInterfaceUtils.cpp:156).
    double collisionMarginFraction = static_cast<double>(DefaultPhysicsCollisionMarginFraction);
    double collisionMarginMax = static_cast<double>(DefaultPhysicsCollisionMarginMax);
};

// 질량 계산 입력이다(BodySettings의 질량 칸 — UE BodyInstance 질량 수정자).
struct BodyMassValues {
    bool overrideMass = false;
    double massOverride = DefaultMassOverrideKilograms;
    double massScale = 1.0;
    // 질량 중심 이동(cm, body 공간 — scale 전)이다.
    Vec3d centerOfMassOffset = Vec3d::Zero();
    Vec3d inertiaTensorScale = Vec3d::One();
};

// BodySettings(nullptr이면 기본값)의 질량 칸이다.
BodyMassValues MassValuesOf(const BodySettings* settings) noexcept;

// 질량 칸이 유한한지다(비유한 질량·배율·관성 배율·질량 중심 이동은 body를 만들지 않는다 — Jolt
// 질량 assert·NaN 시뮬레이션을 막는다).
bool AreMassValuesValid(const BodyMassValues& mass) noexcept;

// 질량 중심 이동(cm, body 공간) × 컴포넌트 scale이다(UE MassModifierTransform — 형상 감싸기와 관성
// 보정이 같은 값을 쓴다).
Vec3d ScaledCenterOfMassOffset(const BodyMassValues& mass, const Vec3d& scale) noexcept;

// body 형상이다: shape는 body에 넣는 형상(질량 중심 이동이 있으면 OffsetCenterOfMassShape로
// 감쌈), inner는 그 안의 원래 형상(질량 계산에 쓴다).
struct BodyShape {
    JPH::RefConst<JPH::Shape> shape;
    JPH::RefConst<JPH::Shape> inner;
};

// 형상 값·월드 scale·장면 값·질량 값으로 body 형상을 만든다(MakeJoltShape + 질량 중심 이동 ×
// scale — UE MassModifierTransform). 오류: MakeJoltShape와 같다.
Result<BodyShape> MakeBodyShape(const BodyGeometry& geometry, const Vec3d& scale,
                                const BodyShapeOptions& options,
                                const BodyMassValues& mass) noexcept;

// UE 질량 규칙으로 Jolt 질량 속성을 만든다(UE BodyUtils::ApplyMassPropertiesModifiers —
// 7.1.15절): inner(재질 밀도로 만든 원래 형상)의 균일 밀도 질량 → 거듭제곱·배율(또는 덮어쓰기)·
// 하한 1 g → 관성 × 비율 → 관성 배율(Chaos ScaleInertia, 질량 미포함) → 질량 중심 이동의 축별 대각
// 보정(m·d² — UE p.ComNudgeAffectsInertia). scale은 컴포넌트 월드 scale이다.
JPH::MassProperties ComputeBodyMass(const JPH::Shape& inner, const BodyMassValues& mass,
                                    const Vec3d& scale) noexcept;

// 형상 값(scale 전 cm)과 월드 scale로 Jolt 형상을 만든다(어댑터 경계 4절 UE scale 규칙,
// PhysicsModule 7.1.3절): box 반폭 × |scale| 축별, 구 반경 × min |scale|(둘 다 최소
// KindaSmallNumber), 캡슐 반높이(반구 포함) × |Z|·반경 × max(|X|, |Y|)(반경은 [0.1, 반높이],
// 원통 길이 최소 0.1). 0인 scale 축은 KindaSmallNumber로 바꾼다 — 크기 0은 거부하지 않는다(UE처럼
// body를 만든다). 오류: 형상 없음·비유한 값(InvalidArgument).
Result<JPH::RefConst<JPH::Shape>> MakeJoltShape(const BodyGeometry& geometry, const Vec3d& scale,
                                                const BodyShapeOptions& options) noexcept;

// 질의 형상(cm)으로 Jolt 형상을 stack에 만들어 body(const JPH::Shape&)를 부르고 그 결과를 돌려준다
// (PhysicsModule 7.1.8절 — 질의마다 힙에 할당하지 않는다, Jolt SetEmbedded). UE FCollisionShape
// 그대로다: 상자는 convex radius 0(정확한 상자), 구는 반경, 캡슐은 원통 반길이 max(반높이 − 반경,
// KindaSmallNumber)(UE GetCapsuleAxisHalfLength). 상자 반폭·반경은 KindaSmallNumber 이상으로
// 올린다. 형상은 body 호출 동안만 산다. 오류(body를 부르지 않는다): Line·비유한
// 값(InvalidArgument), Cylinder(CY2 전 — Unsupported).
template <class Body>
Result<bool> WithJoltQueryShape(const CollisionShape& shape, Body&& body) noexcept {
    const bool finite = math::IsFinite(shape.halfExtent.x) && math::IsFinite(shape.halfExtent.y) &&
                        math::IsFinite(shape.halfExtent.z) && math::IsFinite(shape.radius) &&
                        math::IsFinite(shape.halfHeight);
    if (!finite) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    const double tiny = math::KindaSmallNumber<double>;
    switch (shape.kind) {
    case CollisionShapeKind::Box: {
        const Vec3d half(math::Max(math::Abs(shape.halfExtent.x), tiny),
                         math::Max(math::Abs(shape.halfExtent.y), tiny),
                         math::Max(math::Abs(shape.halfExtent.z), tiny));
        // 질의 상자는 모서리를 둥글리지 않는다(UE 질의도 정확한 상자다).
        JPH::BoxShape box(ToJoltHalfExtents(half), 0.0f);
        box.SetEmbedded();
        return body(static_cast<const JPH::Shape&>(box));
    }

    case CollisionShapeKind::Sphere: {
        JPH::SphereShape sphere(ToJoltLength(math::Max(shape.radius, tiny)));
        sphere.SetEmbedded();
        return body(static_cast<const JPH::Shape&>(sphere));
    }

    case CollisionShapeKind::Capsule: {
        const double radius = math::Max(shape.radius, tiny);
        const double cylinderHalf = math::Max(shape.halfHeight - radius, tiny);
        // 축: Rocks +Z → Jolt +Y(A)라 형상 회전이 없다.
        JPH::CapsuleShape capsule(ToJoltLength(cylinderHalf), ToJoltLength(radius));
        capsule.SetEmbedded();
        return body(static_cast<const JPH::Shape&>(capsule));
    }

    case CollisionShapeKind::Cylinder:
        return Result<bool>::Failure(ErrorCode::Unsupported);

    case CollisionShapeKind::Line:
        break;
    }

    return Result<bool>::Failure(ErrorCode::InvalidArgument);
}

// 실효 충돌과 운동 종류로 object 층을 만든다(6.1절 bit 배치, 7.1.3·7.1.6절): 채널 = object type,
// 운동 묶음 = Static·Moving(실효 충돌 켬이 시뮬레이션을 안 하면 QueryOnly), 참여 표지는 실효 충돌
// 켬에서, 응답 묶음 색인은 responseSet(ResponseSetTable::Acquire가 준 값).
JPH::ObjectLayer ObjectLayerFor(const ResolvedCollision& collision, BodyMotion motion,
                                uint32 responseSet) noexcept;

// 운동 종류를 Jolt로 바꾼다.
JPH::EMotionType ToJoltMotion(BodyMotion motion) noexcept;

} // namespace rocks::physics
