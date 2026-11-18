#pragma once

#include <Base/Types.h>

namespace rocks::platform::detail {

// 종료 hook을 프로세스에서 한 번만 부른다(Fatal·OS 예외·신호 처리기가 같이 쓴다).
void RunTerminationHook(const char* message, usize bytes) noexcept;

// 종료를 부른 스레드 id를 적는다(처음 한 번만, 크래시 처리기는 flush 스레드에 넘기기 전에 부른다).
void RecordTerminatingThread(uint64 threadId) noexcept;

} // namespace rocks::platform::detail
