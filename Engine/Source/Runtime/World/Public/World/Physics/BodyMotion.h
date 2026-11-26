#pragma once

#include <Base/Types.h>

namespace rocks {

// body 운동 종류다(Docs/Design/PhysicsModule 2.5절 — UE Static·Kinematic·Simulated). Mobility가
// Movable이 아니면 Static, Movable이고 simulatePhysics면 Dynamic, 그 밖은 Kinematic이다.
enum class BodyMotion : uint8 {
    Static,
    Kinematic,
    Dynamic
};

} // namespace rocks
