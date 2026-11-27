#pragma once

#include <World/Physics/TeleportMode.h>

namespace rocks {

// PrimitiveComponent::MoveComponent의 선택이다(Docs/Design/PhysicsModule 5.1절·7.1.10절 — UE
// EMoveComponentFlags·ETeleportType). 기본값은 UE MOVECOMP_NoFlags·ETeleportType::None이다.
struct MoveOptions {
    // 자기 body로 되돌려 보내지 않는다(UE MOVECOMP_SkipPhysicsMove — 물리 결과 반영이 쓴다).
    // 자식 body는 따라간다.
    bool skipPhysicsMove = false;
    // 시작 겹침에서 빠져나가는 막는 hit도 무시하지 않는다(UE MOVECOMP_NeverIgnoreBlockingOverlaps).
    bool neverIgnoreBlockingOverlaps = false;
    // 시작 겹침 막는 hit를 발행하지 않는다(UE MOVECOMP_DisableBlockingOverlapDispatch).
    bool disableBlockingOverlapDispatch = false;
    // body 이동 방식이다(3.3절 — 자기와 자식 body 모두).
    TeleportMode teleport = TeleportMode::None;
};

} // namespace rocks
