#pragma once

#include <Base/Types.h>

namespace rocks::platform {

// OS/CRT 정렬 할당의 최소 경계다. bytes는 byte 수, alignment는 0이 아닌 2의 거듭제곱이다.
// 0-byte·잘못된 인자·overflow·메모리 부족은 nullptr로 반환한다.
// 실패 원인을 구분해야 하는 상위 코드는 Core Allocator의 Result 계약을 사용한다.
void* AlignedAllocate(usize bytes, usize alignment) noexcept;
// 같은 플랫폼 adapter가 반환한 살아 있는 주소만 해제한다. nullptr는 무효 연산이다.
// 이 계층에는 소유권 ledger가 없으므로 foreign pointer나 중복 해제를 검증하지 않는다.
void AlignedFree(void* address) noexcept;

} // namespace rocks::platform
