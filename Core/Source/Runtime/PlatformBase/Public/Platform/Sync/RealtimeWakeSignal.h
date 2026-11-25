#pragma once

#include <Base/Compiler.h>
#include <Base/Types.h>
#include <Platform/Atomics.h>
#include <Platform/Sync/WaitTypes.h>

namespace rocks::platform {

// 실시간 스레드와 그 스레드를 깨우는 쪽 사이의 신호다(Docs/Design/Synchronization 9.6절, D34). 첫
// 사용처는 오디오 장치 콜백 → AudioRT 깨우기다. 장치 콜백은 OS·오디오 라이브러리가 소유한 높은
// 우선순위 스레드라 잠금을 잡으면 안 된다. 그래서 parking lot 위의 이벤트(Signal이 bucket 잠금을
// 잡을 수 있다)를 쓰지 않는다.
//
// - 대기자는 한 번에 하나다(고정 스레드는 아니다). 둘이 동시에 기다리면 계약 위반이고
//   Debug·Development에서 Fatal이다.
// - 자동 리셋이다. 연속 Signal은 신호 하나로 합쳐진다.
// - Signal은 잠금·할당이 없고, 대기자가 없으면 시스템 호출도 없다.
// - OS 수단: Windows WaitOnAddress·WakeByAddressSingle, Android futex, macOS Mach semaphore(사용자
// 공간
//   잠금을 잡지 않는 커널 호출 — Core Audio 콜백의 관례적 수단).
// - 만들기: Initialize가 OS 객체를 만든다(macOS semaphore_create는 실패할 수 있다 — false). 다른
//   플랫폼은 늘 true다. Initialize 전·실패 뒤에는 Signal·Wait를 부르지 않는다. 파괴는 모든 호출이
//   끝난 뒤 한다.
//
// 사용 예:
//   RealtimeWakeSignal wake;
//   if (!wake.Initialize()) { /* 장치를 열지 않는다 */ }
//   // 장치 콜백: wake.Signal();
//   // AudioRT:   while (running) { wake.Wait(); RenderPending(); }
class ROCKS_PLATFORMBASE_API RealtimeWakeSignal {
public:
    RealtimeWakeSignal() noexcept = default;
    ~RealtimeWakeSignal() noexcept;
    RealtimeWakeSignal(const RealtimeWakeSignal&) = delete;
    RealtimeWakeSignal& operator=(const RealtimeWakeSignal&) = delete;

    // OS 객체를 만든다. 실패하면 false다(다시 부를 수 있다). 이미 만들었으면 true다.
    bool Initialize() noexcept;

    // 신호를 세운다. 대기자가 잠들어 있을 때만 OS 깨우기를 부른다.
    void Signal() noexcept {
        if (state_.Exchange<MemoryOrder::AcquireRelease>(SignaledState) == WaitingState) {
            WakeOs();
        }
    }

    // 신호가 있으면 소비하고 바로 Signaled다. 없으면 timeoutMs(InfiniteTimeout이면 무한) 안에
    // 신호가 올 때까지 잔다. 시한은 처음 부른 때의 단조 시계 기준이고 spurious wake로 늘어나지
    // 않는다. 0은 즉시 검사다.
    WaitResult Wait(uint32 timeoutMs = InfiniteTimeout) noexcept;

private:
    static constexpr uint32 EmptyState = 0;
    static constexpr uint32 SignaledState = 1;
    static constexpr uint32 WaitingState = 2;

    // 상태가 Waiting인 동안 OS 대기로 한 번 잔다(깨우기·시한·spurious wake로 돌아온다). OS 구현이
    // 정의한다.
    void WaitOs(uint32 timeoutMs) noexcept;
    void WakeOs() noexcept;

    Atomic<uint32> state_;
    // macOS 구현의 Mach semaphore_t(mach_port_t)다(0이면 아직 만들지 않았다). 다른 플랫폼은 쓰지
    // 않는다. 공개 헤더는 OS를 가르지 않으므로 늘 둔다.
    [[maybe_unused]] uint32 semaphore_ = 0;
};

} // namespace rocks::platform
