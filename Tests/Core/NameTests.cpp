#include <Tests/Support/Test.h>
#include <Tests/Support/NFailAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/String.h>
#include <Platform/Clock.h>
#include <Platform/Thread.h>

using namespace rocks;

namespace {

constexpr uint32 WaitBudgetMs = 10000;
constexpr uint64 WaitBudgetNs = static_cast<uint64>(WaitBudgetMs) * 1000000;
constexpr usize WorkerCount = 8;

// timeout 뒤 worker를 방치하고 stack context를 회수하지 않는다. 교착은 process 실패로 드러낸다.
class TestThread {
public:
    ~TestThread() noexcept {
        Join();
    }

    bool Start(void (*function)(void*) noexcept, void* context) noexcept {
        return thread_.Start(function, context) == platform::ThreadStatus::Success;
    }

    void Join() noexcept {
        if (!thread_.IsJoinable()) {
            return;
        }
        if (thread_.WaitForCompletion(WaitBudgetMs) != platform::WaitResult::Signaled ||
            thread_.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("Name test thread join failed or timed out");
        }
    }

private:
    platform::Thread thread_;
};

class BlockingAllocator final : public Allocator {
public:
    explicit BlockingAllocator(Allocator& backing, bool blockFree = false) noexcept
        : backing_(backing), releaseFree_(!blockFree) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        {
            platform::LockGuard lock(gate_);
            ++allocationsArrived_;
            arrived_.NotifyAll();
        }
        WaitReleased(false);
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        {
            platform::LockGuard lock(gate_);
            ++freesArrived_;
            arrived_.NotifyAll();
        }
        WaitReleased(true);
        return backing_.Deallocate(address);
    }

    bool WaitArrived(usize count, bool freeing = false) noexcept {
        const uint64 start = platform::MonotonicNanoseconds();
        platform::LockGuard lock(gate_);
        for (;;) {
            if ((freeing ? freesArrived_ : allocationsArrived_) >= count) {
                return true;
            }
            const uint64 elapsed = platform::MonotonicNanoseconds() - start;
            if (elapsed >= WaitBudgetNs) {
                return false;
            }
            // wake 자체를 도착 증거로 삼지 않는다. spurious wake마다 predicate를 다시 확인하고
            // 원래 deadline의 남은 시간만 대기하여 timeout을 반복 연장하지 않는다.
            const uint32 remainingMs =
                static_cast<uint32>((WaitBudgetNs - elapsed + 999999) / 1000000);
            if (arrived_.Wait(gate_, remainingMs) == platform::WaitResult::Failed) {
                platform::Fatal("Name test arrival wait failed");
            }
        }
    }

    void ReleaseAllocations() noexcept {
        releaseAllocation_.Signal();
    }

    void ReleaseFrees() noexcept {
        releaseFree_.Signal();
    }

private:
    void WaitReleased(bool freeing) noexcept {
        auto& event = freeing ? releaseFree_ : releaseAllocation_;
        if (event.Wait(WaitBudgetMs) != platform::WaitResult::Signaled) {
            platform::Fatal("Name test allocation barrier failed or timed out");
        }
    }

    Allocator& backing_;
    platform::Mutex gate_;
    platform::ConditionVariable arrived_;
    usize allocationsArrived_ = 0;
    usize freesArrived_ = 0;
    platform::ManualResetEvent releaseAllocation_;
    platform::ManualResetEvent releaseFree_;
};

class InspectingAllocator final : public Allocator {
public:
    explicit InspectingAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        const auto before = pool->Stats();
        sawPending = before.activeOperations == 1 && before.pendingEntries == 1;
        if (closeOnAllocate) {
            auto closing = pool->Shutdown();
            sawAllocateBusy = !closing && closing.Error() == ErrorCode::Busy;
        }
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        const auto before = pool->Stats();
        sawRetainedCleanup = before.reclaiming || before.activeOperations != 0;
        auto closing = pool->Shutdown();
        sawFreeBusy = !closing && closing.Error() == ErrorCode::Busy;
        return backing_.Deallocate(address);
    }

    NamePool* pool = nullptr;
    bool closeOnAllocate = false;
    bool sawPending = false;
    bool sawAllocateBusy = false;
    bool sawRetainedCleanup = false;
    bool sawFreeBusy = false;

private:
    Allocator& backing_;
};

struct InternWork {
    NamePool* pool;
    StringView source;
    Name name{};
    ErrorCode error = ErrorCode::None;
    bool readable = false;
};

void RunIntern(void* raw) noexcept {
    auto& work = *static_cast<InternWork*>(raw);
    auto result = work.pool->TryIntern(work.source);
    if (!result) {
        work.error = result.Error();
        return;
    }

    work.name = result.Value();
    auto lease = work.pool->Acquire(work.name);
    work.readable = lease && lease.Value().View() == work.source;
}

struct ShutdownWork {
    NamePool* pool;
    ErrorCode error = ErrorCode::None;
};

void RunShutdown(void* raw) noexcept {
    auto& work = *static_cast<ShutdownWork*>(raw);
    auto closed = work.pool->Shutdown();
    work.error = closed.Error();
}

struct LeaseWork {
    NameLease lease;
    bool valid = false;
};

void ReturnLease(void* raw) noexcept {
    auto& work = *static_cast<LeaseWork*>(raw);
    work.valid = work.lease.View() == StringView("persistent");
    auto clone = work.lease.TryClone();
    work.valid = work.valid && !clone && clone.Error() == ErrorCode::ShuttingDown;
    work.lease.Reset();
}

Result<void> RunNameLifecycle(Allocator& allocator) noexcept {
    NamePool pool(allocator);
    auto initialized = pool.Initialize();
    if (!initialized) {
        return initialized;
    }
    const StringView names[] = {"", "Alpha", "\xED\x95\x9C\xEA\xB8\x80"};
    for (StringView text : names) {
        auto name = pool.TryIntern(text);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }
        auto lease = pool.Acquire(name.Value());
        if (!lease) {
            return Result<void>::Failure(lease.Error());
        }
        auto clone = lease.Value().TryClone();
        if (!clone || clone.Value().View() != text) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }
    }
    return pool.Shutdown();
}

} // namespace

TEST(NameEmptyIsDistinctFromMissingAndInternOwnsBytes) {
    static_assert(__is_trivially_copyable(Name));
    static_assert(!__is_constructible(NameLease, const NameLease&));
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        CHECK(pool.TryIntern("before initialization").Error() == ErrorCode::ShuttingDown);
        REQUIRE(pool.Initialize());
        CHECK(pool.Initialize().Error() == ErrorCode::AlreadyExists);
        CHECK(allocator.Stats().totalAllocations == 0);
        auto empty = pool.TryIntern(StringView(nullptr));
        REQUIRE(empty);
        CHECK(empty.Value().IsValid());
        CHECK(empty.Value() != Name{});
        auto sameEmpty = pool.TryIntern("");
        REQUIRE(sameEmpty);
        CHECK(sameEmpty.Value() == empty.Value());
        String source(allocator);
        REQUIRE(source.TryAssign("owned"));
        auto token = pool.TryIntern(source.View());
        REQUIRE(token);
        REQUIRE(source.TryAssign("other"));
        auto lease = pool.Acquire(token.Value());
        REQUIRE(lease);
        CHECK(lease.Value().View() == StringView("owned"));
        CHECK(pool.Stats().entryCount == 2);
        lease.Value().Reset();
        REQUIRE(pool.Shutdown());
        CHECK(pool.Stats().storageBytes == 0);
        CHECK(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameForcedHashCollisionsUseExactUtf8Bytes) {
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        NamePoolConfig config;
        config.hashMask = 0;
        REQUIRE(pool.Initialize(config));
        const char embedded[] = {'A', '\0', 'a'};
        const StringView inputs[] = {"Alpha",    "alpha",    "", StringView(embedded, 3),
                                     "\xC3\xA9", "e\xCC\x81"};
        Name tokens[6]{};
        for (usize index = 0; index < 6; ++index) {
            auto name = pool.TryIntern(inputs[index]);
            REQUIRE(name);
            tokens[index] = name.Value();
            for (usize previous = 0; previous < index; ++previous) {
                CHECK(tokens[previous] != tokens[index]);
            }
        }
        const uint64 allocations = allocator.Stats().totalAllocations;
        for (usize index = 0; index < 6; ++index) {
            auto name = pool.TryIntern(inputs[index]);
            REQUIRE(name);
            CHECK(name.Value() == tokens[index]);
            auto lease = pool.Acquire(tokens[index]);
            REQUIRE(lease);
            CHECK(lease.Value().View() == inputs[index]);
        }
        CHECK(allocator.Stats().totalAllocations == allocations);
        CHECK(pool.Stats().entryCount == 6);
        CHECK(pool.TryIntern("\xED\xA0\x80").Error() == ErrorCode::InvalidArgument);
        CHECK(pool.Stats().entryCount == 6);
        CHECK(pool.Stats().activeOperations == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameDomainsRejectForeignAndPreviousSessionTokens) {
    SystemAllocator allocator;
    {
        NamePool first(allocator);
        NamePool second(allocator);
        REQUIRE(first.Initialize());
        REQUIRE(second.Initialize());
        auto firstName = first.TryIntern("same bytes");
        auto secondName = second.TryIntern("same bytes");
        REQUIRE(firstName);
        REQUIRE(secondName);
        CHECK(firstName.Value() != secondName.Value());
        CHECK(first.Acquire(secondName.Value()).Error() == ErrorCode::StaleHandle);
        CHECK(first.Acquire(Name{}).Error() == ErrorCode::StaleHandle);
        Name forged = firstName.Value();
        forged.id += 64;
        CHECK(first.Acquire(forged).Error() == ErrorCode::StaleHandle);
        REQUIRE(first.Shutdown());
        REQUIRE(first.Initialize());
        auto newName = first.TryIntern("same bytes");
        REQUIRE(newName);
        CHECK(newName.Value().domain != firstName.Value().domain);
        CHECK(first.Acquire(firstName.Value()).Error() == ErrorCode::StaleHandle);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameLeaseMoveCloneAndClosedAdmissionPreserveViews) {
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        REQUIRE(pool.Initialize());
        auto token = pool.TryIntern("persistent");
        REQUIRE(token);
        auto acquired = pool.Acquire(token.Value());
        REQUIRE(acquired);
        NameLease moved(Move(acquired.Value()));
        CHECK(!acquired.Value());
        CHECK(acquired.Value().View().IsEmpty());
        auto clone = moved.TryClone();
        REQUIRE(clone);
        CHECK(pool.Stats().activeLeases == 2);
        NameLease destination;
        destination = Move(clone.Value());
        NameLease* self = &destination;
        destination = Move(*self);
        CHECK(destination.Identifier() == token.Value());
        CHECK(pool.Shutdown().Error() == ErrorCode::Busy);
        CHECK(pool.Initialize().Error() == ErrorCode::Busy);
        CHECK(!pool.Stats().accepting);
        CHECK(pool.Acquire(token.Value()).Error() == ErrorCode::ShuttingDown);
        CHECK(pool.TryIntern("new").Error() == ErrorCode::ShuttingDown);
        CHECK(moved.TryClone().Error() == ErrorCode::ShuttingDown);
        CHECK(moved.View() == StringView("persistent"));
        moved.Reset();
        moved.Reset();
        CHECK(pool.Shutdown().Error() == ErrorCode::Busy);
        LeaseWork work{Move(destination)};
        TestThread thread;
        if (!thread.Start(ReturnLease, &work)) {
            work.lease.Reset();
            CHECK(false);
        } else {
            thread.Join();
            CHECK(work.valid);
        }
        CHECK(pool.Stats().activeLeases == 0);
        REQUIRE(pool.Shutdown());
        CHECK(NameLease{}.TryClone().Error() == ErrorCode::StaleHandle);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameEntryAndStorageLimitsAreAtomic) {
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        NamePoolConfig config;
        config.maxEntries = 1;
        REQUIRE(pool.Initialize(config));
        auto name = pool.TryIntern("same");
        REQUIRE(name);
        const usize entryBytes = pool.Stats().storageBytes;
        CHECK(pool.TryIntern("next").Error() == ErrorCode::CapacityExceeded);
        auto existing = pool.TryIntern("same");
        REQUIRE(existing);
        CHECK(existing.Value() == name.Value());
        CHECK(pool.Stats().entryCount == 1);
        REQUIRE(pool.Shutdown());
        config.maxEntries = 10;
        config.maxStorageBytes = entryBytes;
        REQUIRE(pool.Initialize(config));
        REQUIRE(pool.TryIntern("same"));
        CHECK(pool.TryIntern("next").Error() == ErrorCode::CapacityExceeded);
        CHECK(pool.Stats().storageBytes == entryBytes);
        REQUIRE(pool.Shutdown());
        config.maxStorageBytes = entryBytes - 1;
        REQUIRE(pool.Initialize(config));
        CHECK(pool.TryIntern("same").Error() == ErrorCode::CapacityExceeded);
        CHECK(pool.Stats().entryCount == 0);
        CHECK(pool.Stats().pendingEntries == 0);
        REQUIRE(pool.Shutdown());
        config.maxEntries = 0;
        config.maxStorageBytes = MaxSize;
        REQUIRE(pool.Initialize(config));
        CHECK(pool.TryIntern("").Error() == ErrorCode::CapacityExceeded);
        CHECK(pool.Stats().storageBytes == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameOomPreservesExistingIdentityAndReleasesReservation) {
    SystemAllocator allocator;
    test::NFailAllocator faults(allocator);
    {
        NamePool pool(faults);
        REQUIRE(pool.Initialize());
        auto original = pool.TryIntern("kept");
        REQUIRE(original);
        const auto before = pool.Stats();
        faults.FailOnAttempt(1);
        CHECK(pool.TryIntern("new bytes").Error() == ErrorCode::OutOfMemory);
        auto existing = pool.TryIntern("kept");
        REQUIRE(existing);
        CHECK(existing.Value() == original.Value());
        CHECK(faults.Attempts() == 1);
        const auto after = pool.Stats();
        CHECK(after.entryCount == before.entryCount);
        CHECK(after.storageBytes == before.storageBytes);
        CHECK(after.pendingEntries == 0);
        CHECK(after.pendingStorageBytes == 0);
        CHECK(after.activeOperations == 0);
        faults.DisableFailure();
        REQUIRE(pool.TryIntern("new bytes"));
        CHECK(pool.Stats().entryCount == 2);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameConcurrentInternSharesIdentityDespiteForcedCollisions) {
    SystemAllocator allocator;
    BlockingAllocator blocking(allocator);
    {
        NamePool pool(blocking);
        NamePoolConfig config;
        config.hashMask = 0;
        REQUIRE(pool.Initialize(config));
        InternWork work[WorkerCount]{};
        TestThread threads[WorkerCount];
        usize started = 0;
        for (usize index = 0; index < WorkerCount; ++index) {
            work[index].pool = &pool;
            work[index].source = index % 2 == 0 ? StringView("shared") : StringView("different");
            if (threads[index].Start(RunIntern, &work[index])) {
                ++started;
            }
        }
        const bool allArrived = blocking.WaitArrived(started);
        const auto pending = pool.Stats();
        blocking.ReleaseAllocations();
        for (auto& thread : threads) {
            thread.Join();
        }
        CHECK(started == WorkerCount);
        CHECK(allArrived);
        CHECK(pending.pendingEntries == started);
        CHECK(pending.activeOperations == started);
        CHECK(pending.entryCount == 0);
        if (started == WorkerCount) {
            for (usize index = 0; index < WorkerCount; ++index) {
                CHECK(work[index].error == ErrorCode::None);
                CHECK(work[index].readable);
                CHECK(work[index].name == work[index % 2].name);
            }
            CHECK(work[0].name != work[1].name);
            CHECK(pool.Stats().entryCount == 2);
            CHECK(allocator.Stats().liveAllocations == 2);
        }
        CHECK(pool.Stats().pendingEntries == 0);
        CHECK(pool.Stats().pendingStorageBytes == 0);
        CHECK(pool.Stats().activeOperations == 0);
        CHECK(pool.Stats().activeLeases == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameShutdownCannotOvertakeAllocationOrCancelledCandidateCleanup) {
    SystemAllocator allocator;
    BlockingAllocator blocking(allocator, true);
    {
        NamePool pool(blocking);
        REQUIRE(pool.Initialize());
        InternWork work{&pool, "cancelled"};
        TestThread thread;
        REQUIRE(thread.Start(RunIntern, &work));
        const bool allocationArrived = blocking.WaitArrived(1);
        const auto whileAllocating = pool.Shutdown();
        blocking.ReleaseAllocations();
        const bool freeArrived = blocking.WaitArrived(1, true);
        const auto duringCleanup = pool.Stats();
        const auto whileFreeing = pool.Shutdown();
        const uint64 liveDuringCleanup = allocator.Stats().liveAllocations;
        blocking.ReleaseFrees();
        thread.Join();
        CHECK(allocationArrived);
        CHECK(freeArrived);
        CHECK(whileAllocating.Error() == ErrorCode::Busy);
        CHECK(whileFreeing.Error() == ErrorCode::Busy);
        CHECK(duringCleanup.activeOperations == 1);
        CHECK(duringCleanup.pendingEntries == 1);
        CHECK(duringCleanup.entryCount == 0);
        CHECK(liveDuringCleanup == 1);
        CHECK(work.error == ErrorCode::ShuttingDown);
        CHECK(pool.Stats().activeOperations == 0);
        CHECK(pool.Stats().pendingEntries == 0);
        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameAllocatorCallbacksRunOutsidePoolGate) {
    SystemAllocator allocator;
    {
        InspectingAllocator inspecting(allocator);
        NamePool pool(inspecting);
        inspecting.pool = &pool;
        inspecting.closeOnAllocate = true;
        REQUIRE(pool.Initialize());
        InternWork work{&pool, "cancel through callback"};
        TestThread intern;
        REQUIRE(intern.Start(RunIntern, &work));
        intern.Join();
        CHECK(work.error == ErrorCode::ShuttingDown);
        CHECK(inspecting.sawPending);
        CHECK(inspecting.sawAllocateBusy);
        CHECK(inspecting.sawRetainedCleanup);
        CHECK(inspecting.sawFreeBusy);
        REQUIRE(pool.Shutdown());
        inspecting.closeOnAllocate = false;
        REQUIRE(pool.Initialize());
        // 정상 entry 회수 중에도 callback의 Shutdown은 Reclaiming을 보고 Busy로 복귀해야 한다.
        InternWork published{&pool, "published"};
        TestThread secondIntern;
        REQUIRE(secondIntern.Start(RunIntern, &published));
        secondIntern.Join();
        CHECK(published.error == ErrorCode::None);
        inspecting.sawRetainedCleanup = false;
        inspecting.sawFreeBusy = false;
        ShutdownWork closing{&pool};
        TestThread shutdown;
        REQUIRE(shutdown.Start(RunShutdown, &closing));
        shutdown.Join();
        CHECK(closing.error == ErrorCode::None);
        CHECK(inspecting.sawRetainedCleanup);
        CHECK(inspecting.sawFreeBusy);
        CHECK(!pool.Stats().reclaiming);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameAllAllocationFailuresAndRepeatedSessionsLeaveNoLeaks) {
    SystemAllocator allocator;
    test::NFailAllocator faults(allocator);
    REQUIRE(RunNameLifecycle(faults));
    const uint64 allocationCount = faults.Attempts();
    CHECK(allocationCount == 3);
    CHECK(allocator.Stats().liveAllocations == 0);
    for (uint64 attempt = 1; attempt <= allocationCount; ++attempt) {
        faults.FailOnAttempt(attempt);
        auto completed = RunNameLifecycle(faults);
        CHECK(completed.Error() == ErrorCode::OutOfMemory);
        CHECK(faults.Attempts() == attempt);
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().liveBytes == 0);
    }
    for (usize cycle = 0; cycle < 1000; ++cycle) {
        REQUIRE(RunNameLifecycle(allocator));
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().liveBytes == 0);
    }
}
