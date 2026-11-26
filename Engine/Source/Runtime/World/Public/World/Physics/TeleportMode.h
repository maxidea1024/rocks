#pragma once

#include <Base/Types.h>

namespace rocks {

// body를 옮기는 방식이다(Docs/Design/PhysicsModule 1.3·3.3절 — UE ETeleportType).
// - None: 물리적 이동이다. Kinematic body는 다음 step에서 목표까지 속도로 움직인다(PH2c).
// - TeleportPhysics: 순간 이동이다. 속도를 바꾸지 않는다(Kinematic은 속도 0).
// - ResetPhysics: 순간 이동 + 물리 상태 초기화(단일 body는 TeleportPhysics와 같다 — UE와 같다).
enum class TeleportMode : uint8 {
    None,
    TeleportPhysics,
    ResetPhysics
};

} // namespace rocks
