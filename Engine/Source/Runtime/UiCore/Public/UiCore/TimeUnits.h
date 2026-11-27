#pragma once

#include <Base/Types.h>

namespace rocks::ui {

// 단조 시계 ns를 초로 바꾸는 배율이다(UI 애니메이션·타이머·툴팁 시계가 같이 쓴다).
inline constexpr double SecondsPerNanosecond = 1.0e-9;

} // namespace rocks::ui
