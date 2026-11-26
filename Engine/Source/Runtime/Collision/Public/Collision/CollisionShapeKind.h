#pragma once

#include <Base/Types.h>

namespace rocks {

// 장면 질의 형상의 종류다(Docs/Design/PhysicsModule 4.3절 — UE ECollisionShape).
enum class CollisionShapeKind : uint8 {
    // 형상 없음(line trace)이다.
    Line,
    Box,
    Sphere,
    // 축은 형상 로컬 +Z다.
    Capsule,
    // Rocks 추가(CY2 — 그 전에는 질의가 hit 없음이다). 축은 형상 로컬 +Z다.
    Cylinder
};

} // namespace rocks
