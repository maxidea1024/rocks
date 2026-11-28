#pragma once

#include <Base/Types.h>

namespace rocks {

// body 형상 원소 종류다(Docs/Design/PhysicsModule 2.4절, 7.1.21절 — UE FKShapeElem 종류).
// Cylinder(CY2)·Convex(PH6c)·삼각형 메시(PH6d)는 그 단계에서 더한다.
enum class BodyGeometryKind : uint8 {
    None,
    Box,
    Sphere,
    Capsule,
    TaperedCapsule
};

} // namespace rocks
