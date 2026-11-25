#include <Platform/Fatal.h>
#include <Platform/Sync/RealtimeWakeSignal.h>

#include <mach/mach.h>
#include <mach/semaphore.h>
#include <mach/task.h>

namespace rocks::platform {

namespace {

constexpr uint32 MillisecondsPerSecond = 1000;
constexpr int NanosecondsPerMillisecond = 1000000;

} // namespace

RealtimeWakeSignal::~RealtimeWakeSignal() noexcept {
    if (semaphore_ != 0) {
        semaphore_destroy(mach_task_self(), static_cast<semaphore_t>(semaphore_));
    }
}

// 신호 객체마다 Mach semaphore 하나다. semaphore_signal은 사용자 공간 잠금을 잡지 않는 커널
// 호출이다 (Docs/Design/Synchronization 9.6절). 커널 객체라 만들기가 실패할 수 있다.
bool RealtimeWakeSignal::Initialize() noexcept {
    if (semaphore_ != 0) {
        return true;
    }

    semaphore_t semaphore = 0;
    if (semaphore_create(mach_task_self(), &semaphore, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) {
        return false;
    }

    semaphore_ = static_cast<uint32>(semaphore);
    return true;
}

// semaphore는 횟수를 센다. 시간 초과한 대기자가 받지 않은 깨우기가 남으면 다음 대기가 한 번 바로
// 돌아오는데, 호출자가 상태를 다시 보므로 spurious wake일 뿐이다.
void RealtimeWakeSignal::WaitOs(uint32 timeoutMs) noexcept {
    const auto semaphore = static_cast<semaphore_t>(semaphore_);
    kern_return_t result = KERN_SUCCESS;
    if (timeoutMs == InfiniteTimeout) {
        result = semaphore_wait(semaphore);
    } else {
        const mach_timespec_t timeout{timeoutMs / MillisecondsPerSecond,
                                      static_cast<clock_res_t>(timeoutMs % MillisecondsPerSecond) *
                                          NanosecondsPerMillisecond};
        result = semaphore_timedwait(semaphore, timeout);
    }

    if (result != KERN_SUCCESS && result != KERN_OPERATION_TIMED_OUT && result != KERN_ABORTED) {
        Fatal("semaphore wait failed");
    }
}

void RealtimeWakeSignal::WakeOs() noexcept {
    if (semaphore_signal(static_cast<semaphore_t>(semaphore_)) != KERN_SUCCESS) {
        Fatal("semaphore_signal failed");
    }
}

} // namespace rocks::platform
