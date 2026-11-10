#pragma once

namespace rocks {

// MetaCompiler 출력이 구체 타입마다 특수화한다. 수동 field offset이나 암묵 fallback은 없다.
// 생성된 Schema/Ops/Register를 사용하며 include되지 않은 타입은 불완전 타입으로 남는다.
template <class T>
struct GeneratedType;

} // namespace rocks
