#include <TestSupport/Test.h>
#include <TestSupport/NFailAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Task/ParallelFor.h>

using namespace rocks;
namespace parallel_for_tests_file {

using namespace rocks::platform;

namespace {

using Body = Function<void(WorkerTaskContext&)>;

constexpr usize Count = 10007;

// 원소마다 몇 번 실행됐는지와 batch가 본 lane 표지를 센다.
struct Coverage {
    uint32 hits[Count]{};
    uint32 wrongLane = 0;
    uint32 batches = 0;
    TaskLane expectedLane = TaskLane::None;

    void Visit(usize begin, usize end) noexcept {
        AtomicFetchAdd(&batches, 1u);
        if (CurrentTaskLane() != expectedLane) {
            AtomicFetchAdd(&wrongLane, 1u);
        }

        for (usize index = begin; index < end; ++index) {
            AtomicFetchAdd(&hits[index], 1u);
        }
    }

    bool EachOnce(usize limit) const noexcept {
        for (usize index = 0; index < limit; ++index) {
            if (hits[index] != 1) {
                return false;
            }
        }

        return true;
    }
};

} // namespace

TEST(ParallelFor_CoversRangeOnceAndInheritsLane) {
    SystemAllocator allocator;
    WorkerScheduler scheduler(allocator);
    WorkerSchedulerConfig config;
    config.workerCount = 4;
    REQUIRE(scheduler.Start(config));

    static Coverage coverage;
    coverage = Coverage{};
    coverage.expectedLane = TaskLane::Render;
    {
        // 렌더 lane이 부른 ParallelFor의 batch는 helper worker에서도 Render 문맥이다.
        TaskLaneScope scope(TaskLane::Render);
        ParallelForOptions options;
        options.minBatchSize = 7;
        auto done = ParallelFor(
            scheduler, allocator, Count,
            [](usize begin, usize end) noexcept { coverage.Visit(begin, end); }, options);
        REQUIRE(done);
    }

    CHECK(coverage.EachOnce(Count));
    CHECK(coverage.wrongLane == 0);
    CHECK(coverage.batches > 1);

    // lane 표지가 없는 호출자의 helper는 Worker 문맥이고 호출 스레드는 None 그대로다. 그래서 어느
    // 쪽에서 실행됐는지에 따라 표지가 다르다. 여기서는 개수만 확인한다.
    coverage = Coverage{};
    coverage.expectedLane = TaskLane::None;
    auto plain = ParallelFor(scheduler, allocator, Count,
                             [](usize begin, usize end) noexcept { coverage.Visit(begin, end); });
    REQUIRE(plain);
    CHECK(coverage.EachOnce(Count));

    // count 0은 body를 부르지 않는다.
    uint32 calls = 0;
    auto empty = ParallelFor(scheduler, allocator, 0, [&calls](usize, usize) noexcept { ++calls; });
    CHECK(empty);
    CHECK(calls == 0);
    REQUIRE(scheduler.Shutdown());
}

TEST(ParallelFor_FirstFailingBatchIsDeterministic) {
    SystemAllocator allocator;
    WorkerScheduler scheduler(allocator);
    WorkerSchedulerConfig config;
    config.workerCount = 4;
    REQUIRE(scheduler.Start(config));

    // [300, 400)은 Busy, [700, 800)은 NotFound로 실패한다. 순차 실행에서 처음 만나는 실패(Busy)가
    // 매번 결과여야 하고, 그보다 앞의 원소는 모두 실행되어야 한다.
    for (uint32 round = 0; round < 20; ++round) {
        static Coverage coverage;
        coverage = Coverage{};
        ParallelForOptions options;
        options.minBatchSize = 10;
        auto failed = ParallelFor(
            scheduler, allocator, 1000,
            [](usize begin, usize end) noexcept -> ErrorCode {
                coverage.Visit(begin, end);
                if (begin < 400 && end > 300) {
                    return ErrorCode::Busy;
                }

                if (begin < 800 && end > 700) {
                    return ErrorCode::NotFound;
                }

                return ErrorCode::None;
            },
            options);
        REQUIRE(!failed);
        CHECK(failed.Error() == ErrorCode::Busy);
        CHECK(coverage.EachOnce(300));
    }

    REQUIRE(scheduler.Shutdown());
}

TEST(ParallelFor_RunsInlineInsideWorkerOrWithoutHelpers) {
    SystemAllocator allocator;
    WorkerScheduler scheduler(allocator);

    // 시작 전 scheduler에서도 호출 스레드가 모두 실행한다.
    static Coverage coverage;
    coverage = Coverage{};
    auto stopped = ParallelFor(scheduler, allocator, Count,
                               [](usize begin, usize end) noexcept { coverage.Visit(begin, end); });
    REQUIRE(stopped);
    CHECK(coverage.EachOnce(Count));

    WorkerSchedulerConfig config;
    config.workerCount = 2;
    REQUIRE(scheduler.Start(config));

    // maxHelpers 0이면 호출 스레드만 실행한다.
    struct ThreadProbe {
        uint64 caller = 0;
        uint32 foreign = 0;
    } probe{CurrentThreadId()};
    ParallelForOptions single;
    single.maxHelpers = 0;
    auto alone = ParallelFor(
        scheduler, allocator, 1000,
        [&probe](usize, usize) noexcept {
            if (CurrentThreadId() != probe.caller) {
                AtomicFetchAdd(&probe.foreign, 1u);
            }
        },
        single);
    REQUIRE(alone);
    CHECK(probe.foreign == 0);

    // worker 안에서 부르면 helper를 기다리지 않고 그 worker가 모두 실행한다.
    struct NestedProbe {
        WorkerScheduler* scheduler;
        Allocator* allocator;
        uint64 worker = 0;
        uint32 foreign = 0;
        uint32 visited = 0;
        ErrorCode result = ErrorCode::OutOfMemory;
    } nested{&scheduler, &allocator};
    auto body = Body::TryCreate(allocator, [&nested](WorkerTaskContext&) noexcept {
        nested.worker = CurrentThreadId();
        auto done = ParallelFor(*nested.scheduler, *nested.allocator, 500,
                                [&nested](usize begin, usize end) noexcept {
                                    if (CurrentThreadId() != nested.worker) {
                                        ++nested.foreign;
                                    }

                                    nested.visited += static_cast<uint32>(end - begin);
                                });
        nested.result = done.IsOk() ? ErrorCode::None : done.Error();
    });
    REQUIRE(body);
    auto task = scheduler.Submit(Move(body.Value()));
    REQUIRE(task);
    auto waited = task.Value().Wait(10000);
    REQUIRE(waited && waited.Value() == WaitResult::Signaled);
    CHECK(nested.result == ErrorCode::None);
    CHECK(nested.foreign == 0);
    CHECK(nested.visited == 500);

    // helper 본문 할당이 실패해도 호출 스레드가 남은 일을 모두 실행한다.
    test::NFailAllocator failing(allocator);
    coverage = Coverage{};
    failing.FailOnAttempt(1);
    auto degraded = ParallelFor(scheduler, failing, Count, [](usize begin, usize end) noexcept {
        coverage.Visit(begin, end);
    });
    REQUIRE(degraded);
    CHECK(coverage.EachOnce(Count));
    REQUIRE(scheduler.Shutdown());
}

TEST(ParallelFor_SmallCountsAndGameLaneIsNotInherited) {
    SystemAllocator allocator;
    WorkerScheduler scheduler(allocator);
    WorkerSchedulerConfig config;
    config.workerCount = 4;
    REQUIRE(scheduler.Start(config));

    // count가 minBatchSize보다 작거나 1이면 batch 하나로 호출 스레드가 한 번 부른다.
    struct Call {
        uint32 calls = 0;
        usize begin = IndexNone;
        usize end = IndexNone;
    };

    const usize counts[] = {5, 1};
    for (usize count : counts) {
        Call call;
        ParallelForOptions options;
        options.minBatchSize = 100;
        auto done = ParallelFor(
            scheduler, allocator, count,
            [&call](usize begin, usize end) noexcept {
                AtomicFetchAdd(&call.calls, 1u);
                call.begin = begin;
                call.end = end;
            },
            options);
        REQUIRE(done);
        CHECK(call.calls == 1);
        CHECK(call.begin == 0);
        CHECK(call.end == count);
    }

    // Game 표지는 helper에 물려주지 않는다. Game으로 보이는 batch는 모두 호출 스레드에서 실행됐고,
    // 그 밖의 batch는 Worker여야 한다.
    struct LaneProbe {
        uint64 caller = 0;
        uint32 gameOnWorker = 0;
        uint32 unexpected = 0;
    } probe{CurrentThreadId()};
    {
        TaskLaneScope scope(TaskLane::Game);
        auto done = ParallelFor(scheduler, allocator, 4000, [&probe](usize, usize) noexcept {
            const TaskLane lane = CurrentTaskLane();
            if (lane == TaskLane::Game) {
                if (CurrentThreadId() != probe.caller) {
                    AtomicFetchAdd(&probe.gameOnWorker, 1u);
                }
            } else if (lane != TaskLane::Worker) {
                AtomicFetchAdd(&probe.unexpected, 1u);
            }
        });
        REQUIRE(done);
    }

    CHECK(probe.gameOnWorker == 0);
    CHECK(probe.unexpected == 0);
    REQUIRE(scheduler.Shutdown());
}

namespace {

struct CancelledHelperProbe {
    WorkerScheduler* scheduler;
    Allocator* allocator;
    ManualResetEvent* helperEntered;
    ManualResetEvent* gate;
    uint32 blocked = 0;
    uint32 hits[1000]{};
    ErrorCode result = ErrorCode::OutOfMemory;
};

void RunCancelledParallelFor(void* raw) noexcept {
    auto& probe = *static_cast<CancelledHelperProbe*>(raw);
    auto done = ParallelFor(
        *probe.scheduler, *probe.allocator, 1000, [&probe](usize begin, usize end) noexcept {
            // 처음 batch를 잡은 helper 하나만 gate에서 막는다. 호출 스레드는 helper가 batch를 잡을
            // 때까지 자기 batch를 끝내지 않는다. 그러지 않으면 호출 스레드가 모든 batch를 먼저
            // 가져가 helper가 batch를 하나도 못 잡을 수 있다.
            if (probe.scheduler->IsCurrentWorker()) {
                if (AtomicExchange(&probe.blocked, 1u) == 0) {
                    probe.helperEntered->Signal();
                    if (probe.gate->Wait(10000) != WaitResult::Signaled) {
                        Fatal("ParallelFor gate was not opened");
                    }
                }
            } else if (probe.helperEntered->Wait(10000) != WaitResult::Signaled) {
                Fatal("ParallelFor helper did not take a batch");
            }

            for (usize index = begin; index < end; ++index) {
                AtomicFetchAdd(&probe.hits[index], 1u);
            }
        });
    probe.result = done.IsOk() ? ErrorCode::None : done.Error();
}

struct AcceptingObserver {
    WorkerScheduler* scheduler;
    ManualResetEvent* gate;
};

void OpenAfterSchedulerCloses(void* raw) noexcept {
    auto& observer = *static_cast<AcceptingObserver*>(raw);
    while (observer.scheduler->Stats().accepting) {
        YieldThread();
    }

    observer.gate->Signal();
}

} // namespace

TEST(ParallelFor_HelperCancelledByShutdownStillCoversRange) {
    SystemAllocator allocator;
    WorkerScheduler scheduler(allocator);
    WorkerSchedulerConfig config;
    config.workerCount = 2;
    REQUIRE(scheduler.Start(config));

    // helper 하나가 batch를 쥔 채 막혀 있는 동안 owner가 Shutdown(Cancel)한다. 그 helper는 쥔
    // batch만 끝내고 돌아오고, 나머지는 호출 스레드가 실행해 범위 전체가 정확히 한 번씩 처리되어야
    // 한다.
    ManualResetEvent helperEntered;
    ManualResetEvent gate;
    static CancelledHelperProbe probe{nullptr, nullptr, nullptr, nullptr};
    probe = CancelledHelperProbe{&scheduler, &allocator, &helperEntered, &gate};
    Thread caller;
    REQUIRE(caller.Start(RunCancelledParallelFor, &probe) == ThreadStatus::Success);
    REQUIRE(helperEntered.Wait(10000) == WaitResult::Signaled);

    AcceptingObserver observer{&scheduler, &gate};
    Thread opener;
    REQUIRE(opener.Start(OpenAfterSchedulerCloses, &observer) == ThreadStatus::Success);
    REQUIRE(scheduler.Shutdown(WorkerShutdownMode::Cancel));
    if (caller.WaitForCompletion(10000) != WaitResult::Signaled ||
        caller.Join() != ThreadStatus::Success || opener.Join() != ThreadStatus::Success) {
        Fatal("ParallelFor cancellation fixture failed to terminate");
    }

    CHECK(probe.result == ErrorCode::None);
    bool once = true;
    for (uint32 hit : probe.hits) {
        once = once && hit == 1;
    }

    CHECK(once);
}
} // namespace parallel_for_tests_file
