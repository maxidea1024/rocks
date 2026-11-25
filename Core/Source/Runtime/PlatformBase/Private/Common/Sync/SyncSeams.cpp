#include "SyncSeams.h"

#include <Platform/Atomics.h>
#include <Platform/Clock.h>

namespace rocks::platform::detail {

namespace {

#if !ROCKS_BUILD_RELEASE
constinit Atomic<SyncClockFunction> testClock;
constinit Atomic<SyncTimedWaitFunction> testTimedWait;
constinit Atomic<SyncParkFunction> testOnParked;
constinit Atomic<SyncParkFunction> testOnUnparked;
thread_local SyncStepFunction threadStep = nullptr;
#endif

} // namespace

uint64 SyncMonotonicNanoseconds() noexcept {
#if !ROCKS_BUILD_RELEASE
    if (const SyncClockFunction clock = testClock.Load<MemoryOrder::Acquire>(); clock != nullptr) {
        return clock();
    }
#endif

    return MonotonicNanoseconds();
}

#if !ROCKS_BUILD_RELEASE
void SetSyncTestClock(SyncClockFunction clock) noexcept {
    testClock.Store<MemoryOrder::Release>(clock);
}

void SetSyncTestTimedWait(SyncTimedWaitFunction wait) noexcept {
    testTimedWait.Store<MemoryOrder::Release>(wait);
}

SyncTimedWaitFunction SyncTestTimedWait() noexcept {
    return testTimedWait.Load<MemoryOrder::Acquire>();
}

void SetSyncTestParkHooks(SyncParkFunction onParked, SyncParkFunction onUnparked) noexcept {
    testOnParked.Store<MemoryOrder::Release>(onParked);
    testOnUnparked.Store<MemoryOrder::Release>(onUnparked);
}

SyncParkFunction SyncTestOnParked() noexcept {
    return testOnParked.Load<MemoryOrder::Acquire>();
}

SyncParkFunction SyncTestOnUnparked() noexcept {
    return testOnUnparked.Load<MemoryOrder::Acquire>();
}

void SetThisThreadSyncStep(SyncStepFunction step) noexcept {
    threadStep = step;
}

void SyncStep(const char* name) noexcept {
    if (const SyncStepFunction step = threadStep; step != nullptr) {
        step(name);
    }
}
#endif

} // namespace rocks::platform::detail
