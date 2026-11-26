#pragma once

#include <Base/Types.h>

namespace rocks {

// 장면 overlap 질의가 볼 결과다(Docs/Design/PhysicsModule 4.1절·7.1.9절 — UE OverlapBlockingTest·
// OverlapAnyTest·OverlapMulti).
enum class OverlapMode : uint8 {
    // 막는 겹침이 있는지만 본다(겹침은 버린다).
    Blocking,
    // 겹침이든 막음이든 하나라도 있는지 본다.
    Any,
    // 겹친 컴포넌트를 모두 모은다.
    All
};

} // namespace rocks
