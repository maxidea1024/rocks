#pragma once

#include <Base/Types.h>

namespace rocks {

// body 형상 종류다(Docs/Design/PhysicsModule 2.4절, 7.1.3절). PH2b는 단순 형상
// 하나다(compound·메시는 PH6).
enum class BodyGeometryKind : uint8 {
    None,
    Box,
    Sphere,
    Capsule
};

} // namespace rocks
