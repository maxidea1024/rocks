#include <Platform/Atomics.h>
#include <Platform/Sync/RealtimeWakeSignal.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>
#include <TestSupport/TestGate.h>

#include <PlatformBase/Private/Common/Sync/SyncTestAccess.h>

using namespace rocks;
namespace realtime_wake_signal_tests_file {

using namespace rocks::platform;

// RealtimeWakeSignal(Docs/Design/Synchronization 9.6절, SY12)을 본다. 신호 합침·자동 리셋·즉시
// 검사, 다른 스레드의 깨움과 발행, 시한(가짜 시계로 실제 시간 없이)을 결정적으로 단언한다.
namespace {

constexpr uint32 FiniteTimeoutMs = 1000;
constexpr uint32 Rounds = 32;

struct Handoff {
    RealtimeWakeSignal wake;
    uint64 payload = 0;
    uint64 observed = 0;
    WaitResult result = WaitResult::TimedOut;
};

void WaitAndRead(void* raw) noexcept {
    auto& handoff = *static_cast<Handoff*>(raw);
    handoff.result = handoff.wake.Wait();
    handoff.observed = handoff.payload;
}

#if !ROCKS_BUILD_RELEASE
constexpr uint64 NanosecondsPerMillisecond = 1000000;

Atomic<uint64> fakeNow;

// 부를 때마다 시한보다 길게 흐른다. 그래서 유한 대기는 OS 대기 없이 시간 초과다.
uint64 JumpingClock() noexcept {
    return fakeNow.FetchAdd<MemoryOrder::Relaxed>(2 * FiniteTimeoutMs * NanosecondsPerMillisecond);
}

bool SameName(const char* left, const char* right) noexcept {
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }

    return *left == *right;
}

// 시한과 Signal이 겹치는 경로: 대기자가 Waiting을 거두기 직전(단계 지점)에 멈춘 사이 Signal이 온다.
struct TimeoutRace {
    RealtimeWakeSignal wake;
    test::TestGate reached;
    test::TestGate resume;
    WaitResult result = WaitResult::TimedOut;
};

thread_local TimeoutRace* threadRace = nullptr;

void StopBeforeWithdraw(const char* name) noexcept {
    if (threadRace != nullptr && SameName(name, "RealtimeWakeSignal.TimeoutWithdraw")) {
        TimeoutRace* race = threadRace;
        threadRace = nullptr;
        race->reached.Open();
        race->resume.Wait();
    }
}

void WaitUntilTimeoutRace(void* raw) noexcept {
    auto& race = *static_cast<TimeoutRace*>(raw);
    threadRace = &race;
    platform::detail::SyncTestAccess::InstallThisThreadStep(StopBeforeWithdraw);
    race.result = race.wake.Wait(FiniteTimeoutMs);
    platform::detail::SyncTestAccess::InstallThisThreadStep(nullptr);
}
#endif

} // namespace

TEST(RealtimeWakeSignal_SignalsMergeAndAutoReset) {
    RealtimeWakeSignal wake;
    REQUIRE(wake.Initialize());
    CHECK(wake.Initialize());
    CHECK(wake.Wait(0) == WaitResult::TimedOut);
    // 연속 Signal은 하나로 합쳐지고 Wait가 소비한다(자동 리셋).
    wake.Signal();
    wake.Signal();
    CHECK(wake.Wait(0) == WaitResult::Signaled);
    CHECK(wake.Wait(0) == WaitResult::TimedOut);
    wake.Signal();
    CHECK(wake.Wait(FiniteTimeoutMs) == WaitResult::Signaled);
    wake.Signal();
    CHECK(wake.Wait() == WaitResult::Signaled);
}

TEST(RealtimeWakeSignal_WakesWaiterAndPublishesPriorWrites) {
    for (uint32 round = 0; round < Rounds; ++round) {
        Handoff handoff;
        REQUIRE(handoff.wake.Initialize());
        Thread waiter;
        REQUIRE(waiter.Start(WaitAndRead, &handoff) == ThreadStatus::Success);
        // 대기자가 잠들기 전이든 뒤든 신호는 사라지지 않는다.
        handoff.payload = 0xABCD000000000000ull + round;
        handoff.wake.Signal();
        REQUIRE(waiter.Join() == ThreadStatus::Success);
        CHECK(handoff.result == WaitResult::Signaled);
        CHECK(handoff.observed == 0xABCD000000000000ull + round);
        CHECK(handoff.wake.Wait(0) == WaitResult::TimedOut);
    }
}

#if !ROCKS_BUILD_RELEASE
TEST(RealtimeWakeSignal_TimeoutUsesSyncClock) {
    RealtimeWakeSignal wake;
    REQUIRE(wake.Initialize());
    fakeNow.Store<MemoryOrder::Relaxed>(0);
    platform::detail::SyncTestAccess::InstallClock(JumpingClock);
    const WaitResult timedOut = wake.Wait(FiniteTimeoutMs);
    wake.Signal();
    const WaitResult signaled = wake.Wait(FiniteTimeoutMs);
    platform::detail::SyncTestAccess::Clear();
    // 시간 초과 뒤 상태는 비어 있고(대기 표시를 거뒀다) 다음 신호를 바로 받는다.
    CHECK(timedOut == WaitResult::TimedOut && signaled == WaitResult::Signaled);
}

TEST(RealtimeWakeSignal_SignalDuringTimeoutWithdrawIsReceived) {
    TimeoutRace race;
    REQUIRE(race.wake.Initialize());
    fakeNow.Store<MemoryOrder::Relaxed>(0);
    platform::detail::SyncTestAccess::InstallClock(JumpingClock);
    Thread waiter;
    REQUIRE(waiter.Start(WaitUntilTimeoutRace, &race) == ThreadStatus::Success);
    // 대기자는 시한이 지나 Waiting을 거두려는 순간 멈춰 있다. 그때 온 Signal은 잃지 않는다.
    race.reached.Wait();
    race.wake.Signal();
    race.resume.Open();
    REQUIRE(waiter.Join() == ThreadStatus::Success);
    platform::detail::SyncTestAccess::Clear();
    CHECK(race.result == WaitResult::Signaled);
    CHECK(race.wake.Wait(0) == WaitResult::TimedOut);
}
#endif

} // namespace realtime_wake_signal_tests_file
