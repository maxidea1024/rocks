#include <Platform/Atomics.h>
#include <Platform/Sync/WaiterSignal.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>

#include <PlatformBase/Private/Common/Sync/SyncTestAccess.h>

using namespace rocks;
namespace waiter_signal_tests_file {

using namespace rocks::platform;

// WaiterSignal(Docs/Design/Synchronization 7.3절)과 시한 시험 seam(14.2절)을 본다. 시간에 기대지
// 않는다: 깨움은 Notify 앞의 쓰기가 보이는지로, 시한은 가짜 시계·가짜 시한 대기로 정한 순서를
// 만든다.
namespace {

constexpr uint32 FiniteTimeoutMs = 1000;

struct Handoff {
    WaiterSignal signal;
    uint64 payload = 0;
    uint64 observed = 0;
};

void WaitAndRead(void* raw) noexcept {
    auto& handoff = *static_cast<Handoff*>(raw);
    handoff.signal.Wait();
    handoff.observed = handoff.payload;
}

#if !ROCKS_BUILD_RELEASE
constexpr uint64 NanosecondsPerMillisecond = 1000000;

// 가짜 시계: 부를 때마다 시한보다 길게 흐른다. 그래서 WaitFor는 OS 대기 없이 시간 초과다.
Atomic<uint64> fakeNow;

uint64 JumpingClock() noexcept {
    return fakeNow.FetchAdd<MemoryOrder::Relaxed>(2 * FiniteTimeoutMs * NanosecondsPerMillisecond);
}

// 가짜 시한 대기: 대기에 들어갔음을 parked로 알리고 대상 신호를 무한으로 기다린다. 시험이
// expired를 세운 뒤 Notify하면 Reset하고 시간 초과(false)를 돌려준다.
struct FakeTimedWaitState {
    WaiterSignal parked;
    Atomic<uint32> expired;
    Atomic<uint32> calls;
    Atomic<uint32> lastTimeoutMs;
    Atomic<WaiterSignal*> waiting;
};

FakeTimedWaitState* fakeState = nullptr;

bool FakeTimedWait(WaiterSignal& signal, uint32 timeoutMs) noexcept {
    FakeTimedWaitState& state = *fakeState;
    state.calls.FetchAdd<MemoryOrder::Relaxed>(1);
    state.lastTimeoutMs.Store<MemoryOrder::Relaxed>(timeoutMs);
    state.waiting.Store<MemoryOrder::Release>(&signal);
    state.parked.Notify();
    signal.Wait();
    if (state.expired.Load<MemoryOrder::Acquire>() != 0) {
        signal.Reset();
        return false;
    }

    return true;
}

struct TimedWaiter {
    WaiterSignal signal;
    bool result = false;
};

void WaitWithTimeout(void* raw) noexcept {
    auto& waiter = *static_cast<TimedWaiter*>(raw);
    waiter.result = waiter.signal.WaitFor(FiniteTimeoutMs);
}
#endif

} // namespace

TEST(WaiterSignal_NotifyResetAndImmediateChecks) {
    WaiterSignal signal;
    CHECK(!signal.IsNotified());
    CHECK(!signal.WaitFor(0));
    signal.Notify();
    CHECK(signal.IsNotified());
    // 신호가 서 있으면 기다리지 않는다. Wait는 신호를 지우지 않는다(수동 리셋).
    signal.Wait();
    CHECK(signal.WaitFor(0) && signal.WaitFor(FiniteTimeoutMs) && signal.WaitFor(InfiniteTimeout));
    signal.Notify();
    signal.Reset();
    CHECK(!signal.IsNotified() && !signal.WaitFor(0));
}

TEST(WaiterSignal_NotifyPublishesPriorWritesToWaiter) {
    for (uint32 round = 0; round < 32; ++round) {
        Handoff handoff;
        Thread waiter;
        REQUIRE(waiter.Start(WaitAndRead, &handoff) == ThreadStatus::Success);
        handoff.payload = 0x1234567800000000ull + round;
        handoff.signal.Notify();
        REQUIRE(waiter.Join() == ThreadStatus::Success);
        CHECK(handoff.observed == 0x1234567800000000ull + round);
    }
}

#if !ROCKS_BUILD_RELEASE
TEST(WaiterSignal_DeadlineUsesSyncTestClock) {
    fakeNow.Store<MemoryOrder::Relaxed>(0);
    platform::detail::SyncTestAccess::InstallClock(JumpingClock);
    WaiterSignal signal;
    // 시한 계산이 가짜 시계를 쓰므로 실제로 기다리지 않고 시간 초과다.
    const bool notified = signal.WaitFor(FiniteTimeoutMs);
    platform::detail::SyncTestAccess::Clear();
    CHECK(!notified);
    CHECK(fakeNow.Load<MemoryOrder::Relaxed>() != 0);
}

TEST(WaiterSignal_FakeTimedWaitOrdersWakeAndTimeout) {
    FakeTimedWaitState state;
    fakeState = &state;
    platform::detail::SyncTestAccess::InstallTimedWait(FakeTimedWait);

    // 시한 전 깨움: 대기자가 가짜 대기에 들어간 뒤 Notify하면 true다.
    {
        TimedWaiter waiter;
        Thread thread;
        REQUIRE(thread.Start(WaitWithTimeout, &waiter) == ThreadStatus::Success);
        state.parked.Wait();
        state.parked.Reset();
        CHECK(state.waiting.Load<MemoryOrder::Acquire>() == &waiter.signal);
        waiter.signal.Notify();
        REQUIRE(thread.Join() == ThreadStatus::Success);
        CHECK(waiter.result);
    }

    // 시한 뒤 시간 초과: 시험이 시간이 지났다고 정하면 false이고 신호는 지워진다.
    {
        TimedWaiter waiter;
        Thread thread;
        REQUIRE(thread.Start(WaitWithTimeout, &waiter) == ThreadStatus::Success);
        state.parked.Wait();
        state.parked.Reset();
        state.expired.Store<MemoryOrder::Release>(1);
        waiter.signal.Notify();
        REQUIRE(thread.Join() == ThreadStatus::Success);
        CHECK(!waiter.result && !waiter.signal.IsNotified());
    }

    // 0·무한 대기와 이미 선 신호는 가짜 대기를 거치지 않는다.
    WaiterSignal immediate;
    CHECK(!immediate.WaitFor(0));
    immediate.Notify();
    CHECK(immediate.WaitFor(FiniteTimeoutMs) && immediate.WaitFor(InfiniteTimeout));
    platform::detail::SyncTestAccess::Clear();
    fakeState = nullptr;
    CHECK(state.calls.Load<MemoryOrder::Relaxed>() == 2);
    CHECK(state.lastTimeoutMs.Load<MemoryOrder::Relaxed>() == FiniteTimeoutMs);
}
#endif

} // namespace waiter_signal_tests_file
