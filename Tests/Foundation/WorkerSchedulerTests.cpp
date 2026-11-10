#include <Tests/Support/Test.h>
#include <Tests/Support/NFailAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Clock.h>
#include <Task/WorkerScheduler.h>

using namespace rocks;
using namespace rocks::platform;

namespace {
using Body = Function<void(WorkerTaskContext&)>;

void CheckTerminal(const WorkerTaskHandle& handle, WorkerTaskState expected) {
    auto waited = handle.Wait(10000);
    if (!waited || waited.Value() != WaitResult::Signaled) {
        Fatal("Worker task did not reach terminal state");
    }
    auto state = handle.Status();
    REQUIRE(state);
    CHECK(state.Value().state == expected);
}

void JoinTestThread(Thread& thread) noexcept {
    if (thread.WaitForCompletion(10000) != WaitResult::Signaled ||
        thread.Join() != ThreadStatus::Success) {
        Fatal("Worker scheduler test producer failed to terminate");
    }
}

struct OpenOnExit {
    ManualResetEvent& event;

    ~OpenOnExit() noexcept {
        event.Signal();
    }
};

struct Counter {
    Mutex gate;
    uint32 calls = 0;

    void Increment() noexcept {
        LockGuard lock(gate);
        ++calls;
    }
};

struct LaunchFailure {
    uint32 calls = 0;
    uint32 nativeStarts = 0;

    static ThreadStatus Launch(void* raw, Thread& thread, Thread::Entry entry,
                               void* context) noexcept {
        auto& self = *static_cast<LaunchFailure*>(raw);
        if (++self.calls == 3) {
            return ThreadStatus::SystemError;
        }
        const auto result = thread.Start(entry, context);
        if (result == ThreadStatus::Success) {
            ++self.nativeStarts;
        }
        return result;
    }
};

struct ProducerContext {
    WorkerScheduler* scheduler;
    Allocator* allocator;
    Counter* counter;
    ManualResetEvent* launch;
    bool ok = true;
};

void SubmitMany(void* raw) noexcept {
    auto& context = *static_cast<ProducerContext*>(raw);
    CHECK(context.launch->Wait(10000) == WaitResult::Signaled);
    for (uint32 index = 0; index < 32; ++index) {
        auto body = Body::TryCreate(*context.allocator, [&context](WorkerTaskContext&) noexcept {
            context.counter->Increment();
        });
        if (!body) {
            context.ok = false;
            return;
        }
        auto handle = context.scheduler->Submit(Move(body.Value()));
        if (!handle) {
            context.ok = false;
            return;
        }
    }
}

struct CleanupContext {
    WorkerScheduler* scheduler;
    WorkerTaskHandle* parent;
    WorkerTaskHandle child;
    Body childBody;
    ManualResetEvent run;
    ManualResetEvent entered;
    ManualResetEvent release;
    bool worker = false;
    bool waitRejected = false;
    bool shutdownRejected = false;
    bool childAccepted = false;
    bool cleaned = false;
};

struct ReentrantCapture {
    explicit ReentrantCapture(CleanupContext& value) noexcept : context(&value) {
    }

    ReentrantCapture(ReentrantCapture&& other) noexcept : context(other.context) {
        other.context = nullptr;
    }

    ~ReentrantCapture() noexcept {
        if (context == nullptr) {
            return;
        }
        context->worker = context->scheduler->IsCurrentWorker();
        auto wait = context->parent->Wait(0);
        context->waitRejected = !wait && wait.Error() == ErrorCode::WrongThread;
        auto shutdown = context->scheduler->Shutdown();
        context->shutdownRejected = !shutdown && shutdown.Error() == ErrorCode::WrongThread;
        CHECK(context->scheduler->Stats().inFlight >= 1);
        auto child = context->scheduler->Submit(Move(context->childBody));
        context->childAccepted = child.IsOk();
        if (child) {
            context->child = Move(child.Value());
        }
        context->entered.Signal();
        if (context->release.Wait(10000) != WaitResult::Signaled) {
            Fatal("Capture cleanup test was not released");
        }
        context->cleaned = true;
    }

    void operator()(WorkerTaskContext&) noexcept {
        if (context->run.Wait(10000) != WaitResult::Signaled) {
            Fatal("Capture body test was not released");
        }
    }

    CleanupContext* context;
};

class PausedAllocator final : public Allocator {
public:
    explicit PausedAllocator(Allocator& source) noexcept : backing(source) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        bool pause;
        {
            LockGuard lock(gate);
            pause = armed;
            armed = false;
        }
        if (pause) {
            entered.Signal();
            if (release.Wait(10000) != WaitResult::Signaled) {
                Fatal("Reserved submission allocation was not released");
            }
        }
        return backing.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* value) noexcept override {
        return backing.Deallocate(value);
    }

    void Arm() noexcept {
        LockGuard lock(gate);
        armed = true;
    }

    Allocator& backing;
    Mutex gate;
    bool armed = false;
    ManualResetEvent entered;
    ManualResetEvent release;
};

struct ReservedSubmission {
    WorkerScheduler* scheduler;
    PausedAllocator* allocator;
    Body body;
    ErrorCode error = ErrorCode::None;
};

void SubmitReserved(void* raw) noexcept {
    auto& context = *static_cast<ReservedSubmission*>(raw);
    auto result = context.scheduler->Submit(Move(context.body));
    context.error = result.Error();
}

void ReleaseAfterAdmissionClosed(void* raw) noexcept {
    auto& context = *static_cast<ReservedSubmission*>(raw);
    const uint64 start = MonotonicNanoseconds();
    while (context.scheduler->Stats().accepting) {
        if (MonotonicNanoseconds() - start > 10000000000ULL) {
            Fatal("Shutdown did not close submission admission");
        }
        YieldThread();
    }
    context.allocator->release.Signal();
}
} // namespace

TEST(WorkerSchedulerRunsEightDistinctWorkersAndDrains) {
    SystemAllocator allocator;
    {
        Semaphore entered(0, 8);
        ManualResetEvent release;
        uint64 ids[8]{};
        WorkerScheduler scheduler(allocator);
        OpenOnExit unblock{release};
        REQUIRE(scheduler.Start({8, 32, 8}));
        CHECK(!scheduler.IsCurrentWorker());
        WorkerTaskHandle handles[8];
        for (uint32 index = 0; index < 8; ++index) {
            auto body = Body::TryCreate(allocator, [&, index](WorkerTaskContext&) noexcept {
                CHECK(scheduler.IsCurrentWorker());
                ids[index] = CurrentThreadId();
                CHECK(entered.Release());
                if (release.Wait(10000) != WaitResult::Signaled) {
                    Fatal("Eight worker barrier was not released");
                }
            });
            REQUIRE(body);
            auto result = scheduler.Submit(Move(body.Value()));
            REQUIRE(result);
            handles[index] = Move(result.Value());
        }
        for (uint32 index = 0; index < 8; ++index) {
            CHECK(entered.Wait(10000) == WaitResult::Signaled);
        }
        for (uint32 left = 0; left < 8; ++left) {
            CHECK(ids[left] != 0);
            for (uint32 right = left + 1; right < 8; ++right) {
                CHECK(ids[left] != ids[right]);
            }
        }
        release.Signal();
        REQUIRE(scheduler.Shutdown());
        for (auto& handle : handles) {
            CheckTerminal(handle, WorkerTaskState::Succeeded);
        }
        CHECK(scheduler.Stats().inFlight == 0);
        CHECK(scheduler.Stats().workerCount == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(WorkerSchedulerEightConcurrentSubmittersDoNotLoseAcceptedTasks) {
    SystemAllocator allocator;
    {
        Counter counter;
        ManualResetEvent launch;
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({8, 256, 8}));
        ProducerContext contexts[8];
        Thread producers[8];
        uint32 started = 0;
        for (; started < 8; ++started) {
            contexts[started] = {&scheduler, &allocator, &counter, &launch};
            if (producers[started].Start(SubmitMany, &contexts[started]) != ThreadStatus::Success) {
                break;
            }
        }
        launch.Signal();
        for (uint32 index = 0; index < started; ++index) {
            JoinTestThread(producers[index]);
        }
        REQUIRE(scheduler.Shutdown());
        CHECK(started == 8);
        for (uint32 index = 0; index < started; ++index) {
            CHECK(contexts[index].ok);
        }
        CHECK(counter.calls == 256);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(WorkerSchedulerDependenciesOrderFanInAndPropagateFailureWithoutCallingBody) {
    SystemAllocator allocator;
    {
        Counter counter;
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({4, 32, 8}));
        WorkerTaskHandle dependencies[2];
        for (uint32 index = 0; index < 2; ++index) {
            auto body = Body::TryCreate(allocator,
                                        [&](WorkerTaskContext&) noexcept { counter.Increment(); });
            REQUIRE(body);
            auto result = scheduler.Submit(Move(body.Value()));
            REQUIRE(result);
            dependencies[index] = Move(result.Value());
        }
        auto fanIn = Body::TryCreate(allocator, [&](WorkerTaskContext&) noexcept {
            LockGuard lock(counter.gate);
            CHECK(counter.calls == 2);
            counter.calls = 3;
        });
        REQUIRE(fanIn);
        auto joined = scheduler.Submit(Move(fanIn.Value()), {dependencies, 2});
        REQUIRE(joined);
        CheckTerminal(joined.Value(), WorkerTaskState::Succeeded);
        auto failing = Body::TryCreate(allocator, [](WorkerTaskContext& context) noexcept {
            context.Fail(ErrorCode::InvalidArgument);
        });
        REQUIRE(failing);
        auto failed = scheduler.Submit(Move(failing.Value()));
        REQUIRE(failed);
        auto blocked =
            Body::TryCreate(allocator, [&](WorkerTaskContext&) noexcept { counter.Increment(); });
        REQUIRE(blocked);
        auto propagated = scheduler.Submit(Move(blocked.Value()), {&failed.Value(), 1});
        REQUIRE(propagated);
        CheckTerminal(propagated.Value(), WorkerTaskState::Failed);
        CHECK(propagated.Value().Status().Value().error == ErrorCode::DependencyFailed);
        CHECK(failed.Value().Status().Value().error == ErrorCode::InvalidArgument);
        REQUIRE(scheduler.Shutdown());
        CHECK(counter.calls == 3);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(WorkerSchedulerPendingCancellationSkipsBodyAndRunningCancellationIsCooperative) {
    SystemAllocator allocator;
    {
        ManualResetEvent running;
        ManualResetEvent release;
        Counter calls;
        WorkerScheduler scheduler(allocator);
        OpenOnExit unblock{release};
        REQUIRE(scheduler.Start({1, 8, 4}));
        auto blocker = Body::TryCreate(allocator, [&](WorkerTaskContext&) noexcept {
            running.Signal();
            CHECK(release.Wait(10000) == WaitResult::Signaled);
        });
        REQUIRE(blocker);
        auto first = scheduler.Submit(Move(blocker.Value()));
        REQUIRE(first);
        REQUIRE(running.Wait(10000) == WaitResult::Signaled);
        auto pending =
            Body::TryCreate(allocator, [&](WorkerTaskContext&) noexcept { calls.Increment(); });
        REQUIRE(pending);
        auto second = scheduler.Submit(Move(pending.Value()));
        REQUIRE(second);
        REQUIRE(second.Value().RequestCancel());
        auto timed = second.Value().Wait(0);
        REQUIRE(timed);
        CHECK(timed.Value() == WaitResult::TimedOut);
        release.Signal();
        CheckTerminal(first.Value(), WorkerTaskState::Succeeded);
        CheckTerminal(second.Value(), WorkerTaskState::Cancelled);
        CHECK(calls.calls == 0);
        running.Reset();
        auto cooperative = Body::TryCreate(allocator, [&](WorkerTaskContext& context) noexcept {
            running.Signal();
            const uint64 start = MonotonicNanoseconds();
            while (!context.IsCancellationRequested()) {
                if (MonotonicNanoseconds() - start > 10000000000ULL) {
                    Fatal("Cooperative cancel was not requested");
                }
                YieldThread();
            }
            calls.Increment();
        });
        REQUIRE(cooperative);
        auto third = scheduler.Submit(Move(cooperative.Value()));
        REQUIRE(third);
        REQUIRE(running.Wait(10000) == WaitResult::Signaled);
        REQUIRE(scheduler.Shutdown(WorkerShutdownMode::Cancel));
        CheckTerminal(third.Value(), WorkerTaskState::Cancelled);
        CHECK(calls.calls == 1);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(WorkerSchedulerCapacityAndAllocationFailurePreserveCallerFunction) {
    SystemAllocator allocator;
    test::NFailAllocator failing(allocator);
    {
        ManualResetEvent entered;
        ManualResetEvent release;
        WorkerScheduler scheduler(failing);
        OpenOnExit unblock{release};
        REQUIRE(scheduler.Start({1, 1, 0}));
        auto firstBody = Body::TryCreate(allocator, [&](WorkerTaskContext&) noexcept {
            entered.Signal();
            CHECK(release.Wait(10000) == WaitResult::Signaled);
        });
        REQUIRE(firstBody);
        auto first = scheduler.Submit(Move(firstBody.Value()));
        REQUIRE(first);
        REQUIRE(entered.Wait(10000) == WaitResult::Signaled);
        auto body = Body::TryCreate(allocator, [](WorkerTaskContext&) noexcept {});
        REQUIRE(body);
        auto full = scheduler.Submit(Move(body.Value()));
        CHECK(!full && full.Error() == ErrorCode::CapacityExceeded);
        CHECK(static_cast<bool>(body.Value()));
        release.Signal();
        CheckTerminal(first.Value(), WorkerTaskState::Succeeded);
        failing.FailOnAttempt(1);
        auto failed = scheduler.Submit(Move(body.Value()));
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
        CHECK(static_cast<bool>(body.Value()));
        CHECK(scheduler.Stats().reservedSubmissions == 0);
        failing.DisableFailure();
        auto retry = scheduler.Submit(Move(body.Value()));
        REQUIRE(retry);
        CHECK(!body.Value());
        CheckTerminal(retry.Value(), WorkerTaskState::Succeeded);
        REQUIRE(scheduler.Shutdown());
        auto closedBody = Body::TryCreate(allocator, [](WorkerTaskContext&) noexcept {});
        REQUIRE(closedBody);
        auto closed = scheduler.Submit(Move(closedBody.Value()));
        CHECK(!closed && closed.Error() == ErrorCode::ShuttingDown);
        CHECK(static_cast<bool>(closedBody.Value()));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(WorkerSchedulerStartAllocationFailuresAndPartialNativeStartRollback) {
    SystemAllocator allocator;
    for (uint64 nth = 1; nth <= 4; ++nth) {
        {
            test::NFailAllocator failing(allocator);
            failing.FailOnAttempt(nth);
            WorkerScheduler scheduler(failing);
            auto started = scheduler.Start({4, 16, 4});
            if (nth <= 3) {
                CHECK(!started && started.Error() == ErrorCode::OutOfMemory);
            } else {
                CHECK(started);
            }
            REQUIRE(scheduler.Shutdown());
        }
        CHECK(allocator.Stats().liveAllocations == 0);
    }
    {
        LaunchFailure injection;
        WorkerScheduler scheduler(allocator);
        WorkerSchedulerConfig config{8, 16, 4, LaunchFailure::Launch, &injection};
        auto failed = scheduler.Start(config);
        CHECK(!failed && failed.Error() == ErrorCode::DependencyFailed);
        CHECK(injection.calls == 3);
        CHECK(injection.nativeStarts == 2);
        CHECK(scheduler.Stats().workerCount == 0);
        CHECK(!scheduler.Stats().accepting);
        REQUIRE(scheduler.Start({2, 8, 4}));
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(WorkerSchedulerCleanupReentryRejectsWaitAndPublishesCompletionAfterCaptureRelease) {
    SystemAllocator allocator;
    {
        WorkerTaskHandle parent;
        CleanupContext context;
        WorkerScheduler scheduler(allocator);
        context.scheduler = &scheduler;
        context.parent = &parent;
        OpenOnExit releaseBody{context.run};
        OpenOnExit releaseCleanup{context.release};
        REQUIRE(scheduler.Start({2, 8, 4}));
        auto childBody = Body::TryCreate(allocator, [](WorkerTaskContext&) noexcept {});
        REQUIRE(childBody);
        context.childBody = Move(childBody.Value());
        auto body = Body::TryCreate(allocator, ReentrantCapture(context));
        REQUIRE(body);
        auto submitted = scheduler.Submit(Move(body.Value()));
        REQUIRE(submitted);
        parent = Move(submitted.Value());
        context.run.Signal();
        REQUIRE(context.entered.Wait(10000) == WaitResult::Signaled);
        auto unfinished = parent.Wait(0);
        REQUIRE(unfinished);
        CHECK(unfinished.Value() == WaitResult::TimedOut);
        CHECK(parent.Status().Value().state == WorkerTaskState::Completing);
        context.release.Signal();
        CheckTerminal(parent, WorkerTaskState::Succeeded);
        CHECK(context.worker && context.waitRejected && context.shutdownRejected &&
              context.childAccepted && context.cleaned);
        CheckTerminal(context.child, WorkerTaskState::Succeeded);
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(WorkerSchedulerDependenciesRejectForeignDuplicateAndPreviousGenerationWithoutConsumption) {
    SystemAllocator allocator;
    {
        WorkerScheduler first(allocator);
        WorkerScheduler second(allocator);
        REQUIRE(first.Start());
        REQUIRE(second.Start());
        auto aBody = Body::TryCreate(allocator, [](WorkerTaskContext&) noexcept {});
        auto bBody = Body::TryCreate(allocator, [](WorkerTaskContext&) noexcept {});
        REQUIRE(aBody && bBody);
        auto a = first.Submit(Move(aBody.Value()));
        auto b = second.Submit(Move(bBody.Value()));
        REQUIRE(a && b);
        auto body = Body::TryCreate(allocator, [](WorkerTaskContext&) noexcept {});
        REQUIRE(body);
        auto foreign = first.Submit(Move(body.Value()), {&b.Value(), 1});
        CHECK(!foreign && foreign.Error() == ErrorCode::WrongContext);
        CHECK(static_cast<bool>(body.Value()));
        auto clone = a.Value().Clone();
        REQUIRE(clone);
        WorkerTaskHandle duplicates[2];
        duplicates[0] = Move(a.Value());
        duplicates[1] = Move(clone.Value());
        auto repeated = first.Submit(Move(body.Value()), {duplicates, 2});
        CHECK(!repeated && repeated.Error() == ErrorCode::InvalidArgument);
        WorkerTaskHandle invalid;
        auto missing = first.Submit(Move(body.Value()), {&invalid, 1});
        CHECK(!missing && missing.Error() == ErrorCode::InvalidArgument);
        REQUIRE(first.Shutdown());
        REQUIRE(first.Start());
        auto previous = first.Submit(Move(body.Value()), {duplicates, 1});
        CHECK(!previous && previous.Error() == ErrorCode::WrongContext);
        CHECK(static_cast<bool>(body.Value()));
        REQUIRE(first.Shutdown());
        REQUIRE(second.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(WorkerSchedulerHundredRestartsAndHandlesOutliveScheduler) {
    SystemAllocator allocator;
    WorkerTaskHandle retained;
    {
        Counter counter;
        WorkerScheduler scheduler(allocator);
        for (uint32 cycle = 0; cycle < 100; ++cycle) {
            REQUIRE(scheduler.Start({2, 8, 4}));
            auto body = Body::TryCreate(allocator,
                                        [&](WorkerTaskContext&) noexcept { counter.Increment(); });
            REQUIRE(body);
            auto task = scheduler.Submit(Move(body.Value()));
            REQUIRE(task);
            REQUIRE(scheduler.Shutdown());
            CheckTerminal(task.Value(), WorkerTaskState::Succeeded);
            if (cycle == 0) {
                retained = Move(task.Value());
            }
        }
        CHECK(counter.calls == 100);
    }
    CheckTerminal(retained, WorkerTaskState::Succeeded);
    auto clone = retained.Clone();
    REQUIRE(clone);
    retained.Reset();
    CHECK(clone.Value().RequestCancel());
    CheckTerminal(clone.Value(), WorkerTaskState::Succeeded);
    clone.Value().Reset();
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(WorkerSchedulerShutdownWaitsForReservedSubmissionCleanupAndPreservesRejectedBody) {
    SystemAllocator backing;
    {
        PausedAllocator allocator(backing);
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({2, 8, 4}));
        auto body = Body::TryCreate(backing, [](WorkerTaskContext&) noexcept {});
        REQUIRE(body);
        ReservedSubmission context{&scheduler, &allocator, Move(body.Value())};
        Thread submitter;
        Thread releaser;
        allocator.Arm();
        REQUIRE(submitter.Start(SubmitReserved, &context) == ThreadStatus::Success);
        if (allocator.entered.Wait(10000) != WaitResult::Signaled) {
            Fatal("Submission never reserved storage");
        }
        CHECK(scheduler.Stats().reservedSubmissions == 1);
        if (releaser.Start(ReleaseAfterAdmissionClosed, &context) != ThreadStatus::Success) {
            Fatal("Could not start reserved submission releaser");
        }
        CHECK(scheduler.Shutdown());
        JoinTestThread(submitter);
        JoinTestThread(releaser);
        CHECK(context.error == ErrorCode::ShuttingDown);
        CHECK(static_cast<bool>(context.body));
        CHECK(scheduler.Stats().reservedSubmissions == 0);
        CHECK(scheduler.Stats().inFlight == 0);
    }
    CHECK(backing.Stats().liveAllocations == 0);
}
