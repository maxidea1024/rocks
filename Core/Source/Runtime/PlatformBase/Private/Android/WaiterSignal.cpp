#include <Platform/Sync/WaiterSignal.h>

#include "AndroidFutex.h"

namespace rocks::platform {

WaiterSignal::WaiterSignal() noexcept = default;

WaiterSignal::~WaiterSignal() noexcept = default;

void WaiterSignal::Notify() noexcept {
    state_.Store<MemoryOrder::Release>(1);
    detail::FutexWakeOne(state_);
}

// futex 깨우기는 주소를 키로만 쓰므로 Notify가 끝나기를 기다릴 것이 없다.
void WaiterSignal::Settle() noexcept {
}

// 값이 0(신호 없음)일 때만 잔다.
void WaiterSignal::WaitOnce(uint32 timeoutMs) noexcept {
    detail::FutexWait(state_, 0, timeoutMs);
}

} // namespace rocks::platform
