#pragma once

#include <Collision/CollisionShapeKind.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/Vector3.h>

namespace rocks {

// 장면 질의(shape trace·overlap)의 형상이다(Docs/Design/PhysicsModule 4.3절 — UE FCollisionShape,
// PhysicsCore/Public/CollisionShape.h). 값은 cm이고 형상 로컬 좌표다(질의의 회전·위치로 놓는다).
// 기본값은 Line(형상 없음)이다.
//
// 사용 예:
//   CollisionShape probe = CollisionShape::Sphere(30.0);
//   CollisionShape body = CollisionShape::Capsule(34.0, 88.0);   // 반높이는 반구 포함(UE와 같다)
struct CollisionShape {
    CollisionShapeKind kind = CollisionShapeKind::Line;
    // Box 반폭이다.
    Vec3d halfExtent;
    // Sphere·Capsule·Cylinder 반경이다.
    double radius = 0.0;
    // Capsule 반높이(반구 포함 — UE CapsuleHalfHeight)와 Cylinder 반높이다.
    double halfHeight = 0.0;

    static CollisionShape Box(const Vec3d& halfExtent) noexcept {
        CollisionShape shape;
        shape.kind = CollisionShapeKind::Box;
        shape.halfExtent = halfExtent;
        return shape;
    }

    static CollisionShape Sphere(double radius) noexcept {
        CollisionShape shape;
        shape.kind = CollisionShapeKind::Sphere;
        shape.radius = radius;
        return shape;
    }

    static CollisionShape Capsule(double radius, double halfHeight) noexcept {
        CollisionShape shape;
        shape.kind = CollisionShapeKind::Capsule;
        shape.radius = radius;
        shape.halfHeight = halfHeight;
        return shape;
    }

    // extent로 캡슐을 만든다: 반경 = max(x, y), 반높이 = z(UE FCollisionShape::SetCapsule).
    static CollisionShape CapsuleFromExtent(const Vec3d& extent) noexcept {
        return Capsule(math::Max(extent.x, extent.y), extent.z);
    }

    static CollisionShape Cylinder(double radius, double halfHeight) noexcept {
        CollisionShape shape;
        shape.kind = CollisionShapeKind::Cylinder;
        shape.radius = radius;
        shape.halfHeight = halfHeight;
        return shape;
    }

    // 크기가 거의 0인지다(UE IsNearlyZero — CollisionShape.h:139-158): 상자는 세 반폭이 모두
    // KindaSmallNumber 이하, 구·캡슐·실린더는 반경이 KindaSmallNumber 이하(높이는 보지 않는다),
    // Line은 참이다. 질의는 이 형상을 line trace로 바꾼다.
    bool IsNearlyZero() const noexcept {
        const double tiny = math::KindaSmallNumber<double>;
        switch (kind) {
        case CollisionShapeKind::Box:
            return halfExtent.x <= tiny && halfExtent.y <= tiny && halfExtent.z <= tiny;
        case CollisionShapeKind::Sphere:
        case CollisionShapeKind::Capsule:
        case CollisionShapeKind::Cylinder:
            return radius <= tiny;
        case CollisionShapeKind::Line:
            break;
        }

        return true;
    }
};

} // namespace rocks
