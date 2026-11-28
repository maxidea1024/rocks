#pragma once

#include <Collision/CollisionTypes.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Vector3.h>
#include <World/Physics/BodyGeometryKind.h>

namespace rocks {

// body 형상 원소 수 상한이다(메시 단순 충돌 CollisionMaxElements와 같은 값 — 7.1.21절).
inline constexpr usize BodyGeometryMaxElements = 65535;

// body 형상 원소 하나다(UE FKShapeElem — Docs/Design/PhysicsModule 7.1.21절). 값은 scale 전 cm이고
// 컴포넌트 공간이다. scale은 Physics가 UE GetFinalScaled 규칙으로 원소마다 굽는다.
// - Box: halfExtent(원소 축별 반폭).
// - Sphere: radius.
// - Capsule: radius와 halfHeight(**반구 포함** — UE CapsuleHalfHeight, FKSphylElem은 Length/2 +
//   Radius), 축은 원소 +Z.
// - TaperedCapsule: radius(+Z 끝 — UE Radius0), radius1(−Z 끝 — UE Radius1), halfHeight((Length +
//   Radius0 + Radius1)/2 — 반구 포함), 축은 원소 +Z.
//
// 사용 예:
//   BodyShapeElement box = BodyShapeElement::Box(Vec3d(50.0, 50.0, 10.0));
//   box.center = Vec3d(0.0, 0.0, 20.0);
//   box.collisionEnabled = CollisionEnabled::QueryOnly;
struct BodyShapeElement {
    BodyGeometryKind kind = BodyGeometryKind::None;
    Vec3d center = Vec3d::Zero();
    Quatd rotation = Quatd::Identity();
    Vec3d halfExtent = Vec3d::Zero();
    double radius = 0.0;
    double radius1 = 0.0;
    double halfHeight = 0.0;
    // 원소의 충돌 켬이다(body 값과 교집합 — UE FKShapeElem::CollisionEnabled).
    CollisionEnabled collisionEnabled = CollisionEnabled::QueryAndPhysics;
    // 질량에 넣을지다(UE FKShapeElem::bContributeToMass).
    bool contributeToMass = true;

    static BodyShapeElement Box(const Vec3d& halfExtent) noexcept {
        BodyShapeElement element;
        element.kind = BodyGeometryKind::Box;
        element.halfExtent = halfExtent;
        return element;
    }

    static BodyShapeElement Sphere(double radius) noexcept {
        BodyShapeElement element;
        element.kind = BodyGeometryKind::Sphere;
        element.radius = radius;
        return element;
    }

    static BodyShapeElement Capsule(double radius, double halfHeight) noexcept {
        BodyShapeElement element;
        element.kind = BodyGeometryKind::Capsule;
        element.radius = radius;
        element.halfHeight = halfHeight;
        return element;
    }

    static BodyShapeElement TaperedCapsule(double radius0, double radius1,
                                           double halfHeight) noexcept {
        BodyShapeElement element;
        element.kind = BodyGeometryKind::TaperedCapsule;
        element.radius = radius0;
        element.radius1 = radius1;
        element.halfHeight = halfHeight;
        return element;
    }
};

} // namespace rocks
