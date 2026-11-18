#include <TestSupport/Test.h>
#include <TestSupport/NFailAllocator.h>
#include <Core/Diagnostics/Correlation.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Task/DedicatedLane.h>

using namespace rocks;
using namespace rocks::platform;

namespace {

using Task = Function<void()>;

// lane 실행 흐름 하나만 쓰는 기록이다. 읽기는 Flush·Shutdown 뒤에만 한다(그 반환이 쓰기 완료를
// 보장한다).
struct Log {
    uint32 values[512]{};
    uint32 count = 0;

    void Add(uint32 value) noexcept {
        if (count == 512) {
            Fatal("Dedicated lane test log overflow");
        }

        values[count++] = value;
    }
};

void JoinTestThread(Thread& thread) noexcept {
    if (thread.WaitForCompletion(10000) != WaitResult::Signaled ||
        thread.Join() != ThreadStatus::Success) {
        Fatal("Dedicated lane test thread failed to terminate");
    }
}

template <typename Callable>
bool PostCallable(DedicatedLane& lane, Allocator& allocator, Callable&& callable) noexcept {
    auto task = Task::TryCreate(allocator, Forward<Callable>(callable));
    if (!task) {
        return false;
    }

    return lane.Post(Move(task.Value())).IsOk();
}

// 재진입 Post 시나리오다. 바깥 작업이 A·B를 넣고, A는 실행 중 C를 넣고, C는 D를 넣는다. 두 모드
// 모두 큐 순서(FIFO 맨 뒤)를 따라 Outer, A, A-after-post, B, C, D 순서가 되어야 한다. Inline
// 모드에서 재진입 Post를 그 자리에서 실행하면 A, C, D, A-after-post 순서가 되어 실패한다.
struct ReentryScenario {
    DedicatedLane* lane;
    Allocator* allocator;
    Log log;
    bool ok = true;

    void PostD() noexcept {
        ok = PostCallable(*lane, *allocator, [this]() noexcept { log.Add(5); }) && ok;
    }

    void PostC() noexcept {
        ok = PostCallable(*lane, *allocator,
                          [this]() noexcept {
                              log.Add(4);
                              PostD();
                          }) &&
             ok;
    }

    void Outer() noexcept {
        log.Add(0);
        ok = PostCallable(*lane, *allocator,
                          [this]() noexcept {
                              log.Add(1);
                              PostC();
                              log.Add(2);
                          }) &&
             ok;
        ok = PostCallable(*lane, *allocator, [this]() noexcept { log.Add(3); }) && ok;
    }
};

void RunReentryScenario(DedicatedLaneMode mode, Log& result) {
    SystemAllocator allocator;
    DedicatedLane lane(allocator);
    DedicatedLaneConfig config;
    config.mode = mode;
    REQUIRE(lane.Start(config));
    ReentryScenario scenario{&lane, &allocator, {}};
    REQUIRE(PostCallable(lane, allocator, [&scenario]() noexcept { scenario.Outer(); }));
    REQUIRE(lane.Flush());
    REQUIRE(lane.Shutdown());
    CHECK(scenario.ok);
    result = scenario.log;
}

struct ProducerContext {
    DedicatedLane* lane;
    Allocator* allocator;
    Log* log;
    ManualResetEvent* launch;
    uint32 producer = 0;
    bool ok = true;
};

void ProduceTasks(void* raw) noexcept {
    auto& context = *static_cast<ProducerContext*>(raw);
    context.ok = context.launch->Wait(10000) == WaitResult::Signaled;
    for (uint32 sequence = 0; sequence < 64 && context.ok; ++sequence) {
        const uint32 value = context.producer * 1000 + sequence;
        Log* log = context.log;
        context.ok = PostCallable(*context.lane, *context.allocator,
                                  [log, value]() noexcept { log->Add(value); });
    }
}

// 시작 handshake 뒤라면 첫 작업이 이름이 붙은 lane 스레드에서 실행된다.
struct NameProbe {
    char name[MaxThreadNameBytes + 1]{};
    usize length = 0;
    TaskLane lane = TaskLane::None;
};

// lane 전용 API의 예다. 스레드 ID가 아니라 실행 표지로 거부한다.
Result<void> LaneOnlyOperation(const DedicatedLane& lane) noexcept {
    if (!lane.IsExecutingTask()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    return Result<void>::Success();
}

struct ContextProbe {
    DedicatedLane* outer;
    DedicatedLane* inner;
    Allocator* allocator;
    bool outerInsideOuter = false;
    bool innerInsideInner = false;
    bool outerInsideInner = true;
    ErrorCode laneOnlyInside = ErrorCode::OutOfMemory;
    ErrorCode flushInside = ErrorCode::None;
    ErrorCode shutdownInside = ErrorCode::None;
    ErrorCode outerFlushFromInner = ErrorCode::None;
    TaskLane laneInsideOuter = TaskLane::None;
    TaskLane laneInsideInner = TaskLane::None;
    TaskLane laneAfterInner = TaskLane::None;
    bool posted = false;
};

} // namespace

TEST(DedicatedLane_MultiProducerFifoPerProducer) {
    SystemAllocator allocator;
    DedicatedLane lane(allocator);
    REQUIRE(lane.Start());

    // lane을 gate로 막아 둔 채 네 생산자가 동시에 넣는다. 생산자마다 넣은 순서가 실행 순서여야
    // 한다.
    ManualResetEvent gate;
    ManualResetEvent launch;
    Log log;
    REQUIRE(PostCallable(lane, allocator, [&gate]() noexcept {
        if (gate.Wait(10000) != WaitResult::Signaled) {
            Fatal("Dedicated lane gate was not opened");
        }
    }));

    ProducerContext contexts[4] = {{&lane, &allocator, &log, &launch, 0},
                                   {&lane, &allocator, &log, &launch, 1},
                                   {&lane, &allocator, &log, &launch, 2},
                                   {&lane, &allocator, &log, &launch, 3}};
    Thread producers[4];
    for (uint32 index = 0; index < 4; ++index) {
        REQUIRE(producers[index].Start(ProduceTasks, &contexts[index]) == ThreadStatus::Success);
    }

    launch.Signal();
    for (Thread& producer : producers) {
        JoinTestThread(producer);
    }

    gate.Signal();
    REQUIRE(lane.Flush());

    for (const ProducerContext& context : contexts) {
        CHECK(context.ok);
    }

    REQUIRE(log.count == 256);
    uint32 next[4]{};
    for (uint32 index = 0; index < log.count; ++index) {
        const uint32 producer = log.values[index] / 1000;
        REQUIRE(producer < 4);
        CHECK(log.values[index] % 1000 == next[producer]);
        ++next[producer];
    }

    const DedicatedLaneStats stats = lane.Stats();
    CHECK(stats.posted == 257);
    CHECK(stats.completed == 257);
    REQUIRE(lane.Shutdown());
}

TEST(DedicatedLane_ReentrantPostOrderMatchesAcrossModes) {
    Log threaded;
    Log inlined;
    RunReentryScenario(DedicatedLaneMode::Thread, threaded);
    RunReentryScenario(DedicatedLaneMode::Inline, inlined);
    const uint32 expected[] = {0, 1, 2, 3, 4, 5};
    REQUIRE(threaded.count == 6);
    REQUIRE(inlined.count == 6);
    for (uint32 index = 0; index < 6; ++index) {
        CHECK(threaded.values[index] == expected[index]);
        CHECK(inlined.values[index] == expected[index]);
    }
}

TEST(DedicatedLane_InlinePostRunsBeforeReturnOutsideTasks) {
    SystemAllocator allocator;
    DedicatedLane lane(allocator);
    DedicatedLaneConfig config;
    config.mode = DedicatedLaneMode::Inline;
    REQUIRE(lane.Start(config));
    uint32 calls = 0;
    REQUIRE(PostCallable(lane, allocator, [&calls]() noexcept { ++calls; }));
    // 가장 바깥 Post는 큐가 빌 때까지 실행하고 돌아온다.
    CHECK(calls == 1);
    CHECK(lane.Stats().completed == 1);
    REQUIRE(lane.Shutdown());
}

TEST(DedicatedLane_ShutdownDrainsQueueAndContinuations) {
    const DedicatedLaneMode modes[] = {DedicatedLaneMode::Thread, DedicatedLaneMode::Inline};
    for (DedicatedLaneMode mode : modes) {
        SystemAllocator allocator;
        DedicatedLane lane(allocator);
        DedicatedLaneConfig config;
        config.mode = mode;
        REQUIRE(lane.Start(config));
        uint32 calls = 0;
        for (uint32 index = 0; index < 50; ++index) {
            REQUIRE(PostCallable(lane, allocator, [&calls]() noexcept { ++calls; }));
        }

        // 이 작업이 Shutdown 전에 실행되든 종료 중에 실행되든 그 후속 Post는 받아 실행해야 한다.
        bool continuationAccepted = false;
        REQUIRE(PostCallable(lane, allocator, [&]() noexcept {
            continuationAccepted =
                PostCallable(lane, allocator, [&calls]() noexcept { calls += 100; });
        }));
        REQUIRE(lane.Shutdown());
        CHECK(continuationAccepted);
        CHECK(calls == 150);
        const DedicatedLaneStats stats = lane.Stats();
        CHECK(!stats.running);
        CHECK(stats.posted == 52);
        CHECK(stats.completed == 52);

        // 종료 뒤 Post는 ShuttingDown이고 task 소유권은 호출자에게 남는다.
        auto late = Task::TryCreate(allocator, [&calls]() noexcept { ++calls; });
        REQUIRE(late);
        auto rejected = lane.Post(Move(late.Value()));
        CHECK(!rejected && rejected.Error() == ErrorCode::ShuttingDown);
        CHECK(static_cast<bool>(late.Value()));
        CHECK(lane.Flush().Error() == ErrorCode::ShuttingDown);
        CHECK(lane.Shutdown());
    }
}

// 올린 스레드의 상관 id가 lane 작업에 전파되고, 작업이 끝나면 lane 스레드의 스택은 원래 깊이로
// 돌아온다(다음 작업·다음 프레임으로 새지 않는다). 직렬 체인처럼 이어 올린 작업과 Inline 모드도
// 같다.
TEST(DedicatedLane_PropagatesCorrelationWithoutLeakingAcrossTasks) {
    const DedicatedLaneMode modes[] = {DedicatedLaneMode::Thread, DedicatedLaneMode::Inline};
    for (DedicatedLaneMode mode : modes) {
        SystemAllocator allocator;
        DedicatedLane lane(allocator);
        DedicatedLaneConfig config;
        config.mode = mode;
        REQUIRE(lane.Start(config));
        uint64 seenRequest = 0;
        usize depthInside = 0;
        usize depthAfter = MaxCorrelationDepth;
        bool emptyInNext = false;
        const usize outerDepth = CorrelationDepth();
        {
            CorrelationScope request(CorrelationIds{0, 0, 0, 0, 77});
            REQUIRE(PostCallable(lane, allocator, [&]() noexcept {
                CorrelationIds ids;
                if (CurrentCorrelation(ids)) {
                    seenRequest = ids.request;
                }

                depthInside = CorrelationDepth();
            }));
        }

        // 상관 id 없이 올린 다음 작업(다음 프레임 slot에 해당)은 빈 스택을 본다.
        REQUIRE(PostCallable(lane, allocator, [&]() noexcept {
            CorrelationIds ids;
            emptyInNext = !CurrentCorrelation(ids);
            depthAfter = CorrelationDepth();
        }));
        REQUIRE(lane.Flush());
        CHECK(seenRequest == 77);
        // Thread는 lane 스레드 기준(빈 스택 위에 복원 칸 하나), Inline은 올린 스레드에서 바로
        // 돌므로 올린 쪽 범위 위에 복원 칸이 하나 더 쌓인다.
        const bool inlineMode = mode == DedicatedLaneMode::Inline;
        CHECK(depthInside == (inlineMode ? outerDepth + 2 : 1u));
        CHECK(emptyInNext);
        CHECK(depthAfter == (inlineMode ? outerDepth : 0u));
        CHECK(CorrelationDepth() == outerDepth);
        REQUIRE(lane.Shutdown());
    }
}

TEST(DedicatedLane_FlushWaitsForEarlierTasks) {
    SystemAllocator allocator;
    DedicatedLane lane(allocator);
    REQUIRE(lane.Start());
    ManualResetEvent gate;
    uint32 calls = 0;
    REQUIRE(PostCallable(lane, allocator, [&gate]() noexcept {
        if (gate.Wait(10000) != WaitResult::Signaled) {
            Fatal("Dedicated lane gate was not opened");
        }
    }));
    for (uint32 index = 0; index < 10; ++index) {
        REQUIRE(PostCallable(lane, allocator, [&calls]() noexcept { ++calls; }));
    }

    CHECK(lane.Stats().completed == 0);
    gate.Signal();
    REQUIRE(lane.Flush());
    CHECK(calls == 10);
    CHECK(lane.Stats().completed == 11);
    REQUIRE(lane.Shutdown());
}

TEST(DedicatedLane_LaneOnlyApiRejectedOutsideTasks) {
    const DedicatedLaneMode modes[] = {DedicatedLaneMode::Thread, DedicatedLaneMode::Inline};
    for (DedicatedLaneMode mode : modes) {
        SystemAllocator allocator;
        DedicatedLane outer(allocator);
        DedicatedLane inner(allocator);
        DedicatedLaneConfig outerConfig;
        outerConfig.mode = mode;
        outerConfig.lane = TaskLane::Render;
        DedicatedLaneConfig innerConfig;
        innerConfig.mode = DedicatedLaneMode::Inline;
        innerConfig.lane = TaskLane::Audio;
        REQUIRE(outer.Start(outerConfig));
        REQUIRE(inner.Start(innerConfig));

        // Inline 모드에서는 이 스레드가 곧 작업을 실행할 스레드지만, 작업 밖이므로 거부되어야 한다.
        CHECK(LaneOnlyOperation(outer).Error() == ErrorCode::WrongThread);
        CHECK(!outer.IsExecutingTask());
        CHECK(CurrentTaskLane() == TaskLane::None);

        ContextProbe probe{&outer, &inner, &allocator};
        REQUIRE(PostCallable(outer, allocator, [&probe]() noexcept {
            probe.outerInsideOuter = probe.outer->IsExecutingTask();
            probe.laneOnlyInside =
                LaneOnlyOperation(*probe.outer).IsOk() ? ErrorCode::None : ErrorCode::WrongThread;
            probe.flushInside = probe.outer->Flush().Error();
            probe.shutdownInside = probe.outer->Shutdown().Error();
            probe.laneInsideOuter = CurrentTaskLane();
            // Inline lane inner는 이 작업 안에서 그 자리에서 비워진다. 그 작업은 inner만 실행
            // 중이다.
            probe.posted = PostCallable(*probe.inner, *probe.allocator, [&probe]() noexcept {
                probe.innerInsideInner = probe.inner->IsExecutingTask();
                probe.outerInsideInner = probe.outer->IsExecutingTask();
                probe.laneInsideInner = CurrentTaskLane();
                // outer 작업 위에 중첩된 실행이 outer를 기다리면 교착이므로 거부한다.
                probe.outerFlushFromInner = probe.outer->Flush().Error();
            });
            probe.laneAfterInner = CurrentTaskLane();
        }));
        REQUIRE(outer.Flush());
        CHECK(probe.posted);
        CHECK(probe.outerInsideOuter);
        CHECK(probe.laneOnlyInside == ErrorCode::None);
        CHECK(probe.flushInside == ErrorCode::WrongThread);
        CHECK(probe.shutdownInside == ErrorCode::WrongThread);
        CHECK(probe.laneInsideOuter == TaskLane::Render);
        CHECK(probe.innerInsideInner);
        CHECK(!probe.outerInsideInner);
        CHECK(probe.laneInsideInner == TaskLane::Audio);
        CHECK(probe.outerFlushFromInner == ErrorCode::WrongThread);
        CHECK(probe.laneAfterInner == TaskLane::Render);
        CHECK(CurrentTaskLane() == TaskLane::None);
        REQUIRE(inner.Shutdown());
        REQUIRE(outer.Shutdown());
    }
}

TEST(DedicatedLane_StartContractAndThreadName) {
    SystemAllocator allocator;
    DedicatedLane lane(allocator);

    // 시작 전 Post는 ShuttingDown이고 task를 소비하지 않는다. 빈 task는 InvalidArgument다.
    auto early = Task::TryCreate(allocator, []() noexcept {});
    REQUIRE(early);
    CHECK(lane.Post(Move(early.Value())).Error() == ErrorCode::ShuttingDown);
    CHECK(static_cast<bool>(early.Value()));
    CHECK(lane.Post(Task{}).Error() == ErrorCode::InvalidArgument);
    CHECK(lane.Shutdown());

    // 잘못된 스레드 옵션은 lane을 열지 않는다.
    char longName[MaxThreadNameBytes + 2]{};
    for (usize index = 0; index < MaxThreadNameBytes + 1; ++index) {
        longName[index] = 'n';
    }

    DedicatedLaneConfig invalid;
    invalid.thread.name = longName;
    CHECK(lane.Start(invalid).Error() == ErrorCode::InvalidArgument);
    CHECK(!lane.Stats().running);
    DedicatedLaneConfig badMode;
    badMode.mode = static_cast<DedicatedLaneMode>(7);
    CHECK(lane.Start(badMode).Error() == ErrorCode::InvalidArgument);

    // 시작 handshake 뒤의 작업은 이름이 붙은 lane 스레드에서 실행된다. 재시작도 된다.
    for (uint32 round = 0; round < 2; ++round) {
        DedicatedLaneConfig config;
        config.lane = TaskLane::Render;
        config.thread.name = "RocksLaneTest";
        config.thread.priority = ThreadPriority::AboveNormal;
        config.thread.stackBytes = 512 * 1024;
        REQUIRE(lane.Start(config));
        CHECK(lane.Start(config).Error() == ErrorCode::Busy);
        NameProbe probe;
        REQUIRE(PostCallable(lane, allocator, [&probe]() noexcept {
            probe.length = CopyCurrentThreadName(probe.name, sizeof(probe.name));
            probe.lane = CurrentTaskLane();
        }));
        REQUIRE(lane.Flush());
        CHECK(probe.length == 13);
        CHECK(MemoryEquals(probe.name, "RocksLaneTest", 14));
        CHECK(probe.lane == TaskLane::Render);
        REQUIRE(lane.Shutdown());
    }

    // 큐 칸 할당 실패는 그 오류이고 task를 소비하지 않는다.
    test::NFailAllocator failing(allocator);
    DedicatedLane limited(failing);
    DedicatedLaneConfig inlineConfig;
    inlineConfig.mode = DedicatedLaneMode::Inline;
    REQUIRE(limited.Start(inlineConfig));
    auto kept = Task::TryCreate(allocator, []() noexcept {});
    REQUIRE(kept);
    failing.FailOnAttempt(1);
    CHECK(limited.Post(Move(kept.Value())).Error() == ErrorCode::OutOfMemory);
    CHECK(static_cast<bool>(kept.Value()));
    failing.DisableFailure();
    CHECK(limited.Post(Move(kept.Value())));
    CHECK(!static_cast<bool>(kept.Value()));
    REQUIRE(limited.Shutdown());
}

TEST(DedicatedLane_StartAndShutdownRequireOwnerThread) {
    SystemAllocator allocator;
    DedicatedLane lane(allocator);
    struct Probe {
        DedicatedLane* lane;
        ErrorCode start = ErrorCode::None;
        ErrorCode shutdown = ErrorCode::None;
    } probe{&lane};
    Thread other;
    REQUIRE(other.Start(
                [](void* raw) noexcept {
                    auto& self = *static_cast<Probe*>(raw);
                    self.start = self.lane->Start().Error();
                    self.shutdown = self.lane->Shutdown().Error();
                },
                &probe) == ThreadStatus::Success);
    JoinTestThread(other);
    CHECK(probe.start == ErrorCode::WrongThread);
    CHECK(probe.shutdown == ErrorCode::WrongThread);
    CHECK(!lane.Stats().running);
}

namespace {

// 종료가 시작되었음(accepting false)을 본 뒤 외부 Post를 시도하고 gate를 연다. 상태 조건을 기다리는
// 명시 대기이며 시간에 기대지 않는다.
struct ShutdownObserver {
    DedicatedLane* lane;
    Allocator* allocator;
    ManualResetEvent* gate;
    ErrorCode external = ErrorCode::None;
    bool taskKept = false;
};

void ObserveShutdownThenOpen(void* raw) noexcept {
    auto& observer = *static_cast<ShutdownObserver*>(raw);
    while (observer.lane->Stats().accepting) {
        YieldThread();
    }

    auto task = Task::TryCreate(*observer.allocator, []() noexcept {});
    if (task) {
        observer.external = observer.lane->Post(Move(task.Value())).Error();
        observer.taskKept = static_cast<bool>(task.Value());
    }

    observer.gate->Signal();
}

struct InlineDrainer {
    DedicatedLane* lane;
    Allocator* allocator;
    ManualResetEvent* entered;
    ManualResetEvent* gate;
    uint64 thread = 0;
    bool posted = false;
};

void DrainFromOtherThread(void* raw) noexcept {
    auto& drainer = *static_cast<InlineDrainer*>(raw);
    // 이 스레드가 Inline lane을 비우는 흐름이 된다. 첫 작업이 gate에서 막혀 있는 동안 다른 스레드가
    // Post한 작업도 이 스레드가 실행한다.
    drainer.posted = PostCallable(*drainer.lane, *drainer.allocator, [&drainer]() noexcept {
        drainer.thread = CurrentThreadId();
        drainer.entered->Signal();
        if (drainer.gate->Wait(10000) != WaitResult::Signaled) {
            Fatal("Inline lane gate was not opened");
        }
    });
}

struct LaunchProbe {
    uint32 calls = 0;
    bool startAnyway = false;
    bool succeed = false;
};

ThreadStatus ProbeLaunch(void* context, Thread& thread, Thread::Entry entry, void* argument,
                         const ThreadOptions& options) noexcept {
    auto& probe = *static_cast<LaunchProbe*>(context);
    ++probe.calls;
    if (probe.succeed) {
        return thread.Start(entry, argument, options);
    }

    if (probe.startAnyway) {
        // 계약을 어긴 launcher: 스레드를 시작하고도 실패를 돌려준다.
        static_cast<void>(thread.Start(entry, argument, options));
    }

    return ThreadStatus::SystemError;
}

} // namespace

TEST(DedicatedLane_ThreadShutdownAcceptsOnlyLaneContinuations) {
    SystemAllocator allocator;
    DedicatedLane lane(allocator);
    REQUIRE(lane.Start());
    ManualResetEvent entered;
    ManualResetEvent gate;
    bool continuationAccepted = false;
    bool continuationRan = false;
    REQUIRE(PostCallable(lane, allocator, [&]() noexcept {
        entered.Signal();
        if (gate.Wait(10000) != WaitResult::Signaled) {
            Fatal("Dedicated lane gate was not opened");
        }

        // gate는 Shutdown이 수락을 닫은 뒤에만 열리므로 이 Post는 반드시 종료 중이다.
        continuationAccepted =
            PostCallable(lane, allocator, [&]() noexcept { continuationRan = true; });
    }));
    REQUIRE(entered.Wait(10000) == WaitResult::Signaled);

    ShutdownObserver observer{&lane, &allocator, &gate};
    Thread helper;
    REQUIRE(helper.Start(ObserveShutdownThenOpen, &observer) == ThreadStatus::Success);
    REQUIRE(lane.Shutdown());
    JoinTestThread(helper);

    CHECK(observer.external == ErrorCode::ShuttingDown);
    CHECK(observer.taskKept);
    CHECK(continuationAccepted);
    CHECK(continuationRan);
    const DedicatedLaneStats stats = lane.Stats();
    CHECK(stats.posted == 2);
    CHECK(stats.completed == 2);
    CHECK(!stats.accepting);
}

TEST(DedicatedLane_InlineCrossThreadTrampolineAndShutdown) {
    // lane을 Shutdown 직후 파괴한 뒤에 비우던 스레드를 Join한다. 비우던 스레드가 잠금 밖에서 lane을
    // 건드리면 Asan 구성에서 해제 뒤 사용으로 드러난다.
    for (uint32 round = 0; round < 20; ++round) {
        SystemAllocator allocator;
        ManualResetEvent entered;
        ManualResetEvent gate;
        Thread drainerThread;
        Thread helper;
        InlineDrainer drainer{nullptr, &allocator, &entered, &gate};
        uint64 secondRunner = 0;
        bool secondRan = false;
        ErrorCode external = ErrorCode::None;
        {
            DedicatedLane lane(allocator);
            DedicatedLaneConfig config;
            config.mode = DedicatedLaneMode::Inline;
            REQUIRE(lane.Start(config));
            drainer.lane = &lane;
            REQUIRE(drainerThread.Start(DrainFromOtherThread, &drainer) == ThreadStatus::Success);
            REQUIRE(entered.Wait(10000) == WaitResult::Signaled);

            // 다른 스레드가 비우는 중이므로 이 Post는 실행하지 않고 돌아온다(그 스레드가 실행한다).
            REQUIRE(PostCallable(lane, allocator, [&]() noexcept {
                secondRunner = CurrentThreadId();
                secondRan = true;
            }));
            CHECK(!secondRan);

            ShutdownObserver observer{&lane, &allocator, &gate};
            REQUIRE(helper.Start(ObserveShutdownThenOpen, &observer) == ThreadStatus::Success);
            // Inline Shutdown은 비우던 스레드가 큐를 다 비울 때까지 기다린다.
            REQUIRE(lane.Shutdown());
            CHECK(secondRan);
            CHECK(lane.Stats().completed == 2);
            JoinTestThread(helper);
            external = observer.external;
        }

        JoinTestThread(drainerThread);
        CHECK(drainer.posted);
        CHECK(secondRunner == drainer.thread);
        CHECK(external == ErrorCode::ShuttingDown);
    }
}

TEST(DedicatedLane_LauncherFailureLeavesLaneUnchanged) {
    SystemAllocator allocator;
    DedicatedLane lane(allocator);
    DedicatedLaneConfig inlineConfig;
    inlineConfig.mode = DedicatedLaneMode::Inline;
    REQUIRE(lane.Start(inlineConfig));
    REQUIRE(lane.Shutdown());

    // 실패한 launcher(스레드 없음)와 계약을 어긴 launcher(스레드를 시작하고 실패 반환) 모두 Start가
    // OutOfMemory이고 lane 상태(mode 포함)를 바꾸지 않는다. 시작된 스레드는 Start가
    // 회수한다(회수하지 않으면 Thread 파괴가 Fatal이다).
    LaunchProbe probe;
    DedicatedLaneConfig config;
    config.lane = TaskLane::Render;
    config.launch = ProbeLaunch;
    config.launchContext = &probe;
    const bool startAnyway[] = {false, true};
    for (bool anyway : startAnyway) {
        probe.startAnyway = anyway;
        CHECK(lane.Start(config).Error() == ErrorCode::OutOfMemory);
        const DedicatedLaneStats stats = lane.Stats();
        CHECK(!stats.running);
        CHECK(!stats.accepting);
        CHECK(stats.mode == DedicatedLaneMode::Inline);
        auto task = Task::TryCreate(allocator, []() noexcept {});
        REQUIRE(task);
        CHECK(lane.Post(Move(task.Value())).Error() == ErrorCode::ShuttingDown);
    }

    CHECK(probe.calls == 2);

    // 잘못된 lane 값은 InvalidArgument다.
    DedicatedLaneConfig badLane;
    badLane.lane = static_cast<TaskLane>(TaskLaneCount);
    CHECK(lane.Start(badLane).Error() == ErrorCode::InvalidArgument);

    // 정상 launcher로는 시작된다.
    probe.succeed = true;
    REQUIRE(lane.Start(config));
    CHECK(probe.calls == 3);
    CHECK(lane.Stats().mode == DedicatedLaneMode::Thread);
    TaskLane seen = TaskLane::None;
    REQUIRE(PostCallable(lane, allocator, [&seen]() noexcept { seen = CurrentTaskLane(); }));
    REQUIRE(lane.Flush());
    CHECK(seen == TaskLane::Render);
    REQUIRE(lane.Shutdown());
}
