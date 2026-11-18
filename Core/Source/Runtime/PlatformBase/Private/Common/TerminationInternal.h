#pragma once

#include <Base/Types.h>

namespace rocks::platform::detail {

// 종료 hook을 프로세스에서 한 번만 부른다(Fatal·OS 예외·신호 처리기가 같이 쓴다).
void RunTerminationHook(const char* message, usize bytes) noexcept;

} // namespace rocks::platform::detail
