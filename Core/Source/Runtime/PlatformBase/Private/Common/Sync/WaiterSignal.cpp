#include <Platform/Sync/WaiterSignal.h>

#include "../WaitDeadline.h"
#include "SyncSeams.h"

namespace rocks::platform {

void WaiterSignal::Wait() noexcept {
    while (!IsNotified()) {
        WaitOnce(InfiniteTimeout);
    }

    Settle();
}

bool WaiterSignal::WaitFor(uint32 timeoutMs) noexcept {
    if (IsNotified()) {
        Settle();
        return true;
    }

    if (timeoutMs == 0) {
        return false;
    }

    if (timeoutMs == InfiniteTimeout) {
        Wait();
        return true;
    }

#if !ROCKS_BUILD_RELEASE
    // 시험의 가짜 시한 대기는 OS를 부르지 않고 시험이 정한 순서로 깨움·시간 초과를 낸다(14.2절).
    if (const detail::SyncTimedWaitFunction wait = detail::SyncTestTimedWait(); wait != nullptr) {
        return wait(*this, timeoutMs);
    }
#endif

    const detail::WaitDeadline deadline(timeoutMs);
    while (!IsNotified()) {
        const uint32 remaining = deadline.RemainingMilliseconds();
        if (remaining == 0) {
            // 시간 초과와 겹친 Notify가 끝난 뒤에 돌아간다(돌아온 뒤 파괴·재사용해도 되게).
            Settle();
            return false;
        }

        WaitOnce(remaining);
    }

    Settle();
    return true;
}

} // namespace rocks::platform
