#pragma once

#include <Core/Diagnostics/Log.h>

namespace rocks::physics {

// Physics 모듈 로그 범주다(LoggingSystem — 문구는 영어).
// - LogPhysics: 장면 생성·step 용량 초과 같은 모듈 동작.
// - LogPhysicsJolt: Jolt Trace hook으로 들어온 라이브러리 메시지.
inline constexpr LogCategory LogPhysics{"Physics"};
inline constexpr LogCategory LogPhysicsJolt{"Physics.Jolt"};

} // namespace rocks::physics
