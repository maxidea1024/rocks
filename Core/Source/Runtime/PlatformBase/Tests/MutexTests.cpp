#include <Platform/Atomics.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>
#include <TestSupport/TestGate.h>

#include <PlatformBase/Private/Common/Sync/SyncTestAccess.h>

using namespace rocks;
namespace mutex_tests_file {

using namespace rocks::platform;

// parking lot판 Mutex·IntrusiveMutex·ConditionVariable·가드(Docs/Design/Synchronization 8절, SY2)를
// 본다. 순서는 시험 훅(onParked)과 단계 훅(ROCKS_SYNC_STEP)·gate로 만들고, 시한은 가짜 시계·가짜
// 시한 대기로 정한다(14.2절). sleep이 없다.
namespace {

constexpr uint32 Workers = 4;
constexpr uint32 Iterations = 50000;

// 상수 초기화되고 파괴자가 없다.
constinit Mutex globalMutex;
constinit ConditionVariable globalCondition;
static_assert(IsTriviallyDestructibleV<Mutex> && IsTriviallyDestructibleV<ConditionVariable>);

// 시험은 잠금을 다른 스레드에서 풀거나 쥔 채 다시 시도하는 등 정적 분석이 따라가지 못하는 순서를
// 일부러 만든다. 그런 호출은 이 도우미로 한다(Docs/Design/Synchronization 13.1절).
void TestUnlock(Mutex& mutex) noexcept ROCKS_NO_THREAD_SAFETY_ANALYSIS {
    mutex.Unlock();
}

bool TestTryLock(Mutex& mutex) noexcept ROCKS_NO_THREAD_SAFETY_ANALYSIS {
    return mutex.TryLock();
}

struct CounterContext {
    Mutex mutex;
    uint64 counter = 0;
};

void IncrementUnderMutex(void* raw) noexcept {
    auto& context = *static_cast<CounterContext*>(raw);
    for (uint32 index = 0; index < Iterations; ++index) {
        LockGuard lock(context.mutex);
        ++context.counter;
    }
}

struct SlotBits {
    using StateType = uint32;
    static constexpr uint32 LockedBit = 1u << 30;
    static constexpr uint32 WaitingBit = 1u << 31;
};

using SlotLock = IntrusiveMutex<SlotBits>;

struct SlotContext {
    Atomic<uint32> state{7};
    uint64 counter = 0;
};

void IncrementUnderSlotLock(void* raw) noexcept {
    auto& context = *static_cast<SlotContext*>(raw);
    for (uint32 index = 0; index < Iterations; ++index) {
        SlotLock::Lock(context.state);
        ++context.counter;
        SlotLock::Unlock(context.state);
    }
}

void UnlockFromOtherThread(void* raw) noexcept {
    TestUnlock(*static_cast<Mutex*>(raw));
}

// 단계 훅: 이 스레드가 ParkingLot.WaitBegin에 처음 오면 reached를 열고 resume을 기다린다.
#if !ROCKS_BUILD_RELEASE
constexpr uint32 FiniteTimeoutMs = 1000;

void TestLock(Mutex& mutex) noexcept ROCKS_NO_THREAD_SAFETY_ANALYSIS {
    mutex.Lock();
}

constexpr uint64 NanosecondsPerMillisecond = 1000000;

bool TestTryLockFor(Mutex& mutex, uint32 timeoutMs) noexcept ROCKS_NO_THREAD_SAFETY_ANALYSIS {
    return mutex.TryLockFor(timeoutMs);
}

test::CountingGate* parkedGate = nullptr;
// 세는 주소 범위다. 시험 밖의 대기(예: Thread::Start가 쥔 내부 잠금을 새 스레드가 기다림)는 세지
// 않는다.
const unsigned char* parkedBegin = nullptr;
const unsigned char* parkedEnd = nullptr;

void OnParked(const void* address) noexcept {
    const auto* byte = static_cast<const unsigned char*>(address);
    if (parkedGate != nullptr && byte >= parkedBegin && byte < parkedEnd) {
        parkedGate->Arrive();
    }
}

class ParkHookScope {
public:
    // watched부터 bytes byte 안의 주소에서 잠든 대기만 센다.
    ParkHookScope(test::CountingGate& gate, const void* watched, usize bytes) noexcept {
        parkedBegin = static_cast<const unsigned char*>(watched);
        parkedEnd = parkedBegin + bytes;
        parkedGate = &gate;
        platform::detail::SyncTestAccess::InstallParkHooks(OnParked, nullptr);
    }

    ~ParkHookScope() noexcept {
        platform::detail::SyncTestAccess::Clear();
        parkedGate = nullptr;
    }

    ParkHookScope(const ParkHookScope&) = delete;
    ParkHookScope& operator=(const ParkHookScope&) = delete;
};

struct LockerContext {
    Mutex* mutex = nullptr;
    test::TestGate acquired;
    test::TestGate release;
};

// 잡은 뒤 acquired를 열고 release를 기다렸다 푼다.
void LockThenHold(void* raw) noexcept {
    auto& context = *static_cast<LockerContext*>(raw);
    TestLock(*context.mutex);
    context.acquired.Open();
    context.release.Wait();
    TestUnlock(*context.mutex);
}

struct StepStop {
    test::TestGate reached;
    test::TestGate resume;
};

thread_local StepStop* threadStop = nullptr;

bool SameName(const char* left, const char* right) noexcept {
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }

    return *left == *right;
}

void StopAtWaitBegin(const char* name) noexcept {
    if (threadStop != nullptr && SameName(name, "ParkingLot.WaitBegin")) {
        StepStop* stop = threadStop;
        threadStop = nullptr;
        stop->reached.Open();
        stop->resume.Wait();
    }
}

struct StoppedLocker {
    Mutex* mutex = nullptr;
    StepStop stop;
    bool acquired = false;
};

void LockWithStop(void* raw) noexcept {
    auto& context = *static_cast<StoppedLocker*>(raw);
    threadStop = &context.stop;
    platform::detail::SyncTestAccess::InstallThisThreadStep(StopAtWaitBegin);
    TestLock(*context.mutex);
    context.acquired = true;
    TestUnlock(*context.mutex);
    platform::detail::SyncTestAccess::InstallThisThreadStep(nullptr);
}

struct ConditionContext {
    Mutex mutex;
    ConditionVariable condition;
    uint32 generation = 0;
    uint32 wokenFirst = 0;
    uint32 wokenSecond = 0;
};

struct ConditionWaiter {
    ConditionContext* context = nullptr;
    uint32 target = 0;
    uint32* woken = nullptr;
    StepStop* stop = nullptr;
};

void WaitForGeneration(void* raw) noexcept {
    auto& waiter = *static_cast<ConditionWaiter*>(raw);
    if (waiter.stop != nullptr) {
        threadStop = waiter.stop;
        platform::detail::SyncTestAccess::InstallThisThreadStep(StopAtWaitBegin);
    }

    LockGuard lock(waiter.context->mutex);
    while (waiter.context->generation < waiter.target) {
        waiter.context->condition.Wait(waiter.context->mutex);
    }

    ++*waiter.woken;
    platform::detail::SyncTestAccess::InstallThisThreadStep(nullptr);
}

// 가짜 시계와 가짜 시한 대기다. expire를 세우고 대상 신호를 Notify하면 시간 초과로 돌아간다.
Atomic<uint64> fakeNow;

uint64 FakeClock() noexcept {
    return fakeNow.Load<MemoryOrder::Acquire>();
}

struct FakeTimeout {
    test::CountingGate entered;
    Atomic<WaiterSignal*> waiting;
    Atomic<uint32> expire;
};

FakeTimeout* fakeTimeout = nullptr;

bool FakeTimedWait(WaiterSignal& signal, uint32) noexcept {
    FakeTimeout& state = *fakeTimeout;
    state.waiting.Store<MemoryOrder::Release>(&signal);
    state.entered.Arrive();
    signal.Wait();
    if (state.expire.Load<MemoryOrder::Acquire>() != 0) {
        signal.Reset();
        return false;
    }

    return true;
}

struct TimedLocker {
    Mutex* mutex = nullptr;
    bool acquired = true;
};

void TryLockWithTimeout(void* raw) noexcept {
    auto& context = *static_cast<TimedLocker*>(raw);
    context.acquired = TestTryLockFor(*context.mutex, FiniteTimeoutMs);
    if (context.acquired) {
        TestUnlock(*context.mutex);
    }
}

#endif

} // namespace

TEST(Mutex_ConstantInitializedAndExcludesWriters) {
    CHECK(!globalMutex.IsLocked());
    {
        LockGuard lock(globalMutex);
        CHECK(globalMutex.IsLocked() && !TestTryLock(globalMutex));
    }

    CHECK(TestTryLock(globalMutex));
    TestUnlock(globalMutex);
    globalCondition.NotifyAll();

    CounterContext context;
    Thread threads[Workers];
    for (Thread& thread : threads) {
        REQUIRE(thread.Start(IncrementUnderMutex, &context) == ThreadStatus::Success);
    }

    for (Thread& thread : threads) {
        REQUIRE(thread.Join() == ThreadStatus::Success);
    }

    CHECK(context.counter == static_cast<uint64>(Workers) * Iterations);
    CHECK(MutexTestAccess::State(context.mutex) == 0);
}

TEST(Mutex_IntrusiveBitsPreserveTheRestOfTheWord) {
    SlotContext context;
    Thread threads[Workers];
    for (Thread& thread : threads) {
        REQUIRE(thread.Start(IncrementUnderSlotLock, &context) == ThreadStatus::Success);
    }

    for (Thread& thread : threads) {
        REQUIRE(thread.Join() == ThreadStatus::Success);
    }

    CHECK(context.counter == static_cast<uint64>(Workers) * Iterations);
    // 잠김·대기 bit 밖의 값(7)은 그대로다.
    CHECK(context.state.Load<MemoryOrder::Relaxed>() == 7);
    CHECK(SlotLock::TryLock(context.state) && SlotLock::IsLocked(context.state));
    CHECK(!SlotLock::TryLock(context.state));
    SlotLock::Unlock(context.state);
}

TEST(Mutex_InitiallyLockedIsReleasedByAnotherThread) {
    Mutex handedOver(InitiallyLocked);
    CHECK(handedOver.IsLocked() && !TestTryLock(handedOver));
    Thread thread;
    REQUIRE(thread.Start(UnlockFromOtherThread, &handedOver) == ThreadStatus::Success);
    REQUIRE(thread.Join() == ThreadStatus::Success);
    CHECK(TestTryLock(handedOver));
    TestUnlock(handedOver);
}

TEST(Mutex_GuardsDeferTryEarlyReleaseAndUnlockScope) {
    Mutex mutex;
    {
        LockOwner owner(mutex, LockStart::Deferred);
        CHECK(!owner.OwnsLock() && !mutex.IsLocked());
        owner.Lock();
        CHECK(owner.OwnsLock() && mutex.IsLocked());
        LockOwner moved(static_cast<LockOwner<Mutex>&&>(owner));
        CHECK(!owner.OwnsLock() && moved.OwnsLock());
        {
            UnlockGuard unlocked(mutex);
            CHECK(!mutex.IsLocked());
        }

        CHECK(mutex.IsLocked());
        moved.Unlock();
        CHECK(!mutex.IsLocked());
    }

    {
        LockGuard held(mutex);
        LockOwner tried(mutex, LockStart::Try);
        CHECK(!tried.OwnsLock());
    }

    LockOwner tried(mutex, LockStart::Try);
    CHECK(tried.OwnsLock());
}

#if !ROCKS_BUILD_RELEASE
TEST(Mutex_WaitBitStaysUntilTheWokenWaiterUnlocks) {
    test::CountingGate parked;
    Mutex mutex;
    ParkHookScope hooks(parked, &mutex, sizeof(mutex));
    TestLock(mutex);
    LockerContext context;
    context.mutex = &mutex;
    Thread thread;
    REQUIRE(thread.Start(LockThenHold, &context) == ThreadStatus::Success);
    parked.WaitFor(1);
    CHECK((MutexTestAccess::State(mutex) & MutexTestAccess::WaitingBit) != 0);
    // 풀면 대기자가 깨어 잡는다. 깨운 스레드가 있었으므로 대기 bit는 남는다(UE와 같다).
    TestUnlock(mutex);
    context.acquired.Wait();
    CHECK(MutexTestAccess::State(mutex) ==
          (MutexTestAccess::LockedBit | MutexTestAccess::WaitingBit));
    // 그 대기자의 Unlock이 빈 큐를 보고서야 bit를 지운다.
    context.release.Open();
    REQUIRE(thread.Join() == ThreadStatus::Success);
    CHECK(MutexTestAccess::State(mutex) == 0);
}

TEST(Mutex_WaitBitRaceDoesNotLoseTheLateWaiter) {
    test::CountingGate parked;
    Mutex mutex;
    ParkHookScope hooks(parked, &mutex, sizeof(mutex));
    TestLock(mutex);
    // W1이 잠든다.
    LockerContext first;
    first.mutex = &mutex;
    Thread firstThread;
    REQUIRE(firstThread.Start(LockThenHold, &first) == ThreadStatus::Success);
    parked.WaitFor(1);
    // W2는 대기 bit를 본 뒤 bucket에 들어가기 직전에 멈춘다.
    StoppedLocker second;
    second.mutex = &mutex;
    Thread secondThread;
    REQUIRE(secondThread.Start(LockWithStop, &second) == ThreadStatus::Success);
    second.stop.reached.Wait();
    // 그 사이 W1을 위한 Unlock과 W1의 Unlock이 모두 끝난다(빈 큐를 보고 bit를 지운다).
    TestUnlock(mutex);
    first.acquired.Wait();
    first.release.Open();
    REQUIRE(firstThread.Join() == ThreadStatus::Success);
    // W2는 canWait에서 다시 확인하고 자지 않으므로 잠금을 얻는다(대기 표시 규칙 1).
    second.stop.resume.Open();
    REQUIRE(secondThread.Join() == ThreadStatus::Success);
    CHECK(second.acquired && MutexTestAccess::State(mutex) == 0);
}

TEST(Mutex_TryLockForTimesOutWithFakeTime) {
    test::CountingGate parked;
    Mutex mutex;
    ParkHookScope hooks(parked, &mutex, sizeof(mutex));
    FakeTimeout state;
    fakeTimeout = &state;
    fakeNow.Store<MemoryOrder::Release>(0);
    platform::detail::SyncTestAccess::InstallClock(FakeClock);
    platform::detail::SyncTestAccess::InstallTimedWait(FakeTimedWait);
    TestLock(mutex);
    CHECK(!TestTryLockFor(mutex, 0));

    // 시한 뒤: 시험이 시간을 시한 뒤로 옮기고 대기를 끝내면 false다.
    TimedLocker timed;
    timed.mutex = &mutex;
    Thread thread;
    REQUIRE(thread.Start(TryLockWithTimeout, &timed) == ThreadStatus::Success);
    state.entered.WaitFor(1);
    fakeNow.Store<MemoryOrder::Release>(2 * FiniteTimeoutMs * NanosecondsPerMillisecond);
    state.expire.Store<MemoryOrder::Release>(1);
    state.waiting.Load<MemoryOrder::Acquire>()->Notify();
    REQUIRE(thread.Join() == ThreadStatus::Success);
    CHECK(!timed.acquired);

    // 시한 전: 풀어 주면 깨어 잡는다.
    state.expire.Store<MemoryOrder::Release>(0);
    fakeNow.Store<MemoryOrder::Release>(0);
    TimedLocker woken;
    woken.mutex = &mutex;
    REQUIRE(thread.Start(TryLockWithTimeout, &woken) == ThreadStatus::Success);
    state.entered.WaitFor(2);
    TestUnlock(mutex);
    REQUIRE(thread.Join() == ThreadStatus::Success);
    CHECK(woken.acquired);
    platform::detail::SyncTestAccess::Clear();
    fakeTimeout = nullptr;
}

TEST(ConditionVariable_LateWaiterIsNotLostAfterNotifyClearsTheBit) {
    test::CountingGate parked;
    ConditionContext context;
    ParkHookScope hooks(parked, &context, sizeof(context));
    // W1이 세대 1을 기다리며 잠든다. 그 조건은 미리 Mutex 아래에서 바꿔 둔다(알림은 아직).
    ConditionWaiter first{&context, 1, &context.wokenFirst, nullptr};
    Thread firstThread;
    REQUIRE(firstThread.Start(WaitForGeneration, &first) == ThreadStatus::Success);
    parked.WaitFor(1);
    {
        LockGuard lock(context.mutex);
        context.generation = 1;
    }

    // W2는 짝 Mutex를 쥐고 세대 2를 기다리러 들어가 ParkingLot 직전에 멈춘다(Mutex를 쥔 채).
    StepStop stop;
    ConditionWaiter second{&context, 2, &context.wokenSecond, &stop};
    Thread secondThread;
    REQUIRE(secondThread.Start(WaitForGeneration, &second) == ThreadStatus::Success);
    stop.reached.Wait();
    // W1을 위한 알림은 Mutex 밖에서 낸다(WorkerScheduler 모양). 큐에 W1뿐이라 bit가 지워진다. W1은
    // 깨어 짝 Mutex를 기다리며 잠든다(W2가 쥐고 있다).
    context.condition.NotifyAll();
    parked.WaitFor(2);
    // W2가 들어가 잠든 뒤(Mutex를 풀어 W1이 끝난다) 알림 하나가 W2를 깨운다. bit는 canWait 안에서
    // 세우므로 W2가 놓치지 않는다.
    stop.resume.Open();
    parked.WaitFor(3);
    REQUIRE(firstThread.Join() == ThreadStatus::Success);
    {
        LockGuard lock(context.mutex);
        context.generation = 2;
    }

    context.condition.NotifyOne();
    REQUIRE(secondThread.Join() == ThreadStatus::Success);
    CHECK(context.wokenFirst == 1 && context.wokenSecond == 1);
}
#endif

} // namespace mutex_tests_file
