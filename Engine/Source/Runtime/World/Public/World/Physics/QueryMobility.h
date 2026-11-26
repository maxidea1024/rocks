#pragma once

#include <Base/Types.h>

namespace rocks {

// 질의할 body의 운동 종류다(Docs/Design/PhysicsModule 4.2절 — UE EQueryMobilityType).
enum class QueryMobility : uint8 {
    // 모든 body다.
    Any,
    // Static body만이다(Mobility가 Movable이 아닌 컴포넌트).
    Static,
    // Static이 아닌 body다(Kinematic·Dynamic).
    Dynamic
};

} // namespace rocks
