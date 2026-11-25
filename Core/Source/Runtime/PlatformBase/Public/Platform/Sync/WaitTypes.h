#pragma once

#include <Base/Types.h>

namespace rocks::platform {

// 대기 시간 단위는 millisecond다. 0은 즉시 검사이며 아래 값만 무한 대기를 뜻한다
// (Docs/Design/Synchronization 9.5절).
inline constexpr uint32 InfiniteTimeout = NumericLimits<uint32>::Max();

enum class WaitResult {
    Signaled,
    TimedOut,
    Failed
};

} // namespace rocks::platform
