#pragma once

#include <Object/ObjectHandle.h>

namespace rocks {

// 장면 overlap 질의 결과 하나다(Docs/Design/PhysicsModule 4.5절·7.1.9절 — UE FOverlapResult). 겹친
// 컴포넌트 하나에 결과 하나다(같은 컴포넌트의 여러 형상은 합친다). item은 instanced 단계에서
// 더한다.
struct OverlapResult {
    // 겹친 컴포넌트와 그 소유 Actor다.
    ObjectHandle component;
    ObjectHandle actor;
    // 막는 겹침인지다(질의 필터의 막음 — 4.4절).
    bool blockingHit = false;
};

} // namespace rocks
