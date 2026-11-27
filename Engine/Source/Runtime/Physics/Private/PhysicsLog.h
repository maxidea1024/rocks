#pragma once

#include <Core/Diagnostics/Log.h>

namespace rocks::physics {

// Physics 모듈 로그 범주다(LoggingSystem — 문구는 영어).
// - LogPhysics: 장면 생성·step 용량 초과 같은 모듈 동작.
// - LogPhysicsJolt: Jolt Trace hook으로 들어온 라이브러리 메시지.
inline constexpr LogCategory LogPhysics{"Physics"};
inline constexpr LogCategory LogPhysicsJolt{"Physics.Jolt"};

// 진단 코드다(Docs/Design/PhysicsModule 7.1.6·7.1.17절).
// - ResponseSetCapacity: 장면의 응답 묶음 칸 수가 object 층 색인 bit(21)를 넘었다. 그 body는 만들지
//   않거나(CreateBody 실패) 필터를 바꾸지 않는다(Warning).
inline constexpr char DiagResponseSetCapacity[] = "ResponseSetCapacity";
// - NonFiniteBodyInput: body 힘·충격량·속도 입력에 비유한 값이 있어 무시했다(Warning — 7.1.17절,
//   Jolt에 NaN을 넣지 않는다).
inline constexpr char DiagNonFiniteBodyInput[] = "NonFiniteBodyInput";

} // namespace rocks::physics
