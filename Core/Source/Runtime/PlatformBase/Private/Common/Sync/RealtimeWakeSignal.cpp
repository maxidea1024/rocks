#include <Platform/Fatal.h>
#include <Platform/Sync/RealtimeWakeSignal.h>

#include "../WaitDeadline.h"
#include "SyncSeams.h"

namespace rocks::platform {

// 상태: Empty → Waiting은 대기자만, Waiting·Empty → Signaled는 Signal만, Signaled → Empty는
// 대기자의 소비다. 대기자는 Waiting인 동안만 OS 대기에 들어가고, 깬 뒤 늘 상태를 다시 본다.
WaitResult RealtimeWakeSignal::Wait(uint32 timeoutMs) noexcept {
    while (true) {
        uint32 state = state_.Load<MemoryOrder::Acquire>();
        if (state == SignaledState) {
            if (state_.CompareExchangeWeak<MemoryOrder::Acquire, MemoryOrder::Acquire>(
                    state, EmptyState)) {
                return WaitResult::Signaled;
            }

            continue;
        }

#if !ROCKS_BUILD_RELEASE
        if (state == WaitingState) {
            Fatal("RealtimeWakeSignal has two concurrent waiters");
        }
#endif

        if (timeoutMs == 0) {
            return WaitResult::TimedOut;
        }

        if (state_.CompareExchangeWeak<MemoryOrder::Relaxed, MemoryOrder::Relaxed>(state,
                                                                                   WaitingState)) {
            break;
        }
    }

    const detail::WaitDeadline deadline(timeoutMs);
    while (true) {
        const uint32 state = state_.Load<MemoryOrder::Acquire>();
        if (state == SignaledState) {
            // 읽기와 비우기 사이에 온 다른 Signal을 덮어쓰지 않게 RMW로 소비한다(그 Signal과도
            // 동기화된다).
            (void)state_.Exchange<MemoryOrder::Acquire>(EmptyState);
            return WaitResult::Signaled;
        }

#if !ROCKS_BUILD_RELEASE
        // Waiting을 세운 뒤 Empty가 되는 것은 다른 대기자가 신호를 소비한 경우뿐이다.
        if (state == EmptyState) {
            Fatal("RealtimeWakeSignal has two concurrent waiters");
        }
#endif

        const uint32 remaining = deadline.RemainingMilliseconds();
        if (remaining == 0) {
            // 시간 초과: Waiting을 거둔다. 그 사이 Signal이 왔으면(Signaled) 그 신호를 받는다.
            ROCKS_SYNC_STEP("RealtimeWakeSignal.TimeoutWithdraw");
            uint32 expected = WaitingState;
            if (state_.CompareExchangeStrong<MemoryOrder::Acquire, MemoryOrder::Acquire>(
                    expected, EmptyState)) {
                return WaitResult::TimedOut;
            }

            (void)state_.Exchange<MemoryOrder::Acquire>(EmptyState);
            return WaitResult::Signaled;
        }

        WaitOs(remaining);
    }
}

} // namespace rocks::platform
