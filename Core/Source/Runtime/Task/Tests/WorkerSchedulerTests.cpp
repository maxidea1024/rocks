#include <TestSupport/Test.h>
#include <TestSupport/NFailAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Clock.h>
#include <Task/AsyncIoEvent.h>
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

// Pending 작업은 유일한 worker가 막혀 있어도 호출 스레드에서 바로 회수된다(body 미실행, 의존 작업은
// DependencyFailed). 실행 중이거나 끝난 작업은 회수하지 않는다.
TEST(WorkerSchedulerRevokeReclaimsPendingWithoutWorker) {
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
        auto follower =
            Body::TryCreate(allocator, [&](WorkerTaskContext&) noexcept { calls.Increment(); });
        REQUIRE(follower);
        auto third = scheduler.Submit(Move(follower.Value()),
                                      Span<const WorkerTaskHandle>(&second.Value(), 1));
        REQUIRE(third);
        CHECK(scheduler.Stats().inFlight == 3);

        // worker는 아직 막혀 있다. 회수는 기다리지 않고 terminal을 게시한다.
        auto revoked = second.Value().TryRevoke();
        REQUIRE(revoked);
        CHECK(revoked.Value());
        auto status = second.Value().Status();
        REQUIRE(status);
        CHECK(status.Value().state == WorkerTaskState::Cancelled);
        CHECK(status.Value().error == ErrorCode::Cancelled);
        CHECK(scheduler.Stats().inFlight == 2);
        auto again = second.Value().TryRevoke();
        CHECK(again && !again.Value());
        auto busy = first.Value().TryRevoke();
        CHECK(busy && !busy.Value());

        release.Signal();
        CheckTerminal(first.Value(), WorkerTaskState::Succeeded);
        CheckTerminal(third.Value(), WorkerTaskState::Failed);
        auto followerStatus = third.Value().Status();
        REQUIRE(followerStatus);
        CHECK(followerStatus.Value().error == ErrorCode::DependencyFailed);
        CHECK(calls.calls == 0);
        auto finished = first.Value().TryRevoke();
        CHECK(finished && !finished.Value());
        WorkerTaskHandle invalid;
        CHECK(invalid.TryRevoke().Error() == ErrorCode::StaleHandle);
        REQUIRE(scheduler.Shutdown());
        CHECK(scheduler.Stats().inFlight == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

struct TriggerContext {
    const WorkerTaskEvent* event;
    ManualResetEvent* go;
};

void TriggerFromThread(void* raw) noexcept {
    auto* context = static_cast<TriggerContext*>(raw);
    CHECK(context->go->Wait(10000) == WaitResult::Signaled);
    auto triggered = context->event->TryTrigger();
    CHECK(triggered && triggered.Value());
}

AsyncIoResult IoResult(FileCode code, usize bytes) noexcept {
    AsyncIoResult value;
    value.status.code = code;
    value.bytes = bytes;
    return value;
}

} // namespace

// 외부 이벤트: worker가 실행하지 않고, 다른 스레드의 Trigger가 의존 작업을 풀어 준다. 다시 완료하면
// false다.
TEST(WorkerSchedulerEventTriggerReleasesDependents) {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({2, 8, 4}));
        Counter calls;
        auto event = scheduler.TryCreateEvent();
        REQUIRE(event);
        CHECK(scheduler.Stats().inFlight == 1);
        auto body =
            Body::TryCreate(allocator, [&](WorkerTaskContext&) noexcept { calls.Increment(); });
        REQUIRE(body);
        auto dependent = scheduler.Submit(Move(body.Value()),
                                          Span<const WorkerTaskHandle>(&event.Value().Handle(), 1));
        REQUIRE(dependent);
        // Trigger 전에는 실행되지 않는다.
        auto early = dependent.Value().Wait(50);
        CHECK(early && early.Value() == WaitResult::TimedOut);
        CHECK(event.Value().Handle().Status().Value().state == WorkerTaskState::Pending);

        ManualResetEvent go;
        TriggerContext context{&event.Value(), &go};
        Thread trigger;
        REQUIRE(trigger.Start(TriggerFromThread, &context) == ThreadStatus::Success);
        go.Signal();
        JoinTestThread(trigger);
        CheckTerminal(event.Value().Handle(), WorkerTaskState::Succeeded);
        CheckTerminal(dependent.Value(), WorkerTaskState::Succeeded);
        CHECK(calls.calls == 1);
        auto again = event.Value().TryTrigger();
        CHECK(again && !again.Value());
        auto failLate = event.Value().TryFail(ErrorCode::Cancelled);
        CHECK(failLate && !failLate.Value());
        CHECK(scheduler.Stats().inFlight == 0);
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// Fail은 의존 작업을 DependencyFailed로, 이벤트는 Failed(error)로 끝낸다. 취소·소멸·회수도 의존
// 작업을 실패시킨다.
TEST(WorkerSchedulerEventFailAndCancelPropagate) {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({1, 8, 4}));
        Counter calls;
        auto submitAfter = [&](const WorkerTaskEvent& event) {
            auto body =
                Body::TryCreate(allocator, [&](WorkerTaskContext&) noexcept { calls.Increment(); });
            return scheduler.Submit(Move(body.Value()),
                                    Span<const WorkerTaskHandle>(&event.Handle(), 1));
        };

        auto failing = scheduler.TryCreateEvent();
        REQUIRE(failing);
        auto afterFail = submitAfter(failing.Value());
        REQUIRE(afterFail);
        auto failed = failing.Value().TryFail(ErrorCode::NotFound);
        CHECK(failed && failed.Value());
        CheckTerminal(failing.Value().Handle(), WorkerTaskState::Failed);
        CHECK(failing.Value().Handle().Status().Value().error == ErrorCode::NotFound);
        CheckTerminal(afterFail.Value(), WorkerTaskState::Failed);
        CHECK(afterFail.Value().Status().Value().error == ErrorCode::DependencyFailed);

        auto none = scheduler.TryCreateEvent();
        REQUIRE(none);
        CHECK(none.Value().TryFail(ErrorCode::None).Value());
        CHECK(none.Value().Handle().Status().Value().error == ErrorCode::InvalidArgument);

        // 취소 요청은 worker가 Cancelled로 끝낸다.
        auto cancelled = scheduler.TryCreateEvent();
        REQUIRE(cancelled);
        auto afterCancel = submitAfter(cancelled.Value());
        REQUIRE(afterCancel);
        REQUIRE(cancelled.Value().Handle().RequestCancel());
        CheckTerminal(cancelled.Value().Handle(), WorkerTaskState::Cancelled);
        CheckTerminal(afterCancel.Value(), WorkerTaskState::Failed);
        CHECK(!cancelled.Value().TryTrigger().Value());

        // Trigger 없이 소멸하면 취소된다(의존 작업이 영원히 기다리지 않는다).
        WorkerTaskHandle orphanDependent;
        {
            auto orphan = scheduler.TryCreateEvent();
            REQUIRE(orphan);
            auto dependent = submitAfter(orphan.Value());
            REQUIRE(dependent);
            orphanDependent = Move(dependent.Value());
        }
        CheckTerminal(orphanDependent, WorkerTaskState::Failed);
        CHECK(orphanDependent.Status().Value().error == ErrorCode::DependencyFailed);

        // 회수도 된다.
        auto revocable = scheduler.TryCreateEvent();
        REQUIRE(revocable);
        CHECK(revocable.Value().Handle().TryRevoke().Value());
        CHECK(!revocable.Value().TryTrigger().Value());

        CHECK(calls.calls == 0);
        WorkerTaskEvent empty;
        CHECK(empty.TryTrigger().Error() == ErrorCode::StaleHandle);
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// Drain도 완료되지 않은 이벤트를 기다리지 않고 취소한다. 이벤트는 maxInFlight 칸을 쓰고 시작
// 전·종료 뒤에는 만들 수 없다.
TEST(WorkerSchedulerEventShutdownAndCapacity) {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        CHECK(scheduler.TryCreateEvent().Error() == ErrorCode::ShuttingDown);
        REQUIRE(scheduler.Start({1, 2, 4}));
        auto first = scheduler.TryCreateEvent();
        auto second = scheduler.TryCreateEvent();
        REQUIRE(first);
        REQUIRE(second);
        CHECK(scheduler.TryCreateEvent().Error() == ErrorCode::CapacityExceeded);
        CHECK(first.Value().TryTrigger().Value());
        auto third = scheduler.TryCreateEvent();
        REQUIRE(third);
        REQUIRE(scheduler.Shutdown(WorkerShutdownMode::Drain));
        CheckTerminal(second.Value().Handle(), WorkerTaskState::Cancelled);
        CheckTerminal(third.Value().Handle(), WorkerTaskState::Cancelled);
        CHECK(!second.Value().TryTrigger().Value());
        CHECK(scheduler.TryCreateEvent().Error() == ErrorCode::ShuttingDown);
        // 이전 세대 이벤트는 새 세대 작업의 dependency로 쓸 수 없다.
        REQUIRE(scheduler.Start({1, 4, 4}));
        auto body = Body::TryCreate(allocator, [](WorkerTaskContext&) noexcept {});
        REQUIRE(body);
        CHECK(scheduler
                  .Submit(Move(body.Value()),
                          Span<const WorkerTaskHandle>(&first.Value().Handle(), 1))
                  .Error() == ErrorCode::WrongContext);
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// AsyncIo 완료 결과 → 이벤트 완료 규칙, 즉시 제출 실패, 문맥 할당 실패.
TEST(WorkerSchedulerAsyncIoEventSubmission) {
    CHECK(AsyncIoCompletionError(IoResult(FileCode::Success, 10), 10) == ErrorCode::None);
    CHECK(AsyncIoCompletionError(IoResult(FileCode::Success, 9), 10) == ErrorCode::InvalidArgument);
    CHECK(AsyncIoCompletionError(IoResult(FileCode::Success, 11), 10) ==
          ErrorCode::InvalidArgument);
    CHECK(AsyncIoCompletionError(IoResult(FileCode::Cancelled, 0), 10) == ErrorCode::Cancelled);
    CHECK(AsyncIoCompletionError(IoResult(FileCode::OutOfMemory, 0), 10) == ErrorCode::OutOfMemory);
    CHECK(AsyncIoCompletionError(IoResult(FileCode::NotFound, 0), 10) == ErrorCode::NotFound);
    CHECK(AsyncIoCompletionError(IoResult(FileCode::SystemError, 0), 10) ==
          ErrorCode::DependencyFailed);

    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({1, 8, 4}));
        AsyncIoContext io;
        REQUIRE(io.Start(AsyncIoBackend::WorkerPool, 1));
        AsyncReadFile unopened;
        uint8 buffer[16];

        // 열리지 않은 파일: 콜백 없이 즉시 실패하므로 이벤트를 그 오류로 Fail한다.
        auto event = scheduler.TryCreateEvent();
        REQUIRE(event);
        auto observed = event.Value().Handle().Clone();
        REQUIRE(observed);
        AsyncIoRequest request;
        auto submitted = TrySubmitReadEvent(unopened, request, 0, buffer, sizeof(buffer),
                                            Move(event.Value()), allocator);
        CHECK(!submitted && submitted.Error() == ErrorCode::DependencyFailed);
        CHECK(!event.Value().IsValid());
        CheckTerminal(observed.Value(), WorkerTaskState::Failed);
        CHECK(observed.Value().Status().Value().error == ErrorCode::DependencyFailed);

        // 문맥 할당 실패: 이벤트는 호출자에게 그대로 남는다.
        test::NFailAllocator failing(allocator);
        auto kept = scheduler.TryCreateEvent();
        REQUIRE(kept);
        failing.FailOnAttempt(1);
        auto oom = TrySubmitReadEvent(unopened, request, 0, buffer, sizeof(buffer),
                                      Move(kept.Value()), failing);
        CHECK(!oom && oom.Error() == ErrorCode::OutOfMemory);
        CHECK(kept.Value().IsValid());
        CHECK(kept.Value().Handle().Status().Value().state == WorkerTaskState::Pending);
        failing.DisableFailure();
        CHECK(kept.Value().TryTrigger().Value());

        WorkerTaskEvent empty;
        CHECK(
            TrySubmitReadEvent(unopened, request, 0, buffer, sizeof(buffer), Move(empty), allocator)
                .Error() == ErrorCode::StaleHandle);
        REQUIRE(io.Shutdown());
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// Drain은 실행 중인 작업이 Trigger할 이벤트를 먼저 취소하지 않는다. 더 진행할 수 없을 때만 남은
// 이벤트를 취소한다. Cancel은 바로 취소한다.
TEST(WorkerSchedulerEventDrainWaitsForInternalTrigger) {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({2, 8, 4}));
        Counter calls;
        auto event = scheduler.TryCreateEvent();
        REQUIRE(event);
        auto stale = scheduler.TryCreateEvent();
        REQUIRE(stale);
        const WorkerTaskEvent* target = &event.Value();
        ManualResetEvent running;
        auto trigger = Body::TryCreate(allocator, [&running, target](WorkerTaskContext&) noexcept {
            running.Signal();
            const uint64 start = MonotonicNanoseconds();
            while (MonotonicNanoseconds() - start < 100000000ull) {
            }
            CHECK(target->TryTrigger().Value());
        });
        REQUIRE(trigger);
        auto producer = scheduler.Submit(Move(trigger.Value()));
        REQUIRE(producer);
        auto body =
            Body::TryCreate(allocator, [&](WorkerTaskContext&) noexcept { calls.Increment(); });
        REQUIRE(body);
        auto dependent = scheduler.Submit(Move(body.Value()),
                                          Span<const WorkerTaskHandle>(&event.Value().Handle(), 1));
        REQUIRE(dependent);
        REQUIRE(running.Wait(10000) == WaitResult::Signaled);
        REQUIRE(scheduler.Shutdown(WorkerShutdownMode::Drain));
        CheckTerminal(event.Value().Handle(), WorkerTaskState::Succeeded);
        CheckTerminal(dependent.Value(), WorkerTaskState::Succeeded);
        CHECK(calls.calls == 1);
        // 아무도 완료하지 않는 이벤트는 Drain이 기다리지 않고 취소한다.
        CheckTerminal(stale.Value().Handle(), WorkerTaskState::Cancelled);

        REQUIRE(scheduler.Start({1, 8, 4}));
        auto cancelled = scheduler.TryCreateEvent();
        REQUIRE(cancelled);
        REQUIRE(scheduler.Shutdown(WorkerShutdownMode::Cancel));
        CheckTerminal(cancelled.Value().Handle(), WorkerTaskState::Cancelled);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// Pending 이벤트 위에 이동 대입하면 이전 이벤트는 취소된다. 이벤트 생성의 할당 실패는 그 오류이고
// 칸을 남기지 않는다.
TEST(WorkerSchedulerEventMoveAssignAndOutOfMemory) {
    SystemAllocator system;
    {
        test::NFailAllocator allocator(system);
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({1, 8, 4}));
        auto first = scheduler.TryCreateEvent();
        auto second = scheduler.TryCreateEvent();
        REQUIRE(first);
        REQUIRE(second);
        auto firstHandle = first.Value().Handle().Clone();
        REQUIRE(firstHandle);
        first.Value() = Move(second.Value());
        CheckTerminal(firstHandle.Value(), WorkerTaskState::Cancelled);
        CHECK(first.Value().TryTrigger().Value());

        allocator.FailOnAttempt(1);
        auto failed = scheduler.TryCreateEvent();
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
        allocator.DisableFailure();
        CHECK(scheduler.Stats().inFlight == 0);
        CHECK(scheduler.Stats().reservedSubmissions == 0);
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(system.Stats().liveAllocations == 0);
}
