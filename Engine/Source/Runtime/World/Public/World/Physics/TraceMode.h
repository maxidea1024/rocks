#pragma once

#include <Base/Types.h>

namespace rocks {

// 장면 trace가 모을 결과다(Docs/Design/PhysicsModule 4.1·4.5절 — UE Test·Single·Multi).
enum class TraceMode : uint8 {
    // 막는 hit가 있는지만 본다(결과를 쓰지 않는다).
    Test,
    // 가장 가까운 막는 hit 하나다(겹침은 건너뛴다).
    First,
    // 가장 가까운 막는 hit까지의 겹침들과 그 막는 hit다(4.5절 정렬·자르기).
    All
};

} // namespace rocks
