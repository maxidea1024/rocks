#include <Core/Containers/Array.h>
#include <Core/Memory/ArenaAllocator.h>
#include <Core/Memory/BudgetAllocator.h>
#include <Core/Memory/PoolAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <Tests/Support/HostFailure.h>
#include <Tests/Support/NFailAllocator.h>
#include <Tests/Support/Test.h>

using namespace rocks;

namespace {

// 상위 할당기 콜백에서 동일 할당기를 호출하면 교착 대신 오류를 반환해야 한다.
class ReentrantBacking final : public Allocator {
public:
    explicit ReentrantBacking(Allocator& backing) noexcept : backing_(backing) {
    }

    Allocator* target = nullptr;
    ErrorCode allocateReentry = ErrorCode::None;
    ErrorCode freeReentry = ErrorCode::None;

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (target) {
            Allocator* current = target;
            target = nullptr;
            auto nested = current->Allocate(1, 1);
            allocateReentry = nested.Error();
            if (nested && nested.Value()) {
                (void)current->Deallocate(nested.Value());
            }
            target = current;
        }

        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (target) {
            Allocator* current = target;
            target = nullptr;
            auto nested = current->Deallocate(address);
            freeReentry = nested.Error();
            target = current;
        }

        return backing_.Deallocate(address);
    }

private:
    Allocator& backing_;
};

struct WrongThreadArena {
    ArenaAllocator* arena;
    ErrorCode allocate = ErrorCode::None;
    ErrorCode release = ErrorCode::None;
    ErrorCode reset = ErrorCode::None;
    ErrorCode trim = ErrorCode::None;
};

void QueryArenaFromOtherThread(void* context) noexcept {
    auto& query = *static_cast<WrongThreadArena*>(context);
    query.allocate = query.arena->Allocate(0, 3).Error();
    query.release = query.arena->Deallocate(nullptr).Error();
    query.reset = query.arena->Reset().Error();
    query.trim = query.arena->Trim().Error();
}

void JoinOrFatal(platform::Thread& thread) noexcept {
    if (thread.WaitForCompletion(20000) != platform::WaitResult::Signaled ||
        thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("Allocator test worker failed to finish");
    }
}

struct ConcurrentAllocatorWork {
    Allocator* allocator;
    uint32 number;
    uint32 failures = 0;
};

void RunConcurrentAllocatorWork(void* context) noexcept {
    auto& work = *static_cast<ConcurrentAllocatorWork*>(context);
    for (uint32 iteration = 0; iteration < 500; ++iteration) {
        auto allocation = work.allocator->Allocate(32, 16, work.number);
        if (!allocation || !allocation.Value()) {
            ++work.failures;
            return;
        }

        auto* bytes = static_cast<uint8*>(allocation.Value());
        for (usize i = 0; i < 32; ++i) {
            bytes[i] = static_cast<uint8>(work.number + i);
        }
        platform::YieldThread();
        for (usize i = 0; i < 32; ++i) {
            if (bytes[i] != static_cast<uint8>(work.number + i)) {
                ++work.failures;
            }
        }

        if (!work.allocator->Deallocate(bytes)) {
            ++work.failures;
            return;
        }
    }
}

} // namespace

TEST(CoreArenaAllocationLifetimeAlignmentAndReuse) {
    SystemAllocator backing;
    {
        ArenaAllocator arena(backing, 128);
        CHECK(arena.Allocate(0, 8).Value() == nullptr);
        CHECK(arena.Allocate(0, 3).Error() == ErrorCode::InvalidArgument);
        CHECK(arena.Allocate(MaxSize, 64).Error() == ErrorCode::Overflow);

        void* pointers[8]{};
        for (usize i = 0; i < 8; ++i) {
            auto block = arena.Allocate(17 + i, static_cast<usize>(1) << i);
            REQUIRE(block);
            pointers[i] = block.Value();
            CHECK(reinterpret_cast<uptr>(pointers[i]) % (static_cast<usize>(1) << i) == 0);
            static_cast<uint8*>(pointers[i])[0] = static_cast<uint8>(i + 10);
        }
        CHECK(arena.Stats().allocations.liveAllocations == 8);
        CHECK(arena.Reset().Error() == ErrorCode::Busy);
        CHECK(arena.Trim().Error() == ErrorCode::Busy);
        for (usize i = 0; i < 8; ++i) {
            CHECK(static_cast<uint8*>(pointers[i])[0] == static_cast<uint8>(i + 10));
        }

        int32 foreign = 0;
        CHECK(arena.Deallocate(&foreign).Error() == ErrorCode::NotFound);
        CHECK(arena.Deallocate(static_cast<uint8*>(pointers[0]) + 1).Error() ==
              ErrorCode::NotFound);
        for (void* pointer : pointers) {
            REQUIRE(arena.Deallocate(pointer));
        }
        CHECK(arena.Deallocate(pointers[0]).Error() == ErrorCode::NotFound);
        const auto before = arena.Stats();
        CHECK(before.allocations.liveAllocations == 0);
        CHECK(before.allocations.liveBytes == 0);
        CHECK(before.chunks != 0);
        REQUIRE(arena.Reset());
        CHECK(arena.Stats().chunks == before.chunks);
        auto reused = arena.Allocate(8, 8);
        REQUIRE(reused);
        CHECK(arena.Stats().chunks == before.chunks);
        REQUIRE(arena.Deallocate(reused.Value()));
        REQUIRE(arena.Trim());
        CHECK(arena.Stats().chunks == 0);
        CHECK(arena.Stats().reservedBytes == 0);
    }
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

TEST(CoreArenaWorksWithContainersAndRejectsOtherThreads) {
    SystemAllocator backing;
    {
        ArenaAllocator arena(backing, 256);
        {
            Array<int32> values(arena);
            for (int32 i = 0; i < 80; ++i) {
                REQUIRE(values.TryPushBack(i));
            }
            CHECK(arena.Reset().Error() == ErrorCode::Busy);
            for (int32 i = 0; i < 80; ++i) {
                CHECK(values[static_cast<usize>(i)] == i);
            }
        }
        REQUIRE(arena.Reset());
        WrongThreadArena query{&arena};
        platform::Thread thread;
        if (thread.Start(QueryArenaFromOtherThread, &query) != platform::ThreadStatus::Success) {
            platform::Fatal("Arena test worker failed to start");
        }
        JoinOrFatal(thread);
        CHECK(query.allocate == ErrorCode::WrongThread);
        CHECK(query.release == ErrorCode::WrongThread);
        CHECK(query.reset == ErrorCode::WrongThread);
        CHECK(query.trim == ErrorCode::WrongThread);
        REQUIRE(arena.Trim());
    }
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(CoreArenaPoolBudgetBackingFailurePreservesState) {
    SystemAllocator backing;
    test::NFailAllocator failing(backing);
    {
        ArenaAllocator arena(failing, 128);
        failing.FailOnAttempt(1);
        CHECK(arena.Allocate(8, 8).Error() == ErrorCode::OutOfMemory);
        CHECK(arena.Stats().chunks == 0);
        failing.DisableFailure();
        auto owned = arena.Allocate(8, 8);
        REQUIRE(owned);
        static_cast<uint8*>(owned.Value())[0] = 77;
        const auto snapshot = arena.Stats();
        failing.FailOnAttempt(1);
        CHECK(arena.Allocate(4096, 64).Error() == ErrorCode::OutOfMemory);
        CHECK(arena.Stats().chunks == snapshot.chunks);
        CHECK(static_cast<uint8*>(owned.Value())[0] == 77);
        REQUIRE(arena.Deallocate(owned.Value()));
        failing.DisableFailure();
        REQUIRE(arena.Trim());
    }
    {
        PoolAllocator pool(failing, 64, 64, 4);
        failing.FailOnAttempt(1);
        CHECK(pool.Allocate(16, 16).Error() == ErrorCode::OutOfMemory);
        CHECK(pool.Capacity() == 0);
        CHECK(pool.Stats().liveAllocations == 0);
        failing.DisableFailure();
        auto owned = pool.Allocate(16, 16);
        REQUIRE(owned);
        REQUIRE(pool.Deallocate(owned.Value()));
        REQUIRE(pool.Trim());
    }
    {
        BudgetAllocator budget(failing, 1024);
        for (uint64 failure = 1; failure <= 2; ++failure) {
            failing.FailOnAttempt(failure);
            CHECK(budget.Allocate(16, 8).Error() == ErrorCode::OutOfMemory);
            CHECK(budget.Stats().liveAllocations == 0);
            CHECK(budget.Stats().liveBytes == 0);
            CHECK(backing.Stats().liveAllocations == 0);
        }
        failing.DisableFailure();
        auto owned = budget.Allocate(16, 8);
        REQUIRE(owned);
        REQUIRE(budget.Deallocate(owned.Value()));
    }
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

TEST(CorePoolReuseOwnershipAndShutdown) {
    SystemAllocator backing;
    {
        PoolAllocator pool(backing, 64, 64, 4);
        CHECK(pool.Allocate(0, 8).Value() == nullptr);
        CHECK(pool.Allocate(0, 3).Error() == ErrorCode::InvalidArgument);
        CHECK(pool.Allocate(65, 8).Error() == ErrorCode::Unsupported);
        CHECK(pool.Allocate(8, 128).Error() == ErrorCode::Unsupported);
        void* blocks[9]{};
        for (usize index = 0; index < 9; ++index) {
            auto block = pool.Allocate(32, 64);
            REQUIRE(block);
            blocks[index] = block.Value();
            CHECK(reinterpret_cast<uptr>(blocks[index]) % 64 == 0);
            for (usize prior = 0; prior < index; ++prior) {
                CHECK(blocks[index] != blocks[prior]);
            }
        }
        CHECK(pool.Capacity() >= 9);
        CHECK(pool.ChunkCount() == 3);
        CHECK(pool.Stats().liveAllocations == 9);
        CHECK(pool.Trim().Error() == ErrorCode::Busy);
        CHECK(pool.Shutdown().Error() == ErrorCode::Busy);
        int32 foreign = 0;
        CHECK(pool.Deallocate(&foreign).Error() == ErrorCode::NotFound);
        CHECK(pool.Deallocate(static_cast<uint8*>(blocks[0]) + 1).Error() == ErrorCode::NotFound);
        REQUIRE(pool.Deallocate(blocks[0]));
        CHECK(pool.Deallocate(blocks[0]).Error() == ErrorCode::NotFound);
        auto reused = pool.Allocate(32, 64);
        REQUIRE(reused);
        CHECK(reused.Value() == blocks[0]);
        REQUIRE(pool.Deallocate(reused.Value()));
        for (usize index = 1; index < 9; ++index) {
            REQUIRE(pool.Deallocate(blocks[index]));
        }
        REQUIRE(pool.Trim());
        CHECK(pool.Capacity() == 0);
        CHECK(pool.ChunkCount() == 0);
        auto again = pool.Allocate(8, 8);
        REQUIRE(again);
        REQUIRE(pool.Deallocate(again.Value()));
        REQUIRE(pool.Shutdown());
        REQUIRE(pool.Shutdown());
        CHECK(pool.Allocate(8, 8).Error() == ErrorCode::ShuttingDown);
        CHECK(pool.Allocate(65, 8).Error() == ErrorCode::Unsupported);
        CHECK(pool.Allocate(0, 3).Error() == ErrorCode::InvalidArgument);
        CHECK(pool.Allocate(0, 8).Value() == nullptr);
        CHECK(pool.Trim().Error() == ErrorCode::ShuttingDown);
    }
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

TEST(CorePoolConfigurationOverflowDoesNotAllocate) {
    SystemAllocator backing;
    {
        PoolAllocator pool(backing, MaxSize, 64, 2);
        CHECK(pool.Allocate(1, 1).Error() == ErrorCode::Overflow);
        CHECK(pool.Capacity() == 0);
        CHECK(pool.Stats().liveAllocations == 0);
    }
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(CoreBudgetLimitOwnershipAndRecovery) {
    SystemAllocator backing;
    {
        BudgetAllocator budget(backing, 100);
        CHECK(budget.Allocate(0, 8).Value() == nullptr);
        CHECK(budget.Allocate(0, 3).Error() == ErrorCode::InvalidArgument);
        CHECK(budget.Allocate(MaxSize, 64).Error() == ErrorCode::Overflow);
        auto first = budget.Allocate(60, 32, 7);
        REQUIRE(first);
        auto second = budget.Allocate(40, 16, 8);
        REQUIRE(second);
        CHECK(reinterpret_cast<uptr>(first.Value()) % 32 == 0);
        CHECK(budget.Stats().liveBytes == 100);
        CHECK(budget.PeakBytes() == 100);
        CHECK(budget.Allocate(1, 1).Error() == ErrorCode::CapacityExceeded);
        CHECK(budget.SetLimit(99).Error() == ErrorCode::CapacityExceeded);
        CHECK(budget.LimitBytes() == 100);
        auto outsider = backing.Allocate(8, 8);
        REQUIRE(outsider);
        CHECK(budget.Deallocate(outsider.Value()).Error() == ErrorCode::NotFound);
        REQUIRE(backing.Deallocate(outsider.Value()));
        REQUIRE(budget.Deallocate(first.Value()));
        CHECK(budget.Deallocate(first.Value()).Error() == ErrorCode::NotFound);
        REQUIRE(budget.SetLimit(40));
        CHECK(budget.LimitBytes() == 40);
        REQUIRE(budget.Deallocate(second.Value()));
        CHECK(budget.Stats().liveBytes == 0);
        CHECK(budget.PeakBytes() == 100);
        REQUIRE(budget.SetLimit(0));
        CHECK(budget.Allocate(1, 1).Error() == ErrorCode::CapacityExceeded);
    }
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

TEST(CoreAllocatorReentrantBackingIsRejectedWithoutDeadlock) {
    SystemAllocator system;
    {
        ReentrantBacking backing(system);
        ArenaAllocator arena(backing, 128);
        backing.target = &arena;
        auto block = arena.Allocate(8, 8);
        REQUIRE(block);
        CHECK(backing.allocateReentry == ErrorCode::Busy);
        REQUIRE(arena.Deallocate(block.Value()));
        REQUIRE(arena.Trim());
        CHECK(backing.freeReentry == ErrorCode::Busy);
        backing.target = nullptr;
    }
    {
        ReentrantBacking backing(system);
        PoolAllocator pool(backing, 32, 16, 4);
        backing.target = &pool;
        auto block = pool.Allocate(8, 8);
        REQUIRE(block);
        CHECK(backing.allocateReentry == ErrorCode::Busy);
        REQUIRE(pool.Deallocate(block.Value()));
        REQUIRE(pool.Trim());
        CHECK(backing.freeReentry == ErrorCode::Busy);
        backing.target = nullptr;
    }
    {
        ReentrantBacking backing(system);
        BudgetAllocator budget(backing, 100);
        backing.target = &budget;
        auto block = budget.Allocate(8, 8);
        REQUIRE(block);
        CHECK(backing.allocateReentry == ErrorCode::Busy);
        REQUIRE(budget.Deallocate(block.Value()));
        CHECK(backing.freeReentry == ErrorCode::Busy);
        backing.target = nullptr;
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(CorePoolAndBudgetSupportConcurrentCallers) {
    SystemAllocator system;
    {
        BudgetAllocator budget(system, 1024 * 1024);
        PoolAllocator pool(budget, 64, 64, 8);
        ConcurrentAllocatorWork work[4] = {{&pool, 0}, {&pool, 1}, {&pool, 2}, {&pool, 3}};
        platform::Thread threads[4];
        for (usize i = 0; i < 4; ++i) {
            if (threads[i].Start(RunConcurrentAllocatorWork, &work[i]) !=
                platform::ThreadStatus::Success) {
                platform::Fatal("Allocator test worker failed to start");
            }
        }
        for (auto& thread : threads) {
            JoinOrFatal(thread);
        }
        for (const auto& item : work) {
            CHECK(item.failures == 0);
        }
        CHECK(pool.Stats().liveAllocations == 0);
        CHECK(pool.Stats().totalAllocations == 2000);
        REQUIRE(pool.Trim());
        CHECK(budget.Stats().liveAllocations == 0);
    }
    CHECK(system.Stats().liveAllocations == 0);
    CHECK(system.Stats().liveBytes == 0);
}

namespace rocks::test {

// 전용 child에서만 호출한다. 대상 할당기를 살아 있는 할당과 함께 파괴하면 Fatal이다.
int RunMemoryAllocatorNegativeControl(uint32 operation) noexcept {
    PrepareFatalTestProcess();
    SystemAllocator backing;
    if (operation == 0) {
        ArenaAllocator arena(backing, 128);
        if (!arena.Allocate(8, 8)) {
            return 0;
        }
    } else if (operation == 1) {
        PoolAllocator pool(backing, 32, 16, 4);
        if (!pool.Allocate(8, 8)) {
            return 0;
        }
    } else if (operation == 2) {
        BudgetAllocator budget(backing, 128);
        if (!budget.Allocate(8, 8)) {
            return 0;
        }
    }
    return 0;
}

} // namespace rocks::test
