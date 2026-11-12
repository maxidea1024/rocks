#include <Core/Memory/BudgetAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Fatal.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>

using namespace rocks;

namespace {

void JoinBudgetWorker(platform::Thread& worker) noexcept {
    if (worker.WaitForCompletion(15000) != platform::WaitResult::Signaled ||
        worker.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("Budget concurrency test worker did not finish");
    }
}

struct Contention {
    explicit Contention(BudgetAllocator& allocator) noexcept : budget(allocator) {
    }

    BudgetAllocator& budget;
    platform::ManualResetEvent start;
    platform::ManualResetEvent attempted;
    platform::ManualResetEvent release;
    platform::Mutex mutex;
    uint32 attempts = 0;
};

struct ContentionWorker {
    Contention* shared;
    uint8 pattern;
    ErrorCode allocation = ErrorCode::None;
    ErrorCode release = ErrorCode::None;
    bool payloadValid = false;
    bool waitSucceeded = true;
};

void RunContention(void* raw) noexcept {
    auto& worker = *static_cast<ContentionWorker*>(raw);
    auto& shared = *worker.shared;
    if (shared.start.Wait(10000) != platform::WaitResult::Signaled) {
        worker.waitSucceeded = false;
        return;
    }

    auto allocation = shared.budget.Allocate(32, 16);
    worker.allocation = allocation.Error();
    auto* bytes = allocation ? static_cast<uint8*>(allocation.Value()) : nullptr;
    if (bytes) {
        for (usize i = 0; i < 32; ++i) {
            bytes[i] = worker.pattern;
        }
    }

    {
        platform::LockGuard lock(shared.mutex);
        ++shared.attempts;
        if (shared.attempts == 4) {
            shared.attempted.Signal();
        }
    }

    if (shared.release.Wait(10000) != platform::WaitResult::Signaled) {
        worker.waitSucceeded = false;
    }
    if (bytes) {
        worker.payloadValid = true;
        for (usize i = 0; i < 32; ++i) {
            if (bytes[i] != worker.pattern) {
                worker.payloadValid = false;
            }
        }
        worker.release = shared.budget.Deallocate(bytes).Error();
    }
}

// 최초 상위 할당이 진행되는 동안 다른 작업이 대기하는 순서를 제어한다.
class BlockingBacking final : public Allocator {
public:
    explicit BlockingBacking(Allocator& allocator) noexcept : backing_(allocator) {
    }

    platform::ManualResetEvent entered;
    platform::ManualResetEvent release;
    bool first = true;

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (first) {
            first = false;
            entered.Signal();
            if (release.Wait(10000) != platform::WaitResult::Signaled) {
                return Result<void*>::Failure(ErrorCode::DependencyFailed);
            }
        }
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return backing_.Deallocate(address);
    }

private:
    Allocator& backing_;
};

struct WaitingAllocation {
    BudgetAllocator* budget;
    void* address = nullptr;
    ErrorCode error = ErrorCode::None;
};

void RunWaitingAllocation(void* raw) noexcept {
    auto& state = *static_cast<WaitingAllocation*>(raw);
    auto result = state.budget->Allocate(80, 16);
    state.error = result.Error();
    if (result) {
        state.address = result.Value();
    }
}

struct WaitingLimit {
    explicit WaitingLimit(BudgetAllocator& allocator) noexcept : budget(&allocator) {
    }

    BudgetAllocator* budget;
    platform::ManualResetEvent started;
    platform::ManualResetEvent finished;
    ErrorCode error = ErrorCode::None;
};

void RunWaitingLimit(void* raw) noexcept {
    auto& state = *static_cast<WaitingLimit*>(raw);
    state.started.Signal();
    state.error = state.budget->SetLimit(40).Error();
    state.finished.Signal();
}

// 상위 콜백의 읽기 스냅샷과 빠른 무효 연산은 상태를 바꾸지 않는다.
class SnapshotBacking final : public Allocator {
public:
    explicit SnapshotBacking(Allocator& allocator) noexcept : backing_(allocator) {
    }

    BudgetAllocator* target = nullptr;
    uint32 observations = 0;
    uint32 failures = 0;

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (target) {
            BudgetAllocator* current = target;
            target = nullptr;
            const auto stats = current->Stats();
            auto zero = current->Allocate(0, 8);
            auto invalid = current->Allocate(0, 3);
            auto nested = current->Allocate(1, 1);
            auto emptyRelease = current->Deallocate(nullptr);
            auto change = current->SetLimit(512);
            if (stats.liveAllocations != 0 || stats.liveBytes != 0 ||
                current->LimitBytes() != 256 || current->PeakBytes() != 0 || !zero ||
                zero.Value() != nullptr || invalid.Error() != ErrorCode::InvalidArgument ||
                nested.Error() != ErrorCode::Busy || !emptyRelease ||
                change.Error() != ErrorCode::Busy) {
                ++failures;
            }
            if (nested && nested.Value()) {
                (void)current->Deallocate(nested.Value());
            }
            ++observations;
            target = current;
        }
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return backing_.Deallocate(address);
    }

private:
    Allocator& backing_;
};

} // namespace

TEST(CoreBudgetConcurrentCallsRespectSharedLimit) {
    SystemAllocator backing;
    {
        BudgetAllocator budget(backing, 64);
        Contention shared(budget);
        ContentionWorker work[4] = {{&shared, 11}, {&shared, 22}, {&shared, 33}, {&shared, 44}};
        platform::Thread threads[4];
        for (usize i = 0; i < 4; ++i) {
            if (threads[i].Start(RunContention, &work[i]) != platform::ThreadStatus::Success) {
                platform::Fatal("Budget contention worker failed to start");
            }
        }

        shared.start.Signal();
        const bool allAttempted = shared.attempted.Wait(10000) == platform::WaitResult::Signaled;
        if (allAttempted) {
            const auto snapshot = budget.Stats();
            CHECK(snapshot.liveAllocations == 2);
            CHECK(snapshot.liveBytes == 64);
            CHECK(budget.PeakBytes() == 64);
        }
        shared.release.Signal();
        for (auto& worker : threads) {
            JoinBudgetWorker(worker);
        }

        CHECK(allAttempted);
        uint32 successes = 0;
        uint32 limited = 0;
        for (const auto& worker : work) {
            CHECK(worker.waitSucceeded);
            if (worker.allocation == ErrorCode::None) {
                ++successes;
                CHECK(worker.payloadValid);
                CHECK(worker.release == ErrorCode::None);
            } else if (worker.allocation == ErrorCode::CapacityExceeded) {
                ++limited;
            } else {
                CHECK(false);
            }
        }
        CHECK(successes == 2);
        CHECK(limited == 2);
        CHECK(budget.Stats().liveAllocations == 0);
        CHECK(budget.Stats().liveBytes == 0);
        CHECK(budget.Stats().totalAllocations == 2);
    }
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

TEST(CoreBudgetSetLimitWaitsForPublishedAllocation) {
    SystemAllocator system;
    BlockingBacking backing(system);
    {
        BudgetAllocator budget(backing, 128);
        WaitingAllocation allocation{&budget};
        WaitingLimit limit(budget);
        platform::Thread allocating;
        platform::Thread setting;
        if (allocating.Start(RunWaitingAllocation, &allocation) !=
            platform::ThreadStatus::Success) {
            platform::Fatal("Budget allocation worker failed to start");
        }

        const bool allocationEntered =
            backing.entered.Wait(10000) == platform::WaitResult::Signaled;
        if (!allocationEntered) {
            backing.release.Signal();
            JoinBudgetWorker(allocating);
            if (allocation.address) {
                (void)budget.Deallocate(allocation.address);
            }
            CHECK(false);
            return;
        }

        CHECK(budget.Stats().liveAllocations == 0);
        CHECK(budget.Stats().liveBytes == 0);
        CHECK(budget.LimitBytes() == 128);
        if (setting.Start(RunWaitingLimit, &limit) != platform::ThreadStatus::Success) {
            backing.release.Signal();
            JoinBudgetWorker(allocating);
            if (allocation.address) {
                (void)budget.Deallocate(allocation.address);
            }
            platform::Fatal("Budget limit worker failed to start");
        }

        const bool limitStarted = limit.started.Wait(10000) == platform::WaitResult::Signaled;
        const auto early = limit.finished.Wait(50);
        backing.release.Signal();
        JoinBudgetWorker(allocating);
        JoinBudgetWorker(setting);

        CHECK(limitStarted);
        CHECK(early == platform::WaitResult::TimedOut);
        CHECK(allocation.error == ErrorCode::None);
        CHECK(allocation.address != nullptr);
        CHECK(limit.error == ErrorCode::CapacityExceeded);
        CHECK(budget.LimitBytes() == 128);
        CHECK(budget.Stats().liveBytes == 80);
        if (allocation.address) {
            REQUIRE(budget.Deallocate(allocation.address));
        }
        CHECK(budget.Stats().liveAllocations == 0);
    }
    CHECK(system.Stats().liveAllocations == 0);
    CHECK(system.Stats().liveBytes == 0);
}

TEST(CoreBudgetBackingCallbackCanReadSnapshotAndNoop) {
    SystemAllocator system;
    SnapshotBacking backing(system);
    {
        BudgetAllocator budget(backing, 256);
        backing.target = &budget;
        auto result = budget.Allocate(32, 16);
        REQUIRE(result);
        CHECK(backing.observations == 2);
        CHECK(backing.failures == 0);
        CHECK(budget.Stats().liveAllocations == 1);
        CHECK(budget.Stats().liveBytes == 32);
        CHECK(budget.PeakBytes() == 32);
        backing.target = nullptr;
        REQUIRE(budget.Deallocate(result.Value()));
    }
    CHECK(system.Stats().liveAllocations == 0);
    CHECK(system.Stats().liveBytes == 0);
}
