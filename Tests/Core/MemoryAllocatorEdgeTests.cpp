#include <Core/Memory/ArenaAllocator.h>
#include <Core/Memory/BudgetAllocator.h>
#include <Core/Memory/ExternalAllocator.h>
#include <Core/Memory/PoolAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <Tests/Support/HostFailure.h>
#include <Tests/Support/NFailAllocator.h>
#include <Tests/Support/Test.h>

using namespace rocks;

namespace {

// 실패한 반환은 실제 메모리를 유지해야 호출자가 같은 주소를 재시도할 수 있다.
class FailOneRelease final : public Allocator {
public:
    explicit FailOneRelease(Allocator& backing) noexcept : backing_(backing) {
    }

    void* failAddress = nullptr;
    bool failOnce = false;

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (address == failAddress && failOnce) {
            failOnce = false;
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }
        return backing_.Deallocate(address);
    }

private:
    Allocator& backing_;
};

struct ForeignPoolWork {
    PoolAllocator* pool;
    BudgetAllocator* budget;
    ErrorCode poolError = ErrorCode::None;
    ErrorCode budgetError = ErrorCode::None;
};

void UseThreadBoundBacking(void* context) noexcept {
    auto& work = *static_cast<ForeignPoolWork*>(context);
    auto pool = work.pool->Allocate(8, 8);
    work.poolError = pool.Error();
    if (pool && pool.Value()) {
        (void)work.pool->Deallocate(pool.Value());
    }
    auto budget = work.budget->Allocate(8, 8);
    work.budgetError = budget.Error();
    if (budget && budget.Value()) {
        (void)work.budget->Deallocate(budget.Value());
    }
}

struct BackendFixture {
    SystemAllocator backing;
    uint32 retains = 0;
    uint32 releases = 0;
    uint32 lastTag = 0;
    bool failAllocate = false;
    bool failRelease = false;
    ExternalAllocator* reenter = nullptr;
    ErrorCode nested = ErrorCode::None;
};

struct ConcurrentBackend {
    SystemAllocator backing;
    platform::Mutex gate;
    platform::ManualResetEvent bothInside;
    uint32 entrants = 0;
};

Result<void*> ConcurrentBackendAllocate(void* raw, usize bytes, usize alignment,
                                        uint32 tag) noexcept {
    auto& backend = *static_cast<ConcurrentBackend*>(raw);
    {
        platform::LockGuard lock(backend.gate);
        ++backend.entrants;
        if (backend.entrants == 2) {
            backend.bothInside.Signal();
        }
    }

    // 두 호출이 실제로 동시에 공급자에 진입했을 때만 성공한다.
    if (backend.bothInside.Wait(5000) != platform::WaitResult::Signaled) {
        return Result<void*>::Failure(ErrorCode::DependencyFailed);
    }
    return backend.backing.Allocate(bytes, alignment, tag);
}

Result<void> ConcurrentBackendDeallocate(void* raw, void* address) noexcept {
    return static_cast<ConcurrentBackend*>(raw)->backing.Deallocate(address);
}

struct ExternalWorker {
    ExternalAllocator* allocator;
    ErrorCode error = ErrorCode::None;
};

struct BlockingReleaseBackend {
    SystemAllocator backing;
    platform::ManualResetEvent entered;
    platform::ManualResetEvent proceed;
};

Result<void*> BlockingReleaseAllocate(void* raw, usize bytes, usize alignment,
                                      uint32 tag) noexcept {
    return static_cast<BlockingReleaseBackend*>(raw)->backing.Allocate(bytes, alignment, tag);
}

Result<void> BlockingReleaseDeallocate(void* raw, void* address) noexcept {
    auto& backend = *static_cast<BlockingReleaseBackend*>(raw);
    backend.entered.Signal();
    if (backend.proceed.Wait(5000) != platform::WaitResult::Signaled) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }
    return backend.backing.Deallocate(address);
}

struct ReleaseWorker {
    ExternalAllocator* allocator;
    void* address;
    ErrorCode error = ErrorCode::None;
};

void ReleaseExternalOnWorker(void* raw) noexcept {
    auto& worker = *static_cast<ReleaseWorker*>(raw);
    worker.error = worker.allocator->Deallocate(worker.address).Error();
}

void UseExternalOnWorker(void* raw) noexcept {
    auto& worker = *static_cast<ExternalWorker*>(raw);
    auto block = worker.allocator->Allocate(32, 16);
    if (!block) {
        worker.error = block.Error();
        return;
    }
    worker.error = worker.allocator->Deallocate(block.Value()).Error();
}

void RetainBackend(void* raw) noexcept {
    ++static_cast<BackendFixture*>(raw)->retains;
}

void ReleaseBackend(void* raw) noexcept {
    ++static_cast<BackendFixture*>(raw)->releases;
}

Result<void*> BackendAllocate(void* raw, usize bytes, usize alignment, uint32 tag) noexcept {
    auto& fixture = *static_cast<BackendFixture*>(raw);
    fixture.lastTag = tag;
    if (fixture.reenter) {
        ExternalAllocator* current = fixture.reenter;
        fixture.reenter = nullptr;
        auto nested = current->Allocate(1, 1);
        fixture.nested = nested.Error();
        if (nested && nested.Value()) {
            (void)current->Deallocate(nested.Value());
        }
        fixture.reenter = current;
    }
    if (fixture.failAllocate) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }
    return fixture.backing.Allocate(bytes, alignment, tag);
}

Result<void> BackendDeallocate(void* raw, void* address) noexcept {
    auto& fixture = *static_cast<BackendFixture*>(raw);
    if (fixture.failRelease) {
        fixture.failRelease = false;
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }
    return fixture.backing.Deallocate(address);
}

} // namespace

TEST(CoreBudgetFailedReleasePreservesOwnershipAndBytes) {
    SystemAllocator system;
    FailOneRelease backing(system);
    {
        BudgetAllocator budget(backing, 100);
        auto block = budget.Allocate(31, 16);
        REQUIRE(block);
        auto* bytes = static_cast<uint8*>(block.Value());
        bytes[0] = 42;
        backing.failAddress = bytes;
        backing.failOnce = true;
        CHECK(budget.Deallocate(bytes).Error() == ErrorCode::DependencyFailed);
        CHECK(bytes[0] == 42);
        CHECK(budget.Stats().liveAllocations == 1);
        CHECK(budget.Stats().liveBytes == 31);
        CHECK(budget.SetLimit(30).Error() == ErrorCode::CapacityExceeded);
        REQUIRE(budget.Deallocate(bytes));
        CHECK(budget.Stats().liveAllocations == 0);
    }
    CHECK(system.Stats().liveAllocations == 0);
    CHECK(system.Stats().liveBytes == 0);
}

TEST(CoreAllocatorCompositionRespectsBackingThread) {
    SystemAllocator system;
    {
        ArenaAllocator arena(system, 512);
        PoolAllocator pool(arena, 32, 16, 1);
        BudgetAllocator budget(arena, 512);
        auto owned = pool.Allocate(8, 8);
        REQUIRE(owned);

        // Pool의 캐시가 비어 있으면 다른 스레드의 새 chunk 확보는 상위 Arena가 거부한다.
        ForeignPoolWork work{&pool, &budget};
        platform::Thread worker;
        if (worker.Start(UseThreadBoundBacking, &work) != platform::ThreadStatus::Success ||
            worker.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
            worker.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("Allocator composition worker did not finish");
        }
        CHECK(work.poolError == ErrorCode::WrongThread);
        CHECK(work.budgetError == ErrorCode::WrongThread);
        REQUIRE(pool.Deallocate(owned.Value()));
        REQUIRE(pool.Trim());
        REQUIRE(arena.Trim());
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(CoreExternalAllocatorRequiresAnActualBackend) {
    ExternalAllocatorBackend missing;
    ExternalAllocator allocator(Move(missing));
    CHECK(!allocator.IsAvailable());
    CHECK(allocator.Allocate(0, 8).Value() == nullptr);
    CHECK(allocator.Allocate(0, 3).Error() == ErrorCode::InvalidArgument);
    CHECK(allocator.Allocate(8, 8).Error() == ErrorCode::Unsupported);
    CHECK(allocator.Deallocate(reinterpret_cast<void*>(static_cast<uptr>(1))).Error() ==
          ErrorCode::Unsupported);
    REQUIRE(allocator.Deallocate(nullptr));
}

TEST(CoreExternalAllocatorRetainsModuleAndValidatesOwnership) {
    BackendFixture fixture;
    {
        auto module = ModuleLease::Acquire(&fixture, RetainBackend, ReleaseBackend);
        REQUIRE(module);
        ExternalAllocatorBackend backend;
        backend.context = &fixture;
        backend.allocate = BackendAllocate;
        backend.deallocate = BackendDeallocate;
        backend.module = Move(module.Value());
        ExternalAllocator allocator(Move(backend));
        CHECK(allocator.IsAvailable());
        CHECK(fixture.retains == 1);
        CHECK(fixture.releases == 0);

        fixture.failAllocate = true;
        CHECK(allocator.Allocate(16, 16).Error() == ErrorCode::OutOfMemory);
        CHECK(allocator.Stats().liveAllocations == 0);
        fixture.failAllocate = false;
        fixture.reenter = &allocator;
        auto block = allocator.Allocate(32, 64, 37);
        REQUIRE(block);
        CHECK(fixture.nested == ErrorCode::Busy);
        CHECK(fixture.lastTag == 37);
        CHECK(reinterpret_cast<uptr>(block.Value()) % 64 == 0);
        auto* bytes = static_cast<uint8*>(block.Value());
        bytes[0] = 73;
        CHECK(allocator.Stats().liveBytes == 32);
        fixture.failRelease = true;
        CHECK(allocator.Deallocate(bytes).Error() == ErrorCode::DependencyFailed);
        CHECK(bytes[0] == 73);
        CHECK(allocator.Stats().liveBytes == 32);
        int32 foreign = 0;
        CHECK(allocator.Deallocate(&foreign).Error() == ErrorCode::NotFound);
        REQUIRE(allocator.Deallocate(bytes));
        CHECK(allocator.Deallocate(bytes).Error() == ErrorCode::NotFound);
        CHECK(allocator.Stats().liveAllocations == 0);
        fixture.reenter = nullptr;
    }
    CHECK(fixture.releases == 1);
    CHECK(fixture.backing.Stats().liveAllocations == 0);
}

TEST(CoreExternalAllocatorCallsBackendInParallel) {
    ConcurrentBackend fixture;
    {
        ExternalAllocatorBackend backend;
        backend.context = &fixture;
        backend.allocate = ConcurrentBackendAllocate;
        backend.deallocate = ConcurrentBackendDeallocate;
        ExternalAllocator allocator(Move(backend));
        ExternalWorker work[2] = {{&allocator}, {&allocator}};
        platform::Thread threads[2];
        for (usize index = 0; index < 2; ++index) {
            if (threads[index].Start(UseExternalOnWorker, &work[index]) !=
                platform::ThreadStatus::Success) {
                platform::Fatal("External allocator worker failed to start");
            }
        }
        for (auto& thread : threads) {
            if (thread.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
                thread.Join() != platform::ThreadStatus::Success) {
                platform::Fatal("External allocator worker did not finish");
            }
        }
        CHECK(work[0].error == ErrorCode::None);
        CHECK(work[1].error == ErrorCode::None);
        CHECK(fixture.entrants == 2);
        CHECK(allocator.Stats().liveAllocations == 0);
    }
    CHECK(fixture.backing.Stats().liveAllocations == 0);
}

TEST(CoreExternalAllocatorRecordFailureDoesNotInvokeBackend) {
    BackendFixture fixture;
    SystemAllocator recordBacking;
    test::NFailAllocator records(recordBacking);
    {
        ExternalAllocatorBackend backend;
        backend.context = &fixture;
        backend.allocate = BackendAllocate;
        backend.deallocate = BackendDeallocate;
        ExternalAllocator allocator(Move(backend), &records);
        records.FailOnAttempt(1);
        CHECK(allocator.Allocate(16, 8, 95).Error() == ErrorCode::OutOfMemory);
        CHECK(fixture.lastTag == 0);
        CHECK(fixture.backing.Stats().liveAllocations == 0);
        CHECK(recordBacking.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().liveAllocations == 0);
        records.DisableFailure();
        auto block = allocator.Allocate(16, 8, 95);
        REQUIRE(block);
        CHECK(fixture.lastTag == 95);
        REQUIRE(allocator.Deallocate(block.Value()));
    }
    CHECK(recordBacking.Stats().liveAllocations == 0);
    CHECK(fixture.backing.Stats().liveAllocations == 0);
}

TEST(CoreExternalAllocatorRejectsConcurrentDoubleRelease) {
    BlockingReleaseBackend fixture;
    {
        ExternalAllocatorBackend backend;
        backend.context = &fixture;
        backend.allocate = BlockingReleaseAllocate;
        backend.deallocate = BlockingReleaseDeallocate;
        ExternalAllocator allocator(Move(backend));
        auto block = allocator.Allocate(32, 16);
        REQUIRE(block);
        ReleaseWorker work{&allocator, block.Value()};
        platform::Thread worker;
        if (worker.Start(ReleaseExternalOnWorker, &work) != platform::ThreadStatus::Success ||
            fixture.entered.Wait(10000) != platform::WaitResult::Signaled) {
            platform::Fatal("External release worker did not start");
        }
        CHECK(allocator.Deallocate(block.Value()).Error() == ErrorCode::Busy);
        fixture.proceed.Signal();
        if (worker.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
            worker.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("External release worker did not finish");
        }
        CHECK(work.error == ErrorCode::None);
        CHECK(allocator.Deallocate(block.Value()).Error() == ErrorCode::NotFound);
        CHECK(allocator.Stats().liveAllocations == 0);
    }
    CHECK(fixture.backing.Stats().liveAllocations == 0);
}

namespace {

// 공급자의 잘못된 주소를 역참조하지 않고 진단하는 자식 프로세스 전용 fixture다.
struct BrokenBackend {
    alignas(16) unsigned char storage[128]{};
    uint32 calls = 0;
    bool overflow = false;
};

Result<void*> BrokenBackendAllocate(void* raw, usize, usize, uint32) noexcept {
    auto& backend = *static_cast<BrokenBackend*>(raw);
    if (backend.overflow) {
        return Result<void*>::Success(reinterpret_cast<void*>(MaxSize - 7));
    }
    return Result<void*>::Success(backend.storage + (backend.calls++ == 0 ? 0 : 16));
}

Result<void> BrokenBackendDeallocate(void*, void*) noexcept {
    return Result<void>::Success();
}

class BrokenMetadataRelease final : public Allocator {
public:
    explicit BrokenMetadataRelease(Allocator& backing) noexcept : backing_(backing) {
    }
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        return backing_.Allocate(bytes, alignment, tag);
    }
    Result<void> Deallocate(void*) noexcept override {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

private:
    Allocator& backing_;
};

} // namespace

namespace rocks::test {

int RunExternalAllocatorNegativeControl(uint32 operation) noexcept {
    PrepareFatalTestProcess();
    if (operation == 0 || operation == 1) {
        BrokenBackend fixture;
        fixture.overflow = operation == 1;
        ExternalAllocatorBackend backend;
        backend.context = &fixture;
        backend.allocate = BrokenBackendAllocate;
        backend.deallocate = BrokenBackendDeallocate;
        ExternalAllocator allocator(Move(backend));
        if (!allocator.Allocate(32, 1)) {
            return 0;
        }
        if (operation == 0) {
            (void)allocator.Allocate(32, 1);
        }
        return 0;
    }

    BackendFixture fixture;
    SystemAllocator metadataBacking;
    BrokenMetadataRelease metadata(metadataBacking);
    ExternalAllocatorBackend backend;
    backend.context = &fixture;
    backend.allocate = BackendAllocate;
    backend.deallocate = BackendDeallocate;
    if (operation == 2) {
        ExternalAllocator allocator(Move(backend), &metadata);
        auto block = allocator.Allocate(16, 8);
        if (!block) {
            return 0;
        }
        (void)allocator.Deallocate(block.Value());
        return 0;
    }
    ExternalAllocator allocator(Move(backend));
    (void)allocator.Allocate(16, 8);
    return 0;
}

} // namespace rocks::test
