#include <Platform/Atomics.h>
#include <Platform/Sync/ParkingLot.h>
#include <Platform/Sync/SyncHooks.h>
#include <Platform/Sync/WaiterSignal.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>
#include <TestSupport/TestGate.h>

#include <PlatformBase/Private/Common/Sync/QueueMutex.h>
#include <PlatformBase/Private/Common/Sync/SyncTestAccess.h>

using namespace rocks;
namespace parking_lot_tests_file {

using namespace rocks::platform;

// ParkingLot(Docs/Design/Synchronization 10절)과 내부 QueueMutex를 본다. 순서는 sleep이 아니라 시험
// 훅(onParked — 대기자가 큐에 들어가 잠들기 직전)과 gate로 만든다(14.2절). 시한은 가짜 시한 대기로
// 실제 시간 없이 정한다.
namespace {

constexpr uint32 FiniteTimeoutMs = 1000;
constexpr uint64 WakeTokenValue = 42;

// 시험 훅이 여는 gate다. 시험마다 새로 둔다.
test::CountingGate* parkedGate = nullptr;

void OnParked(const void*) noexcept {
    if (parkedGate != nullptr) {
        parkedGate->Arrive();
    }
}

// 시험 동안 onParked 훅을 건다.
class ParkHookScope {
public:
    explicit ParkHookScope(test::CountingGate& gate) noexcept {
        parkedGate = &gate;
        detail::SyncTestAccess::InstallParkHooks(OnParked, nullptr);
    }

    ~ParkHookScope() noexcept {
        detail::SyncTestAccess::Clear();
        parkedGate = nullptr;
    }

    ParkHookScope(const ParkHookScope&) = delete;
    ParkHookScope& operator=(const ParkHookScope&) = delete;
};

struct Waiter {
    const void* address = nullptr;
    uint32 timeoutMs = InfiniteTimeout;
    ParkingLotWaitOutcome outcome;
    bool beforeWaitCalled = false;
};

void WaitOnAddress(void* raw) noexcept {
    auto& waiter = *static_cast<Waiter*>(raw);
    waiter.outcome = ParkingLot::Wait(
        waiter.address, []() noexcept { return true; },
        [&]() noexcept { waiter.beforeWaitCalled = true; }, waiter.timeoutMs);
}

// 가짜 시한 대기: 대상 신호를 기다리고, 시험이 expire를 세우면 시간 초과로 돌아간다.
// resetOnExpire는 시험이 "시간이 지났다"를 알리려고 낸 Notify를 지운다(진짜 깨우기와 겹친 경우에는
// 지우지 않는다).
struct FakeTimeout {
    test::CountingGate entered;
    Atomic<WaiterSignal*> waiting;
    Atomic<uint32> expire;
    Atomic<uint32> resetOnExpire;
};

FakeTimeout* fakeTimeout = nullptr;

bool FakeTimedWait(WaiterSignal& signal, uint32) noexcept {
    FakeTimeout& state = *fakeTimeout;
    state.waiting.Store<MemoryOrder::Release>(&signal);
    state.entered.Arrive();
    signal.Wait();
    if (state.expire.Load<MemoryOrder::Acquire>() == 0) {
        return true;
    }

    if (state.resetOnExpire.Load<MemoryOrder::Acquire>() != 0) {
        signal.Reset();
    }

    return false;
}

Atomic<uint32> waitBegins;
Atomic<uint32> waitEnds;

void CountWaitBegin(const void*) noexcept {
    waitBegins.FetchAdd<MemoryOrder::Relaxed>(1);
}

void CountWaitEnd(const void*) noexcept {
    waitEnds.FetchAdd<MemoryOrder::Relaxed>(1);
}

// 스레드 종료 중의 대기(D31): 이 객체의 소멸자가 thread_local 파괴 중에 ParkingLot에서 잔다.
struct ExitWaitContext {
    uint8 address = 0;
    ParkingLotWaitOutcome outcome;
};

ExitWaitContext* exitWaitContext = nullptr;

struct WaitsInDestructor {
    bool armed = false;

    ~WaitsInDestructor() noexcept {
        if (armed) {
            exitWaitContext->outcome = ParkingLot::Wait(
                &exitWaitContext->address, []() noexcept { return true; }, []() noexcept {});
        }
    }
};

thread_local WaitsInDestructor waitsInDestructor;

// thread_local은 생성의 역순으로 파괴된다. 먼저 이 객체를 만들고 그 뒤 ParkingLot을 써 레코드 칸을
// 만들면, 칸이 먼저 파괴되고 이 소멸자가 임시 레코드로 잔다.
void ArmExitWaitAfterRecord(void*) noexcept {
    waitsInDestructor.armed = true;
    uint8 scratch = 0;
    (void)ParkingLot::Wait(&scratch, []() noexcept { return false; }, []() noexcept {});
}

constexpr uint32 GrowthThreads = 80;
constexpr uint32 QueueMutexWorkers = 4;
constexpr uint32 QueueMutexIterations = 50000;

struct QueueMutexContext {
    detail::QueueMutex mutex;
    uint64 counter = 0;
};

void IncrementUnderQueueMutex(void* raw) noexcept {
    auto& context = *static_cast<QueueMutexContext*>(raw);
    for (uint32 index = 0; index < QueueMutexIterations; ++index) {
        detail::QueueMutexGuard guard(context.mutex);
        ++context.counter;
    }
}

} // namespace

TEST(ParkingLot_CanWaitFalseDoesNotSleep) {
    uint8 address = 0;
    bool beforeWait = false;
    const ParkingLotWaitOutcome outcome = ParkingLot::Wait(
        &address, []() noexcept { return false; }, [&]() noexcept { beforeWait = true; });
    CHECK(!outcome.waited && !outcome.woken && outcome.wakeToken == 0 && !beforeWait);
    // 대기자가 없으면 깨우기 콜백은 "깨운 스레드 없음·남은 대기자 없음"을 받는다.
    const ParkingLotWakeOutcome wake = ParkingLot::WakeOne(&address);
    CHECK(!wake.wokeThread && !wake.mayHaveMoreWaiters);
    CHECK(ParkingLot::WakeAll(&address) == 0);
}

#if !ROCKS_BUILD_RELEASE
TEST(ParkingLot_WakeOneDeliversTokenToParkedWaiter) {
    test::CountingGate parked;
    ParkHookScope hooks(parked);
    uint8 address = 0;
    Waiter waiter;
    waiter.address = &address;
    Thread thread;
    REQUIRE(thread.Start(WaitOnAddress, &waiter) == ThreadStatus::Success);
    parked.WaitFor(1);
    ParkingLotWakeOutcome seen;
    ParkingLot::WakeOne(&address, [&](ParkingLotWakeOutcome outcome) noexcept -> uint64 {
        seen = outcome;
        return WakeTokenValue;
    });
    REQUIRE(thread.Join() == ThreadStatus::Success);
    CHECK(seen.wokeThread && !seen.mayHaveMoreWaiters);
    CHECK(waiter.outcome.waited && waiter.outcome.woken &&
          waiter.outcome.wakeToken == WakeTokenValue);
    CHECK(waiter.beforeWaitCalled);
}

TEST(ParkingLot_WakeMultipleAndAllReportRemainingWaiters) {
    test::CountingGate parked;
    ParkHookScope hooks(parked);
    uint8 address = 0;
    uint8 otherAddress = 0;
    constexpr uint32 SameAddressWaiters = 3;
    Waiter waiters[SameAddressWaiters + 1];
    Thread threads[SameAddressWaiters + 1];
    for (uint32 index = 0; index <= SameAddressWaiters; ++index) {
        waiters[index].address = index < SameAddressWaiters ? &address : &otherAddress;
        REQUIRE(threads[index].Start(WaitOnAddress, &waiters[index]) == ThreadStatus::Success);
    }

    parked.WaitFor(SameAddressWaiters + 1);
    ParkingLotWakeOutcome first;
    CHECK(ParkingLot::WakeMultiple(
              &address, 2, [&](ParkingLotWakeOutcome outcome) noexcept { first = outcome; }) == 2);
    CHECK(first.wokeThread && first.mayHaveMoreWaiters);
    // 다른 주소의 대기자는 같은 주소의 "남은 대기자"로 세지 않는다.
    ParkingLotWakeOutcome rest;
    CHECK(ParkingLot::WakeAll(
              &address, [&](ParkingLotWakeOutcome outcome) noexcept { rest = outcome; }) == 1);
    CHECK(rest.wokeThread && !rest.mayHaveMoreWaiters);
    CHECK(ParkingLot::WakeOne(&otherAddress).wokeThread);
    for (uint32 index = 0; index <= SameAddressWaiters; ++index) {
        REQUIRE(threads[index].Join() == ThreadStatus::Success);
        CHECK(waiters[index].outcome.waited && waiters[index].outcome.woken);
    }
}

TEST(ParkingLot_TimeoutLeavesQueueAndRaceWithWakeIsWoken) {
    test::CountingGate parked;
    ParkHookScope hooks(parked);
    FakeTimeout state;
    fakeTimeout = &state;
    detail::SyncTestAccess::InstallTimedWait(FakeTimedWait);
    uint8 address = 0;

    // 시한 뒤 시간 초과: 대기자는 큐에서 나오고 깨우기는 아무도 찾지 못한다.
    {
        Waiter waiter;
        waiter.address = &address;
        waiter.timeoutMs = FiniteTimeoutMs;
        Thread thread;
        REQUIRE(thread.Start(WaitOnAddress, &waiter) == ThreadStatus::Success);
        state.entered.WaitFor(1);
        state.expire.Store<MemoryOrder::Release>(1);
        state.resetOnExpire.Store<MemoryOrder::Release>(1);
        state.waiting.Load<MemoryOrder::Acquire>()->Notify();
        REQUIRE(thread.Join() == ThreadStatus::Success);
        CHECK(waiter.outcome.waited && !waiter.outcome.woken && waiter.outcome.wakeToken == 0);
        CHECK(!ParkingLot::WakeOne(&address).wokeThread);
    }

    // 시간 초과와 깨우기가 겹침: 깨우는 쪽이 이미 꺼냈으면 대기자는 그 깨우기를 받는다(woken).
    {
        state.waiting.Store<MemoryOrder::Release>(nullptr);
        state.resetOnExpire.Store<MemoryOrder::Release>(0);
        Waiter waiter;
        waiter.address = &address;
        waiter.timeoutMs = FiniteTimeoutMs;
        Thread thread;
        REQUIRE(thread.Start(WaitOnAddress, &waiter) == ThreadStatus::Success);
        parked.WaitFor(2);
        ParkingLot::WakeOne(
            &address, [](ParkingLotWakeOutcome) noexcept -> uint64 { return WakeTokenValue; });
        REQUIRE(thread.Join() == ThreadStatus::Success);
        CHECK(waiter.outcome.waited && waiter.outcome.woken &&
              waiter.outcome.wakeToken == WakeTokenValue);
    }

    detail::SyncTestAccess::Clear();
    fakeTimeout = nullptr;
}

TEST(ParkingLot_WaitDuringThreadExitUsesTemporaryRecord) {
    test::CountingGate parked;
    ParkHookScope hooks(parked);
    ExitWaitContext context;
    exitWaitContext = &context;
    Thread thread;
    REQUIRE(thread.Start(ArmExitWaitAfterRecord, nullptr) == ThreadStatus::Success);
    // 스레드 함수가 끝난 뒤 thread_local 파괴 중에 잠든다.
    parked.WaitFor(1);
    CHECK(ParkingLot::WakeOne(&context.address).wokeThread);
    REQUIRE(thread.Join() == ThreadStatus::Success);
    exitWaitContext = nullptr;
    CHECK(context.outcome.waited && context.outcome.woken);
    // 칸 레코드와 임시 레코드가 모두 영구 풀로 돌아왔다(새 스레드가 그중 하나를 다시 쓴다).
    const uint32 freeAfterExit = detail::SyncTestAccess::FreeRecordCount();
    CHECK(freeAfterExit >= 2);
    exitWaitContext = &context;
    Thread reuse;
    REQUIRE(reuse.Start(ArmExitWaitAfterRecord, nullptr) == ThreadStatus::Success);
    parked.WaitFor(2);
    CHECK(ParkingLot::WakeOne(&context.address).wokeThread);
    REQUIRE(reuse.Join() == ThreadStatus::Success);
    exitWaitContext = nullptr;
    CHECK(detail::SyncTestAccess::FreeRecordCount() == freeAfterExit);
}

TEST(ParkingLot_TableGrowthKeepsEveryWaiter) {
    test::CountingGate parked;
    ParkHookScope hooks(parked);
    static uint8 addresses[GrowthThreads] = {};
    static Waiter waiters[GrowthThreads];
    static Thread threads[GrowthThreads];
    for (uint32 index = 0; index < GrowthThreads; ++index) {
        waiters[index] = Waiter{};
        waiters[index].address = &addresses[index];
        REQUIRE(threads[index].Start(WaitOnAddress, &waiters[index]) == ThreadStatus::Success);
    }

    // 스레드가 늘며 표가 커지는 동안 잠든 대기자도 새 표로 옮겨져 각자의 주소로 깬다.
    parked.WaitFor(GrowthThreads);
    for (uint32 index = 0; index < GrowthThreads; ++index) {
        CHECK(ParkingLot::WakeOne(&addresses[index]).wokeThread);
    }

    for (uint32 index = 0; index < GrowthThreads; ++index) {
        REQUIRE(threads[index].Join() == ThreadStatus::Success);
        CHECK(waiters[index].outcome.woken);
    }
}
#endif

TEST(ParkingLot_SyncHooksSeeSlowPathOnly) {
    waitBegins.Store<MemoryOrder::Relaxed>(0);
    waitEnds.Store<MemoryOrder::Relaxed>(0);
    SyncHooks hooks;
    hooks.onWaitBegin = CountWaitBegin;
    hooks.onWaitEnd = CountWaitEnd;
    InstallSyncHooks(hooks);
    uint8 address = 0;
    (void)ParkingLot::Wait(&address, []() noexcept { return false; }, []() noexcept {});
    InstallSyncHooks(SyncHooks{});
    CHECK(waitBegins.Load<MemoryOrder::Relaxed>() == 1 &&
          waitEnds.Load<MemoryOrder::Relaxed>() == 1);
    (void)ParkingLot::Wait(&address, []() noexcept { return false; }, []() noexcept {});
    CHECK(waitBegins.Load<MemoryOrder::Relaxed>() == 1);
}

TEST(ParkingLot_QueueMutexExcludesConcurrentWriters) {
    QueueMutexContext context;
    Thread threads[QueueMutexWorkers];
    for (Thread& thread : threads) {
        REQUIRE(thread.Start(IncrementUnderQueueMutex, &context) == ThreadStatus::Success);
    }

    for (Thread& thread : threads) {
        REQUIRE(thread.Join() == ThreadStatus::Success);
    }

    CHECK(context.counter == static_cast<uint64>(QueueMutexWorkers) * QueueMutexIterations);
    CHECK(context.mutex.TryLock());
    CHECK(!context.mutex.TryLock());
    context.mutex.Unlock();
}

} // namespace parking_lot_tests_file
