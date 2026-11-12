#include <TestSupport/Test.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>

using namespace rocks;
using namespace rocks::platform;

namespace {
void CompleteAndJoin(Thread& thread) noexcept {
    if (thread.WaitForCompletion(10000) != WaitResult::Signaled) {
        Fatal("Platform test worker entry exceeded its 10 second deadline");
    }
    if (thread.Join() != ThreadStatus::Success) {
        Fatal("Platform test worker native join failed");
    }
}

struct BlockingEntry {
    ManualResetEvent entered;
    ManualResetEvent release;
    uint32 calls = 0;
};

void BlockUntilReleased(void* raw) noexcept {
    auto& context = *static_cast<BlockingEntry*>(raw);
    context.entered.Signal();
    CHECK(context.release.Wait(10000) == WaitResult::Signaled);
    ++context.calls;
}

struct SelfJoinContext {
    Thread* thread = nullptr;
    ThreadStatus result = ThreadStatus::Success;
};

void AttemptSelfJoin(void* raw) noexcept {
    auto& context = *static_cast<SelfJoinContext*>(raw);
    context.result = context.thread->Join();
}

struct ThreadLocalContext {
    ManualResetEvent destructorEntered;
    ManualResetEvent releaseDestructor;
    bool cleaned = false;
};

struct ThreadLocalProbe {
    ThreadLocalContext* context = nullptr;

    ~ThreadLocalProbe() noexcept {
        if (context != nullptr) {
            context->destructorEntered.Signal();
            CHECK(context->releaseDestructor.Wait(10000) == WaitResult::Signaled);
            context->cleaned = true;
        }
    }
};

void RegisterThreadLocalCleanup(void* raw) noexcept {
    thread_local ThreadLocalProbe probe;
    probe.context = static_cast<ThreadLocalContext*>(raw);
}

struct PredicateContext {
    Mutex mutex;
    ConditionVariable condition;
    Semaphore ready{0, 3};
    Semaphore completed{0, 3};
    uint32 permits = 0;
    uint32 consumed = 0;
};

void ConsumePredicate(void* raw) noexcept {
    auto& context = *static_cast<PredicateContext*>(raw);
    LockGuard lock(context.mutex);
    CHECK(context.ready.Release());
    const uint64 start = MonotonicNanoseconds();
    while (context.permits == 0) {
        const WaitResult wait = context.condition.Wait(context.mutex, 100);
        CHECK(wait != WaitResult::Failed);
        if (MonotonicNanoseconds() - start > 10000000000ULL) {
            Fatal("Condition predicate was not made ready");
        }
    }
    --context.permits;
    ++context.consumed;
    CHECK(context.completed.Release());
}

struct EventContext {
    ManualResetEvent event;
    Semaphore ready{0, 4};
    Mutex mutex;
    uint32 passed = 0;
};

void WaitForSharedEvent(void* raw) noexcept {
    auto& context = *static_cast<EventContext*>(raw);
    CHECK(context.ready.Release());
    CHECK(context.event.Wait(10000) == WaitResult::Signaled);
    LockGuard lock(context.mutex);
    ++context.passed;
}

struct QueueContext {
    Semaphore available{0, 1};
    Semaphore free{1, 1};
    Mutex mutex;
    uint32 value = 0;
    uint64 sum = 0;
};

void Produce(void* raw) noexcept {
    auto& context = *static_cast<QueueContext*>(raw);
    for (uint32 value = 1; value <= 500; ++value) {
        if (context.free.Wait(10000) != WaitResult::Signaled) {
            Fatal("Producer permit timed out");
        }
        {
            LockGuard lock(context.mutex);
            context.value = value;
        }
        CHECK(context.available.Release());
    }
}

void Consume(void* raw) noexcept {
    auto& context = *static_cast<QueueContext*>(raw);
    for (uint32 expected = 1; expected <= 500; ++expected) {
        if (context.available.Wait(10000) != WaitResult::Signaled) {
            Fatal("Consumer permit timed out");
        }
        {
            LockGuard lock(context.mutex);
            CHECK(context.value == expected);
            context.sum += context.value;
        }
        CHECK(context.free.Release());
    }
}
} // namespace

TEST(PlatformThreadStartCompletionTimeoutJoinAndRestart) {
    Thread thread;
    BlockingEntry context;
    CHECK(!thread.IsJoinable());
    CHECK(thread.Join() == ThreadStatus::NotJoinable);
    CHECK(thread.WaitForCompletion(0) == WaitResult::Failed);
    CHECK(thread.Start(nullptr) == ThreadStatus::InvalidArgument);
    CHECK(!thread.IsJoinable());
    REQUIRE(thread.Start(BlockUntilReleased, &context) == ThreadStatus::Success);
    CHECK(context.entered.Wait(10000) == WaitResult::Signaled);
    CHECK(thread.IsJoinable());
    CHECK(thread.WaitForCompletion(0) == WaitResult::TimedOut);
    CHECK(thread.WaitForCompletion(1) == WaitResult::TimedOut);
    CHECK(thread.IsJoinable());
    CHECK(thread.Start(BlockUntilReleased, &context) == ThreadStatus::AlreadyJoinable);
    context.release.Signal();
    CompleteAndJoin(thread);
    CHECK(!thread.IsJoinable());
    CHECK(context.calls == 1);
    CHECK(thread.Join() == ThreadStatus::NotJoinable);
    REQUIRE(thread.Start(BlockUntilReleased, &context) == ThreadStatus::Success);
    CompleteAndJoin(thread);
    CHECK(context.calls == 2);
}

TEST(PlatformThreadRejectsSelfJoinWithoutLosingOwnerJoin) {
    Thread thread;
    SelfJoinContext context{&thread};
    REQUIRE(thread.Start(AttemptSelfJoin, &context) == ThreadStatus::Success);
    CompleteAndJoin(thread);
    CHECK(context.result == ThreadStatus::SelfJoin);
    CHECK(!thread.IsJoinable());
}

TEST(PlatformThreadCompletionPrecedesTlsCleanupButJoinWaitsForCleanup) {
    ThreadLocalContext context;
    Thread thread;
    REQUIRE(thread.Start(RegisterThreadLocalCleanup, &context) == ThreadStatus::Success);
    // entry는 반환했지만 TLS destructor가 대기 중인 상태를 만든다. 공용 API의 두 종료 시점을
    // 의도적으로 분리하며, TLS cleanup 이전에 context가 사라지는 회귀를 검출한다.
    const WaitResult completion = thread.WaitForCompletion(10000);
    const WaitResult destructor = context.destructorEntered.Wait(10000);
    context.releaseDestructor.Signal();
    CompleteAndJoin(thread);
    CHECK(completion == WaitResult::Signaled);
    CHECK(destructor == WaitResult::Signaled);
    CHECK(context.cleaned);
}

TEST(PlatformConditionVariableUsesPredicateForNotifyOneAndAll) {
    PredicateContext context;
    Thread threads[3];
    usize started = 0;
    for (; started < 3; ++started) {
        if (threads[started].Start(ConsumePredicate, &context) != ThreadStatus::Success) {
            break;
        }
    }
    for (usize index = 0; index < started; ++index) {
        CHECK(context.ready.Wait(10000) == WaitResult::Signaled);
    }
    if (started != 0) {
        {
            LockGuard lock(context.mutex);
            context.permits = 1;
            context.condition.NotifyOne();
        }
        CHECK(context.completed.Wait(10000) == WaitResult::Signaled);
        CHECK(context.completed.Wait(0) == WaitResult::TimedOut);
        {
            LockGuard lock(context.mutex);
            CHECK(context.consumed == 1);
            context.permits = static_cast<uint32>(started - 1);
            context.condition.NotifyAll();
        }
    }
    for (usize index = 0; index < started; ++index) {
        CompleteAndJoin(threads[index]);
    }
    CHECK(started == 3);
    CHECK(context.consumed == started);
}

TEST(PlatformConditionTimeoutReturnsWithMutexReacquired) {
    Mutex mutex;
    ConditionVariable condition;
    // Timeout 후 LockGuard의 Unlock이 정상이어야 한다. spurious wake도 허용되는 계약이다.
    for (usize iteration = 0; iteration < 3; ++iteration) {
        LockGuard lock(mutex);
        const WaitResult result = condition.Wait(mutex, 0);
        CHECK(result == WaitResult::TimedOut || result == WaitResult::Signaled);
    }
}

TEST(PlatformManualResetEventIsStickyResettableAndWakesEveryWaiter) {
    EventContext context;
    CHECK(context.event.Wait(0) == WaitResult::TimedOut);
    CHECK(context.event.Wait(1) == WaitResult::TimedOut);
    context.event.Signal();
    CHECK(context.event.Wait(0) == WaitResult::Signaled);
    CHECK(context.event.Wait(0) == WaitResult::Signaled);
    context.event.Reset();
    CHECK(context.event.Wait(0) == WaitResult::TimedOut);
    Thread threads[4];
    usize started = 0;
    for (; started < 4; ++started) {
        if (threads[started].Start(WaitForSharedEvent, &context) != ThreadStatus::Success) {
            break;
        }
    }
    for (usize index = 0; index < started; ++index) {
        CHECK(context.ready.Wait(10000) == WaitResult::Signaled);
    }
    context.event.Signal();
    for (usize index = 0; index < started; ++index) {
        CompleteAndJoin(threads[index]);
    }
    CHECK(started == 4);
    CHECK(context.passed == started);
    CHECK(context.event.Wait(0) == WaitResult::Signaled);
    context.event.Reset();
    CHECK(context.event.Wait(0) == WaitResult::TimedOut);
    ManualResetEvent initiallySet(true);
    CHECK(initiallySet.Wait(0) == WaitResult::Signaled);
}

TEST(PlatformSemaphoreOverflowAndZeroReleaseLeaveAllPermitsUnchanged) {
    Semaphore semaphore(1, 3);
    CHECK(!semaphore.Release(0));
    CHECK(!semaphore.Release(3));
    CHECK(semaphore.Wait(0) == WaitResult::Signaled);
    CHECK(semaphore.Wait(0) == WaitResult::TimedOut);
    CHECK(semaphore.Wait(1) == WaitResult::TimedOut);
    CHECK(semaphore.Release(3));
    CHECK(!semaphore.Release());
    CHECK(!semaphore.Release(static_cast<uint32>(-1)));
    CHECK(semaphore.Wait(0) == WaitResult::Signaled);
    CHECK(semaphore.Wait(0) == WaitResult::Signaled);
    CHECK(semaphore.Wait(0) == WaitResult::Signaled);
    CHECK(semaphore.Wait(0) == WaitResult::TimedOut);
}

TEST(PlatformSemaphoreProducerConsumerPreservesEveryOrderedValue) {
    QueueContext context;
    Thread producer;
    Thread consumer;
    REQUIRE(producer.Start(Produce, &context) == ThreadStatus::Success);
    if (consumer.Start(Consume, &context) != ThreadStatus::Success) {
        Fatal("Could not start semaphore consumer while producer was active");
    }
    CompleteAndJoin(producer);
    CompleteAndJoin(consumer);
    CHECK(context.sum == 125250);
    CHECK(context.available.Wait(0) == WaitResult::TimedOut);
    CHECK(context.free.Wait(0) == WaitResult::Signaled);
    CHECK(context.free.Wait(0) == WaitResult::TimedOut);
}

TEST(PlatformThreadSleepAndYieldKeepMonotonicClockNondecreasing) {
    const uint64 before = MonotonicNanoseconds();
    SleepFor(0);
    YieldThread();
    SleepFor(1);
    CHECK(MonotonicNanoseconds() >= before);
}
