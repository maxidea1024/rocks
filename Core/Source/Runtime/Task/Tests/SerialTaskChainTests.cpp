#include <TestSupport/Test.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Task/SerialTaskChain.h>

using namespace rocks;
using namespace rocks::platform;

namespace {

using Body = Function<void(WorkerTaskContext&)>;

void JoinTestThread(Thread& thread) noexcept {
    if (thread.WaitForCompletion(10000) != WaitResult::Signaled ||
        thread.Join() != ThreadStatus::Success) {
        Fatal("Serial chain test thread failed to terminate");
    }
}

WorkerTaskState WaitState(const WorkerTaskHandle& handle) noexcept {
    auto waited = handle.Wait(10000);
    if (!waited || waited.Value() != WaitResult::Signaled) {
        Fatal("Serial chain task did not finish");
    }

    return handle.Status().Value().state;
}

WorkerTaskState StateNow(const WorkerTaskHandle& handle) noexcept {
    return handle.Status().Value().state;
}

// 앞 작업(predecessorDone이 1이 되기 전)과 겹쳐 body가 실행되거나 capture가 정리되면 early를 센다.
// 취소·회수·선행 실패로 body 없이 끝나는 작업도 capture 정리는 순서 전용 선행 뒤여야 한다.
struct OrderedCapture {
    OrderedCapture(uint32& done, uint32& earlyCount, uint32& runCount) noexcept
        : predecessorDone(&done), early(&earlyCount), runs(&runCount) {
    }

    OrderedCapture(OrderedCapture&& other) noexcept
        : predecessorDone(other.predecessorDone), early(other.early), runs(other.runs) {
        other.predecessorDone = nullptr;
    }

    ~OrderedCapture() noexcept {
        if (predecessorDone != nullptr && AtomicLoad(predecessorDone) == 0) {
            AtomicFetchAdd(early, 1u);
        }
    }

    void operator()(WorkerTaskContext&) noexcept {
        if (AtomicLoad(predecessorDone) == 0) {
            AtomicFetchAdd(early, 1u);
        }

        AtomicFetchAdd(runs, 1u);
    }

    uint32* predecessorDone;
    uint32* early;
    uint32* runs;
};

Result<WorkerTaskHandle> SubmitOrdered(SerialTaskChain& chain, Allocator& allocator,
                                       OrderedCapture&& capture,
                                       Span<const WorkerTaskHandle> dependencies = {}) noexcept {
    auto body = Body::TryCreate(allocator, Move(capture));
    if (!body) {
        return Result<WorkerTaskHandle>::Failure(body.Error());
    }

    return chain.Submit(Move(body.Value()), dependencies);
}

// 체인 작업이 겹쳐 실행되면 active가 1을 넘는다. 기록은 체인 순서로만 쓰이므로 잠금 없이
// 둔다(겹침이 있으면 overlaps가 먼저 드러난다).
struct ChainProbe {
    uint32 active = 0;
    uint32 overlaps = 0;
    uint32 values[256]{};
    uint32 count = 0;
    uint32 wrongLane = 0;
    TaskLane expectedLane = TaskLane::Worker;

    void Enter(uint32 value) noexcept {
        if (AtomicFetchAdd(&active, 1u) != 0) {
            AtomicFetchAdd(&overlaps, 1u);
        }

        if (CurrentTaskLane() != expectedLane) {
            AtomicFetchAdd(&wrongLane, 1u);
        }

        if (count < 256) {
            values[count] = value;
        }

        ++count;
        AtomicFetchSub(&active, 1u);
    }
};

Result<WorkerTaskHandle> SubmitRecord(SerialTaskChain& chain, Allocator& allocator,
                                      ChainProbe& probe, uint32 value,
                                      Span<const WorkerTaskHandle> dependencies = {}) noexcept {
    auto body = Body::TryCreate(
        allocator, [&probe, value](WorkerTaskContext&) noexcept { probe.Enter(value); });
    if (!body) {
        return Result<WorkerTaskHandle>::Failure(body.Error());
    }

    return chain.Submit(Move(body.Value()), dependencies);
}

struct ProducerContext {
    SerialTaskChain* chain;
    Allocator* allocator;
    ChainProbe* probe;
    ManualResetEvent* launch;
    uint32 producer = 0;
    bool ok = true;
};

void ProduceChainTasks(void* raw) noexcept {
    auto& context = *static_cast<ProducerContext*>(raw);
    context.ok = context.launch->Wait(10000) == WaitResult::Signaled;
    for (uint32 sequence = 0; sequence < 32 && context.ok; ++sequence) {
        context.ok = SubmitRecord(*context.chain, *context.allocator, *context.probe,
                                  context.producer * 1000 + sequence)
                         .IsOk();
    }
}

} // namespace

TEST(SerialTaskChain_RunsInSubmitOrderWithoutOverlap) {
    SystemAllocator allocator;
    WorkerScheduler scheduler(allocator);
    WorkerSchedulerConfig config;
    config.workerCount = 4;
    REQUIRE(scheduler.Start(config));
    SerialTaskChain chain(scheduler, TaskLane::Render);
    ChainProbe probe;
    probe.expectedLane = TaskLane::Render;

    // 비어 있는 체인의 Wait는 바로 끝난다.
    auto empty = chain.Wait(0);
    REQUIRE(empty);
    CHECK(empty.Value() == WaitResult::Signaled);

    for (uint32 index = 0; index < 200; ++index) {
        REQUIRE(SubmitRecord(chain, allocator, probe, index));
    }

    auto waited = chain.Wait(10000);
    REQUIRE(waited && waited.Value() == WaitResult::Signaled);
    CHECK(probe.overlaps == 0);
    CHECK(probe.wrongLane == 0);
    REQUIRE(probe.count == 200);
    for (uint32 index = 0; index < 200; ++index) {
        CHECK(probe.values[index] == index);
    }

    REQUIRE(scheduler.Shutdown());
}

TEST(SerialTaskChain_MultiProducerKeepsPerProducerOrder) {
    SystemAllocator allocator;
    WorkerScheduler scheduler(allocator);
    WorkerSchedulerConfig config;
    config.workerCount = 4;
    REQUIRE(scheduler.Start(config));
    SerialTaskChain chain(scheduler);
    ChainProbe probe;
    ManualResetEvent launch;
    ProducerContext contexts[4] = {{&chain, &allocator, &probe, &launch, 0},
                                   {&chain, &allocator, &probe, &launch, 1},
                                   {&chain, &allocator, &probe, &launch, 2},
                                   {&chain, &allocator, &probe, &launch, 3}};
    Thread producers[4];
    for (uint32 index = 0; index < 4; ++index) {
        REQUIRE(producers[index].Start(ProduceChainTasks, &contexts[index]) ==
                ThreadStatus::Success);
    }

    launch.Signal();
    for (Thread& producer : producers) {
        JoinTestThread(producer);
    }

    auto waited = chain.Wait(10000);
    REQUIRE(waited && waited.Value() == WaitResult::Signaled);
    for (const ProducerContext& context : contexts) {
        CHECK(context.ok);
    }

    CHECK(probe.overlaps == 0);
    REQUIRE(probe.count == 128);
    uint32 next[4]{};
    for (uint32 index = 0; index < probe.count; ++index) {
        const uint32 producer = probe.values[index] / 1000;
        REQUIRE(producer < 4);
        CHECK(probe.values[index] % 1000 == next[producer]);
        ++next[producer];
    }

    REQUIRE(scheduler.Shutdown());
}

TEST(SerialTaskChain_HeldHeadBlocksCancelledRevokedAndFailedSuccessors) {
    SystemAllocator allocator;
    WorkerScheduler scheduler(allocator);
    WorkerSchedulerConfig config;
    config.workerCount = 2;
    REQUIRE(scheduler.Start(config));
    SerialTaskChain chain(scheduler);

    // 머리 작업을 gate로 막은 채(실행 중) 뒤 작업을 취소·회수 시도·선행 실패시킨다. 순서 전용
    // 선행이 끝나지 않았으므로 어느 것도 gate 전에 정리·terminal이 되면 안 된다. worker가 비어
    // 있어도 그렇다는 것을 상태로 결정적으로 단언한다.
    ManualResetEvent entered;
    ManualResetEvent gate;
    uint32 headDone = 0;
    uint32 early = 0;
    uint32 runs = 0;
    auto head = Body::TryCreate(allocator, [&](WorkerTaskContext& context) noexcept {
        entered.Signal();
        if (gate.Wait(10000) != WaitResult::Signaled) {
            Fatal("Serial chain gate was not opened");
        }

        AtomicStore(&headDone, 1u);
        context.Fail(ErrorCode::Busy);
    });
    REQUIRE(head);
    auto failed = chain.Submit(Move(head.Value()));
    REQUIRE(failed);
    REQUIRE(entered.Wait(10000) == WaitResult::Signaled);

    auto cancelled = SubmitOrdered(chain, allocator, OrderedCapture(headDone, early, runs));
    REQUIRE(cancelled);
    auto revoked = SubmitOrdered(chain, allocator, OrderedCapture(headDone, early, runs));
    REQUIRE(revoked);
    auto event = scheduler.TryCreateEvent();
    REQUIRE(event);
    auto blocked = SubmitOrdered(chain, allocator, OrderedCapture(headDone, early, runs),
                                 Span<const WorkerTaskHandle>(&event.Value().Handle(), 1));
    REQUIRE(blocked);
    auto tail = SubmitOrdered(chain, allocator, OrderedCapture(headDone, early, runs));
    REQUIRE(tail);

    REQUIRE(cancelled.Value().RequestCancel());
    auto revokedNow = revoked.Value().TryRevoke();
    REQUIRE(revokedNow);
    CHECK(!revokedNow.Value());
    REQUIRE(revoked.Value().RequestCancel());
    auto eventFailed = event.Value().TryFail(ErrorCode::NotFound);
    REQUIRE(eventFailed);
    CHECK(eventFailed.Value());

    CHECK(StateNow(failed.Value()) == WorkerTaskState::Running);
    CHECK(StateNow(cancelled.Value()) == WorkerTaskState::Pending);
    CHECK(StateNow(revoked.Value()) == WorkerTaskState::Pending);
    CHECK(StateNow(blocked.Value()) == WorkerTaskState::Pending);
    CHECK(StateNow(tail.Value()) == WorkerTaskState::Pending);
    auto held = chain.Wait(0);
    REQUIRE(held);
    CHECK(held.Value() == WaitResult::TimedOut);
    CHECK(runs == 0);

    gate.Signal();
    auto waited = chain.Wait(10000);
    REQUIRE(waited && waited.Value() == WaitResult::Signaled);
    // 마지막 작업이 끝났으면 체인 전체가 끝났다.
    CHECK(StateNow(failed.Value()) == WorkerTaskState::Failed);
    CHECK(StateNow(cancelled.Value()) == WorkerTaskState::Cancelled);
    CHECK(StateNow(revoked.Value()) == WorkerTaskState::Cancelled);
    CHECK(StateNow(blocked.Value()) == WorkerTaskState::Failed);
    CHECK(blocked.Value().Status().Value().error == ErrorCode::DependencyFailed);
    CHECK(StateNow(tail.Value()) == WorkerTaskState::Succeeded);
    CHECK(runs == 1);
    CHECK(early == 0);
    REQUIRE(scheduler.Shutdown());
}

TEST(WorkerScheduler_CancelShutdownWaitsForOrderDependencies) {
    SystemAllocator allocator;
    WorkerScheduler scheduler(allocator);
    WorkerSchedulerConfig config;
    config.workerCount = 2;
    REQUIRE(scheduler.Start(config));

    // 실행 중인 앞 작업은 취소 요청을 볼 때까지 돈다(명시 조건 대기). Shutdown(Cancel)이 Pending인
    // 뒤 작업을 정리하는 것은 앞 작업이 끝난 뒤여야 한다.
    ManualResetEvent entered;
    uint32 done = 0;
    uint32 early = 0;
    uint32 runs = 0;
    auto running = Body::TryCreate(allocator, [&](WorkerTaskContext& context) noexcept {
        entered.Signal();
        while (!context.IsCancellationRequested()) {
            YieldThread();
        }

        AtomicStore(&done, 1u);
    });
    REQUIRE(running);
    auto first = scheduler.Submit(Move(running.Value()));
    REQUIRE(first);
    REQUIRE(entered.Wait(10000) == WaitResult::Signaled);

    auto ordered = Body::TryCreate(allocator, OrderedCapture(done, early, runs));
    REQUIRE(ordered);
    WorkerSubmitOptions options;
    options.orderDependencies = Span<const WorkerTaskHandle>(&first.Value(), 1);
    auto second = scheduler.Submit(Move(ordered.Value()), {}, options);
    REQUIRE(second);
    auto revoked = second.Value().TryRevoke();
    REQUIRE(revoked);
    CHECK(!revoked.Value());

    REQUIRE(scheduler.Shutdown(WorkerShutdownMode::Cancel));
    CHECK(StateNow(first.Value()) == WorkerTaskState::Cancelled);
    CHECK(StateNow(second.Value()) == WorkerTaskState::Cancelled);
    CHECK(runs == 0);
    CHECK(early == 0);
}

TEST(SerialTaskChain_RejectionKeepsBodyAndChainSurvivesRestart) {
    SystemAllocator allocator;
    WorkerScheduler scheduler(allocator);
    SerialTaskChain chain(scheduler);
    ChainProbe probe;

    // 시작 전 제출은 ShuttingDown이고 body를 소비하지 않는다.
    auto body =
        Body::TryCreate(allocator, [&probe](WorkerTaskContext&) noexcept { probe.Enter(7); });
    REQUIRE(body);
    auto early = chain.Submit(Move(body.Value()));
    CHECK(!early && early.Error() == ErrorCode::ShuttingDown);
    CHECK(static_cast<bool>(body.Value()));
    auto last = chain.Last();
    REQUIRE(last);
    CHECK(!last.Value().IsValid());

    for (uint32 round = 0; round < 3; ++round) {
        REQUIRE(scheduler.Start());
        REQUIRE(SubmitRecord(chain, allocator, probe, round * 10));
        REQUIRE(SubmitRecord(chain, allocator, probe, round * 10 + 1));
        // 이전 세대의 마지막 작업은 선행에서 빠지므로 재시작 뒤 제출이 WrongContext가 되지 않는다.
        REQUIRE(scheduler.Shutdown());
    }

    REQUIRE(probe.count == 6);
    const uint32 expected[] = {0, 1, 10, 11, 20, 21};
    for (uint32 index = 0; index < 6; ++index) {
        CHECK(probe.values[index] == expected[index]);
    }
}

TEST(WorkerScheduler_OrderDependencyAndLaneOptions) {
    SystemAllocator allocator;
    WorkerScheduler scheduler(allocator);
    WorkerSchedulerConfig config;
    config.workerCount = 2;
    config.maxDependencies = 2;
    REQUIRE(scheduler.Start(config));

    auto failing = Body::TryCreate(
        allocator, [](WorkerTaskContext& context) noexcept { context.Fail(ErrorCode::Busy); });
    REQUIRE(failing);
    auto failed = scheduler.Submit(Move(failing.Value()));
    REQUIRE(failed);
    const Span<const WorkerTaskHandle> failedSpan(&failed.Value(), 1);

    // 순서 전용 선행은 결과와 무관하게 끝난 뒤 실행하고, 일반 선행은 실패를 전파한다.
    TaskLane seen = TaskLane::None;
    auto ordered = Body::TryCreate(
        allocator, [&seen](WorkerTaskContext&) noexcept { seen = CurrentTaskLane(); });
    REQUIRE(ordered);
    WorkerSubmitOptions orderOnly;
    orderOnly.orderDependencies = failedSpan;
    orderOnly.lane = TaskLane::Physics;
    auto afterOrder = scheduler.Submit(Move(ordered.Value()), {}, orderOnly);
    REQUIRE(afterOrder);
    auto strict = Body::TryCreate(allocator, [](WorkerTaskContext&) noexcept {});
    REQUIRE(strict);
    auto afterStrict = scheduler.Submit(Move(strict.Value()), failedSpan);
    REQUIRE(afterStrict);
    CHECK(WaitState(afterOrder.Value()) == WorkerTaskState::Succeeded);
    CHECK(seen == TaskLane::Physics);
    CHECK(WaitState(afterStrict.Value()) == WorkerTaskState::Failed);
    CHECK(afterStrict.Value().Status().Value().error == ErrorCode::DependencyFailed);

    // 기본 lane 표지는 Worker다.
    TaskLane plain = TaskLane::None;
    auto plainBody = Body::TryCreate(
        allocator, [&plain](WorkerTaskContext&) noexcept { plain = CurrentTaskLane(); });
    REQUIRE(plainBody);
    auto plainTask = scheduler.Submit(Move(plainBody.Value()));
    REQUIRE(plainTask);
    CHECK(WaitState(plainTask.Value()) == WorkerTaskState::Succeeded);
    CHECK(plain == TaskLane::Worker);

    // 두 목록에 같은 선행을 넣으면 InvalidArgument, 합이 한도를 넘으면 CapacityExceeded, 잘못된
    // lane은 InvalidArgument다. 모두 body를 소비하지 않는다.
    auto kept = Body::TryCreate(allocator, [](WorkerTaskContext&) noexcept {});
    REQUIRE(kept);
    CHECK(scheduler.Submit(Move(kept.Value()), failedSpan, orderOnly).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(static_cast<bool>(kept.Value()));
    const WorkerTaskHandle* pair[] = {&afterOrder.Value(), &afterStrict.Value()};
    WorkerTaskHandle cloned[2];
    for (uint32 index = 0; index < 2; ++index) {
        auto clone = pair[index]->Clone();
        REQUIRE(clone);
        cloned[index] = Move(clone.Value());
    }

    CHECK(scheduler.Submit(Move(kept.Value()), Span<const WorkerTaskHandle>(cloned, 2), orderOnly)
              .Error() == ErrorCode::CapacityExceeded);
    WorkerSubmitOptions badLane;
    badLane.lane = static_cast<TaskLane>(200);
    CHECK(scheduler.Submit(Move(kept.Value()), {}, badLane).Error() == ErrorCode::InvalidArgument);
    CHECK(static_cast<bool>(kept.Value()));
    REQUIRE(scheduler.Shutdown());
}
