#include <Tests/Support/Test.h>
#include <Tests/Support/NFailAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Task/WorkerScheduler.h>
#include <Platform/Clock.h>

using namespace rocks;

namespace {

constexpr Guid IncrementalType{0x494e43524743ULL, 1};
uint32 destroyed = 0;
platform::Mutex traceGate;
uint64 expectedOwner = 0;
uint32 workerTraces = 0;
bool observeWorkers = false;

struct IncrementalNode final : Object {
    ObjectRef<IncrementalNode> first;
    ObjectRef<IncrementalNode> second;
    ObjectHandle poison;

    ~IncrementalNode() noexcept {
        ++destroyed;
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        {
            platform::LockGuard lock(traceGate);
            if (observeWorkers && platform::CurrentThreadId() != expectedOwner) {
                ++workerTraces;
            }
        }
        visitor.Visit(first);
        visitor.Visit(second);
        visitor.Visit(poison);
    }
};

bool Finish(ObjectRegistry& registry, uint32 budget = 3,
            WorkerScheduler* workers = nullptr) noexcept {
    for (uint32 iteration = 0; iteration < 100000; ++iteration) {
        if (registry.CollectionPhase() == GcPhase::Idle) {
            return true;
        }
        auto step = registry.StepCollection(registry.OwnerAccess(), {budget, 0}, workers);
        if (!step || step.Value().processed > budget) {
            return false;
        }
    }
    return false;
}

Result<TypeRegistrationToken> Register(TypeRegistry& types, bool parallel = false) noexcept {
    auto ops = MakeRuntimeTypeOps<IncrementalNode>();
    ops.parallelTraceSafe = parallel;
    return types.Register({IncrementalType, 1, {}, {}}, Move(ops));
}

} // namespace

TEST(ObjectIncrementalGcInsertionRootAndNewbornBarriersPreserveReachability) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(Register(types));
        ObjectRegistry registry(allocator, types, {81, 1});
        const auto access = registry.OwnerAccess();
        auto root = registry.NewObject<IncrementalNode>(access, IncrementalType);
        auto old = registry.NewObject<IncrementalNode>(access, IncrementalType);
        auto promoted = registry.NewObject<IncrementalNode>(access, IncrementalType);
        REQUIRE(root && old && promoted);
        const auto oldHandle = old.Value().Handle();
        const auto promotedHandle = promoted.Value().Handle();
        old.Value().Reset();
        promoted.Value().Reset();
        REQUIRE(registry.BeginCollection(access));
        auto firstStep = registry.StepCollection(access, {1, 0});
        REQUIRE(firstStep);
        CHECK(firstStep.Value().phase == GcPhase::Marking);
        CHECK(firstStep.Value().processed == 1);

        // root는 이미 black이다. 이후 white 대상 연결을 쓰기 장벽이 반드시 shade해야 한다.
        {
            auto borrow = registry.Resolve<IncrementalNode>(access, root.Value().Handle());
            REQUIRE(borrow);
            auto pointer = borrow.Value().Get();
            REQUIRE(pointer);
            REQUIRE(registry.SetReference(access, root.Value().Handle(), pointer.Value()->first,
                                          oldHandle));
        }
        auto pinned = registry.TryPin<IncrementalNode>(access, promotedHandle);
        REQUIRE(pinned);
        pinned.Value().Reset();
        auto newborn = registry.NewObject<IncrementalNode>(access, IncrementalType);
        REQUIRE(newborn);
        newborn.Value().Reset();
        REQUIRE(Finish(registry, 1));
        CHECK(registry.Stats().liveObjects == 4);
        CHECK(registry.LastGcReport().marked == 4);

        // 이번 cycle의 부유 쓰레기는 다음 독립 full collector에서 회수한다.
        REQUIRE(registry.Collect(access));
        CHECK(registry.Stats().liveObjects == 2);
        root.Value().Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectIncrementalGcCutoffBlocksUnclassifiedWhiteAndProtectsLaterBirths) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(Register(types));
        ObjectRegistry registry(allocator, types, {81, 2});
        const auto access = registry.OwnerAccess();
        auto first = registry.NewObject<IncrementalNode>(access, IncrementalType);
        auto second = registry.NewObject<IncrementalNode>(access, IncrementalType);
        auto root = registry.NewObject<IncrementalNode>(access, IncrementalType);
        REQUIRE(first && second && root);
        const auto white = second.Value().Handle();
        first.Value().Reset();
        second.Value().Reset();
        REQUIRE(registry.BeginCollection(access));
        REQUIRE(registry.StepCollection(access, {1, 0}));
        auto cutoff = registry.StepCollection(access, {1, 0});
        REQUIRE(cutoff);
        CHECK(cutoff.Value().phase == GcPhase::Sweeping);
        CHECK(registry.Stats().retiredObjects == 1);
        CHECK(!registry.Resolve<IncrementalNode>(access, white));
        CHECK(!registry.TryPin<IncrementalNode>(access, white));
        CHECK(!registry.AcquireBackendLease(access, white));
        CHECK(registry.CancelCollection(access).Error() == ErrorCode::Busy);

        auto newborn = registry.NewObject<IncrementalNode>(access, IncrementalType);
        REQUIRE(newborn);
        {
            auto borrow = registry.Resolve<IncrementalNode>(access, root.Value().Handle());
            REQUIRE(borrow);
            auto pointer = borrow.Value().Get();
            REQUIRE(pointer);
            CHECK(!registry.SetReference(access, root.Value().Handle(), pointer.Value()->first,
                                         white));
            CHECK(pointer.Value()->first.Handle().IsNull());
            REQUIRE(registry.SetReference(access, root.Value().Handle(), pointer.Value()->first,
                                          newborn.Value().Handle()));
        }
        newborn.Value().Reset();
        REQUIRE(Finish(registry, 1));
        CHECK(registry.Stats().liveObjects == 2);
        root.Value().Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectIncrementalGcSafepointsAndCancelPreserveLastSuccessfulReport) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(Register(types));
        ObjectRegistry registry(allocator, types, {81, 3});
        const auto access = registry.OwnerAccess();
        auto root = registry.NewObject<IncrementalNode>(access, IncrementalType);
        REQUIRE(root);
        REQUIRE(registry.Collect(access));
        const auto previous = registry.LastGcReport();
        {
            auto borrow = registry.Resolve<IncrementalNode>(access, root.Value().Handle());
            REQUIRE(borrow);
            CHECK(registry.BeginCollection(access).Error() == ErrorCode::Busy);
        }
        {
            ConstructionContext construction(registry, access);
            CHECK(registry.BeginCollection(access).Error() == ErrorCode::Busy);
        }
        REQUIRE(registry.BeginCollection(access));
        CHECK(registry.BeginCollection(access).Error() == ErrorCode::Busy);
        CHECK(registry.Collect(access).Error() == ErrorCode::Busy);
        CHECK(registry.StepCollection(access, {0, 0}).Error() == ErrorCode::InvalidArgument);
        {
            auto borrow = registry.Resolve<IncrementalNode>(access, root.Value().Handle());
            REQUIRE(borrow);
            CHECK(registry.StepCollection(access, {1, 0}).Error() == ErrorCode::Busy);
        }
        REQUIRE(registry.StepCollection(access, {1, 0}));
        REQUIRE(registry.CancelCollection(access));
        CHECK(registry.CollectionPhase() == GcPhase::Idle);
        CHECK(registry.LastGcReport().durationNanoseconds == previous.durationNanoseconds);
        CHECK(registry.LastGcReport().marked == previous.marked);
        CHECK(registry.StepCollection(access).Error() == ErrorCode::InvalidArgument);
        root.Value().Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectIncrementalGcFailedSnapshotAndBirthKeepGraphAndCycleUsable) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(Register(types));
        rocks::test::NFailAllocator fault(allocator);
        ObjectRegistry registry(fault, types, {81, 4});
        const auto access = registry.OwnerAccess();
        auto root = registry.NewObject<IncrementalNode>(access, IncrementalType);
        REQUIRE(root);
        fault.FailOnAttempt(1);
        CHECK(registry.BeginCollection(access).Error() == ErrorCode::OutOfMemory);
        CHECK(registry.CollectionPhase() == GcPhase::Idle);
        CHECK(registry.Stats().liveObjects == 1);
        fault.DisableFailure();
        REQUIRE(registry.BeginCollection(access));
        fault.FailOnAttempt(1);
        CHECK(!registry.NewObject<IncrementalNode>(access, IncrementalType));
        CHECK(registry.Stats().liveObjects == 1);
        CHECK(registry.Stats().reservedObjects == 0);
        CHECK(registry.CollectionPhase() == GcPhase::Marking);
        fault.DisableFailure();
        REQUIRE(Finish(registry));
        root.Value().Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectIncrementalGcTraceFailureCannotPublishCutoffOrReclaimGarbage) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(Register(types));
        ObjectRegistry registry(allocator, types, {81, 5});
        const auto access = registry.OwnerAccess();
        auto root = registry.NewObject<IncrementalNode>(access, IncrementalType);
        auto dead = registry.NewObject<IncrementalNode>(access, IncrementalType);
        REQUIRE(root && dead);
        dead.Value().Reset();
        {
            auto borrow = registry.Resolve<IncrementalNode>(access, root.Value().Handle());
            REQUIRE(borrow);
            auto pointer = borrow.Value().Get();
            REQUIRE(pointer);
            pointer.Value()->poison = {0xffffu, 17, 1};
        }
        REQUIRE(registry.BeginCollection(access));
        CHECK(registry.StepCollection(access, {10, 0}).Error() == ErrorCode::StaleHandle);
        CHECK(registry.CollectionPhase() == GcPhase::Idle);
        CHECK(registry.Stats().liveObjects == 2);
        CHECK(registry.Stats().retiredObjects == 0);
        {
            auto borrow = registry.Resolve<IncrementalNode>(access, root.Value().Handle());
            REQUIRE(borrow);
            auto pointer = borrow.Value().Get();
            REQUIRE(pointer);
            pointer.Value()->poison = {};
        }
        REQUIRE(registry.BeginCollection(access));
        REQUIRE(Finish(registry));
        CHECK(registry.Stats().liveObjects == 1);
        root.Value().Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectIncrementalGcMatchesIndependentGraphOracleWithParallelMarking) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(Register(types, true));
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({4, 256, 64}));
        ObjectRegistry registry(allocator, types, {81, 6});
        const auto access = registry.OwnerAccess();
        constexpr uint32 count = 160;
        ObjectHandle handles[count];
        uint32 graph[count][2];
        ConstructionContext construction(registry, access);
        for (uint32 index = 0; index < count; ++index) {
            auto made = construction.Create<IncrementalNode>(IncrementalType);
            REQUIRE(made);
            handles[index] = made.Value();
        }
        for (uint32 index = 0; index < count; ++index) {
            const uint32 group = (index / 40) * 40;
            graph[index][0] = group + (index * 17 + 3) % 40;
            graph[index][1] = group + (index + 1) % 40;
            auto object = construction.Get<IncrementalNode>(handles[index]);
            REQUIRE(object);
            REQUIRE(construction.SetReference(handles[index], object.Value()->first,
                                              handles[graph[index][0]]));
            REQUIRE(construction.SetReference(handles[index], object.Value()->second,
                                              handles[graph[index][1]]));
        }
        auto root = construction.Commit();
        REQUIRE(root);
        auto secondRoot = registry.TryPin<IncrementalNode>(access, handles[80]);
        REQUIRE(secondRoot);
        bool reachable[count]{};
        uint32 pending[count];
        uint32 pendingCount = 2;
        pending[0] = 0;
        pending[1] = 80;
        reachable[0] = reachable[80] = true;
        while (pendingCount != 0) {
            const uint32 current = pending[--pendingCount];
            for (uint32 target : graph[current]) {
                if (!reachable[target]) {
                    reachable[target] = true;
                    pending[pendingCount++] = target;
                }
            }
        }
        expectedOwner = platform::CurrentThreadId();
        workerTraces = 0;
        observeWorkers = true;
        REQUIRE(registry.BeginCollection(access));
        REQUIRE(Finish(registry, 7, &scheduler));
        observeWorkers = false;
        CHECK(workerTraces != 0);
        uint32 expectedCount = 0;
        for (uint32 index = 0; index < count; ++index) {
            expectedCount += reachable[index] ? 1u : 0u;
            CHECK(bool(registry.Resolve<IncrementalNode>(access, handles[index])) ==
                  reachable[index]);
        }
        CHECK(registry.Stats().liveObjects == expectedCount);
        CHECK(registry.LastGcReport().marked == expectedCount);
        auto full = registry.Collect(access);
        REQUIRE(full);
        CHECK(full.Value().marked == expectedCount);
        CHECK(full.Value().retired == 0);
        secondRoot.Value().Reset();
        root.Value().Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(scheduler.Shutdown());
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectIncrementalGcRejectedMarkerSubmissionAbortsWithoutSweep) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(Register(types, true));
        WorkerScheduler stopped(allocator);
        ObjectRegistry registry(allocator, types, {81, 7});
        const auto access = registry.OwnerAccess();
        auto root = registry.NewObject<IncrementalNode>(access, IncrementalType);
        auto garbage = registry.NewObject<IncrementalNode>(access, IncrementalType);
        REQUIRE(root && garbage);
        garbage.Value().Reset();
        REQUIRE(registry.BeginCollection(access));
        CHECK(!registry.StepCollection(access, {10, 0}, &stopped));
        CHECK(registry.CollectionPhase() == GcPhase::Idle);
        CHECK(registry.Stats().liveObjects == 2);
        CHECK(registry.Stats().retiredObjects == 0);
        root.Value().Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectIncrementalGcNewbornTraceDiscoversPreexistingWhiteChild) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(Register(types));
        ObjectRegistry registry(allocator, types, {81, 8});
        const auto access = registry.OwnerAccess();
        auto child = registry.NewObject<IncrementalNode>(access, IncrementalType);
        REQUIRE(child);
        const auto childHandle = child.Value().Handle();
        child.Value().Reset();
        REQUIRE(registry.BeginCollection(access));

        ConstructionContext creation(registry, access);
        auto newborn = creation.Create<IncrementalNode>(IncrementalType);
        REQUIRE(newborn);
        auto pointer = creation.Get<IncrementalNode>(newborn.Value());
        REQUIRE(pointer);
        // 사용자 Trace가 관리하는 생성 중 값이다. 단순 black 표시만 하고 outgoing trace를
        // 생략하는 잘못된 newborn 정책이면 기존 white child가 회수되는 반례다.
        pointer.Value()->poison = childHandle;
        auto root = creation.Commit();
        REQUIRE(root);
        REQUIRE(Finish(registry, 1));
        CHECK(registry.Stats().liveObjects == 2);
        CHECK(registry.Resolve<IncrementalNode>(access, childHandle));
        root.Value().Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectIncrementalGcReclaimBudgetAndBackendHoldCompleteWithoutEarlyFree) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(Register(types));
        ObjectRegistry registry(allocator, types, {81, 9});
        const auto access = registry.OwnerAccess();
        auto held = registry.NewObject<IncrementalNode>(access, IncrementalType);
        REQUIRE(held);
        auto backend = registry.AcquireBackendLease(access, held.Value().Handle());
        REQUIRE(backend);
        held.Value().Reset();
        for (uint32 index = 0; index < 5; ++index) {
            auto transient = registry.NewObject<IncrementalNode>(access, IncrementalType);
            REQUIRE(transient);
        }
        destroyed = 0;
        REQUIRE(registry.BeginCollection(access));
        while (registry.CollectionPhase() != GcPhase::Idle) {
            const uint32 before = destroyed;
            auto progress = registry.StepCollection(access, {1, 0});
            REQUIRE(progress);
            CHECK(progress.Value().processed <= 1);
            CHECK(destroyed - before <= 1);
        }
        CHECK(destroyed == 5);
        CHECK(registry.Stats().retiredObjects == 1);
        CHECK(registry.LastGcReport().retired == 6);
        CHECK(registry.LastGcReport().reclaimed == 5);
        backend.Value().Reset();
        auto drained = registry.DrainRetired(access);
        REQUIRE(drained);
        CHECK(drained.Value() == 1);
        CHECK(destroyed == 6);
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
