#include <Platform/Fatal.h>
#include <Platform/Sync/RealtimeWakeSignal.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

namespace rocks::platform {

RealtimeWakeSignal::~RealtimeWakeSignal() noexcept = default;

bool RealtimeWakeSignal::Initialize() noexcept {
    return true;
}

// 상태 word의 주소에서 잔다(Atomic<uint32>는 표준 배치라 객체 주소가 값 주소다). 깨우기는 주소를
// 키로만 쓰므로 Signal이 시스템 호출 하나 말고 다른 것을 건드리지 않는다.
void RealtimeWakeSignal::WaitOs(uint32 timeoutMs) noexcept {
    static_assert(sizeof(Atomic<uint32>) == sizeof(uint32));
    uint32 waiting = WaitingState;
    if (!WaitOnAddress(&state_, &waiting, sizeof(waiting),
                       timeoutMs == InfiniteTimeout ? INFINITE : static_cast<DWORD>(timeoutMs)) &&
        GetLastError() != ERROR_TIMEOUT) {
        Fatal("WaitOnAddress failed");
    }
}

void RealtimeWakeSignal::WakeOs() noexcept {
    WakeByAddressSingle(&state_);
}

} // namespace rocks::platform
