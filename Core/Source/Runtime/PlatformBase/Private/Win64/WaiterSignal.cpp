#include <Platform/Sync/WaiterSignal.h>
#include <Platform/Fatal.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

namespace rocks::platform {

namespace {

// 상태 word의 주소다. Atomic<uint32>는 표준 배치라 객체 주소가 값 주소다.
void* StateAddress(Atomic<uint32>& state) noexcept {
    static_assert(sizeof(Atomic<uint32>) == sizeof(uint32));
    return &state;
}

} // namespace

WaiterSignal::WaiterSignal() noexcept = default;

WaiterSignal::~WaiterSignal() noexcept = default;

void WaiterSignal::Notify() noexcept {
    state_.Store<MemoryOrder::Release>(1);
    WakeByAddressSingle(StateAddress(state_));
}

// 깨우기(WakeByAddressSingle)는 주소를 키로만 쓰므로 Notify가 끝나기를 기다릴 것이 없다.
void WaiterSignal::Settle() noexcept {
}

// WaitOnAddress는 주소 값이 비교 값(0)과 다를 때 바로 돌아오고, 같으면 깨우기·시한까지 잔다.
// spurious wake가 있어 호출자가 다시 본다(Synchronization.lib, PlatformBase.Build.cs).
void WaiterSignal::WaitOnce(uint32 timeoutMs) noexcept {
    uint32 unsignaled = 0;
    if (!WaitOnAddress(StateAddress(state_), &unsignaled, sizeof(unsignaled),
                       timeoutMs == InfiniteTimeout ? INFINITE : static_cast<DWORD>(timeoutMs)) &&
        GetLastError() != ERROR_TIMEOUT) {
        Fatal("WaitOnAddress failed");
    }
}

} // namespace rocks::platform
