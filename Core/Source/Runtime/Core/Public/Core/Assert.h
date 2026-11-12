#pragma once

#include <Base/Types.h>

namespace rocks {
// 불변식 위반은 복구하지 않고 플랫폼의 최소 치명 오류 경로로 종료한다.
[[noreturn]] void AssertFailed(const char* expression, const char* file, uint32 line) noexcept;
} // namespace rocks

// 모든 빌드에서 expression을 한 번 평가한다. 예상 가능한 입력·할당 오류는 Result로 처리한다.
#define ROCKS_ASSERT(expression)                                                                   \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            ::rocks::AssertFailed(#expression, __FILE__, __LINE__);                                \
        }                                                                                          \
    } while (false)
