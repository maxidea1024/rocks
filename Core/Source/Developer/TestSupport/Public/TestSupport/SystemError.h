#pragma once

#include <Base/Types.h>

namespace rocks::test {

// 호출 스레드의 OS 오류 값(Windows last-error, POSIX errno)을 쓰고 읽는다. "이 호출은 OS 오류 값을
// 바꾸지 않는다"는 계약을 플랫폼 분기 없이 시험할 때 쓴다.
//
// 사용 예:
//   test::SetSystemError(Marker);
//   (void)slot.Get();
//   CHECK(test::SystemError() == Marker);
void SetSystemError(uint32 value) noexcept;
uint32 SystemError() noexcept;

} // namespace rocks::test
