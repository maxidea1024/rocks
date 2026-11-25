#include <Platform/Sync/RealtimeWakeSignal.h>

#include "AndroidFutex.h"

namespace rocks::platform {

RealtimeWakeSignal::~RealtimeWakeSignal() noexcept = default;

bool RealtimeWakeSignal::Initialize() noexcept {
    return true;
}

// 값이 Waiting일 때만 잔다.
void RealtimeWakeSignal::WaitOs(uint32 timeoutMs) noexcept {
    detail::FutexWait(state_, WaitingState, timeoutMs);
}

void RealtimeWakeSignal::WakeOs() noexcept {
    detail::FutexWakeOne(state_);
}

} // namespace rocks::platform
