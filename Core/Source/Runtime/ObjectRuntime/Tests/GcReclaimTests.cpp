#include <TestSupport/Test.h>

#include <Core/Memory/SystemAllocator.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Platform/Clock.h>

namespace gc_reclaim_tests_file {

// 묶음 회수, 회수를 미루는 정지형 수집, 예산 회수의 시험이다(GC 설계 17.1·17.2절 —
// Docs/Design/GarbageCollectionScale.ko.md).
namespace {

using namespace rocks;

uint32 reclaimDestructions = 0;
// 참이면 소멸자가 시계가 앞으로 갈 때까지 기다린다(시간 한도를 결정적으로 넘기는 시험용 — 잠이나
// 경쟁 없이 시계 값이 바뀌는 것만 본다).
bool reclaimAdvanceClock = false;

// 소멸자에서 다른 객체의 backend 보유를 놓는 노드다(묶음 사이에 지연 슬롯이 풀리는지 본다).
struct ReclaimNode final : Object {
    BackendLease held;

    ~ReclaimNode() noexcept {
        ++reclaimDestructions;
        held.Reset();
        if (reclaimAdvanceClock) {
            const uint64 entered = platform::MonotonicNanoseconds();
            while (platform::MonotonicNanoseconds() <= entered + 1) {
            }
        }
    }

    void Trace(TraceVisitor&) const noexcept {
    }
};

constexpr Guid ReclaimNodeType{0x5245434c41494dULL, 1};

bool RegisterReclaimTypes(TypeRegistry& types) noexcept {
    return types.Register(TypeSchema{ReclaimNodeType, 1, {}, {}}, MakeRuntimeTypeOps<ReclaimNode>())
        .IsOk();
}

// 루트 없는 쓰레기 count개를 만든다(만들자마자 루트를 놓는다).
bool MakeGarbage(ObjectRegistry& objects, uint32 count) noexcept {
    for (uint32 index = 0; index < count; ++index) {
        auto created = objects.NewObject<ReclaimNode>(objects.OwnerAccess(), ReclaimNodeType);
        if (!created) {
            return false;
        }
    }

    return true;
}

} // namespace

// Deferred 정지형 수집은 분류만 하고 회수하지 않는다. 예산 회수는 객체 수 한도를 묶음(64개)
// 안에서도 정확히 지킨다(70 = 64 + 6). 남은 것이 없으면 0이다.
TEST(ObjectGcReclaim_DeferredCollectDrainsWithinBudget) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterReclaimTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x5243, 1});
        const auto access = objects.OwnerAccess();
        constexpr uint32 Garbage = 200;
        REQUIRE(MakeGarbage(objects, Garbage));
        reclaimDestructions = 0;
        auto collected = objects.Collect(access, nullptr, GcReclaim::Deferred);
        REQUIRE(collected);
        CHECK(collected.Value().retired == Garbage);
        CHECK(collected.Value().reclaimed == 0);
        CHECK(reclaimDestructions == 0);
        CHECK(objects.Stats().retiredObjects == Garbage);
        const GcBudget budget{70, 0};
        auto first = objects.DrainRetired(access, budget);
        REQUIRE(first);
        CHECK(first.Value() == 70);
        CHECK(reclaimDestructions == 70);
        auto second = objects.DrainRetired(access, budget);
        auto third = objects.DrainRetired(access, budget);
        auto fourth = objects.DrainRetired(access, budget);
        REQUIRE(second && third && fourth);
        CHECK(second.Value() == 70);
        CHECK(third.Value() == 60);
        CHECK(fourth.Value() == 0);
        CHECK(reclaimDestructions == Garbage);
        CHECK(objects.Stats().retiredObjects == 0);
        CHECK(objects.DrainRetired(access, GcBudget{0, 0}).Error() == ErrorCode::InvalidArgument);
        REQUIRE(objects.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 시간 한도는 소멸자 사이마다 본다: 소멸자 하나가 한도(1ns)를 넘기면 그 호출은 하나만 회수하고,
// 묶음 안에서 소멸하지 않은 객체는 회수 대상(Retired)으로 되돌아간다. 회수할 것이 있으면 한 호출이
// 적어도 하나는 회수한다(최소 진척). 질의(HasPendingReclaim)가 회수 대기를 보여 준다.
TEST(ObjectGcReclaim_TimeBudgetStopsBetweenDestructorsWithProgress) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterReclaimTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x5243, 4});
        const auto access = objects.OwnerAccess();
        constexpr uint32 Garbage = 10;
        REQUIRE(MakeGarbage(objects, Garbage));
        reclaimDestructions = 0;
        REQUIRE(objects.Collect(access, nullptr, GcReclaim::Deferred));
        CHECK(objects.HasPendingReclaim());
        reclaimAdvanceClock = true;
        const GcBudget tight{GcBudget::DefaultMaxObjects, 1};
        for (uint32 call = 0; call < Garbage; ++call) {
            auto drained = objects.DrainRetired(access, tight);
            REQUIRE(drained);
            CHECK(drained.Value() == 1);
            CHECK(objects.Stats().retiredObjects == Garbage - call - 1);
        }

        reclaimAdvanceClock = false;
        CHECK(reclaimDestructions == Garbage);
        CHECK(!objects.HasPendingReclaim());
        // 회수를 미룬 수집의 마지막 보고서가 나눠 한 회수까지 보여 준다.
        CHECK(objects.LastGcReport().reclaimed == Garbage);
        REQUIRE(objects.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 회수를 미루고 cycle을 끝낸다: Reclaiming 단계의 증분 cycle을 FinishCollectionDeferringReclaim이
// 회수 없이 Idle로 끝내고 남은 것은 회수 대상으로 남는다. Marking이면 InvalidArgument다.
TEST(ObjectGcReclaim_FinishCollectionDeferringReclaimLeavesRetired) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterReclaimTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x5243, 5});
        const auto access = objects.OwnerAccess();
        constexpr uint32 Garbage = 20;
        REQUIRE(MakeGarbage(objects, Garbage));
        REQUIRE(objects.BeginCollection(access));
        CHECK(objects.FinishCollectionDeferringReclaim(access).Error() ==
              ErrorCode::InvalidArgument);
        // 회수 단계에 들어가 하나만 회수할 때까지 한 단위씩 진행한다.
        for (uint32 guard = 0; guard < 256 && objects.CollectionPhase() != GcPhase::Reclaiming;
             ++guard) {
            REQUIRE(objects.StepCollection(access, GcBudget{1, 0}));
        }

        REQUIRE(objects.CollectionPhase() == GcPhase::Reclaiming);
        const uint32 left = objects.Stats().retiredObjects;
        CHECK(left != 0);
        reclaimDestructions = 0;
        REQUIRE(objects.FinishCollectionDeferringReclaim(access));
        CHECK(objects.CollectionPhase() == GcPhase::Idle);
        CHECK(reclaimDestructions == 0);
        CHECK(objects.Stats().retiredObjects == left);
        CHECK(objects.HasPendingReclaim());
        auto drained = objects.DrainRetired(access);
        REQUIRE(drained);
        CHECK(drained.Value() == left);
        CHECK(objects.Stats().liveObjects == 0);
        CHECK(objects.FinishCollectionDeferringReclaim(access));
        REQUIRE(objects.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// FinishCollectionDeferringReclaim의 safepoint는 StepCollection과 같다: Sweeping 단계에서 Borrow가
// 있으면 Busy이고 cycle은 그대로다. Borrow를 놓으면 남은 분류를 하고 Idle로 끝낸다.
TEST(ObjectGcReclaim_FinishCollectionDeferringReclaimNeedsSafepoint) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterReclaimTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x5243, 6});
        const auto access = objects.OwnerAccess();
        auto kept = objects.NewObject<ReclaimNode>(access, ReclaimNodeType);
        REQUIRE(kept);
        constexpr uint32 Garbage = 4;
        REQUIRE(MakeGarbage(objects, Garbage));
        REQUIRE(objects.BeginCollection(access));
        for (uint32 guard = 0; guard < 256 && objects.CollectionPhase() == GcPhase::Marking;
             ++guard) {
            REQUIRE(objects.StepCollection(access, GcBudget{1, 0}));
        }

        REQUIRE(objects.CollectionPhase() == GcPhase::Sweeping);
        {
            auto borrowed = objects.Resolve<ReclaimNode>(access, kept.Value().Handle());
            REQUIRE(borrowed);
            CHECK(objects.FinishCollectionDeferringReclaim(access).Error() == ErrorCode::Busy);
            CHECK(objects.CollectionPhase() == GcPhase::Sweeping);
        }

        reclaimDestructions = 0;
        REQUIRE(objects.FinishCollectionDeferringReclaim(access));
        CHECK(objects.CollectionPhase() == GcPhase::Idle);
        CHECK(objects.Stats().retiredObjects == Garbage);
        CHECK(reclaimDestructions == 0);
        auto drained = objects.DrainRetired(access);
        REQUIRE(drained);
        CHECK(drained.Value() == Garbage);
        kept.Value().Reset();
        REQUIRE(objects.Collect(access));
        REQUIRE(objects.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 남은 회수 대상이 있어도 다음 수집을 할 수 있고, 즉시 회수 수집이 남은 것까지 회수한다.
TEST(ObjectGcReclaim_CollectAfterDeferredReclaimsTheRest) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterReclaimTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x5243, 2});
        const auto access = objects.OwnerAccess();
        REQUIRE(MakeGarbage(objects, 100));
        reclaimDestructions = 0;
        REQUIRE(objects.Collect(access, nullptr, GcReclaim::Deferred));
        auto partial = objects.DrainRetired(access, GcBudget{10, 0});
        REQUIRE(partial);
        CHECK(partial.Value() == 10);
        REQUIRE(MakeGarbage(objects, 5));
        auto collected = objects.Collect(access);
        REQUIRE(collected);
        CHECK(collected.Value().retired == 5);
        CHECK(collected.Value().reclaimed == 95);
        CHECK(reclaimDestructions == 105);
        CHECK(objects.Stats().retiredObjects == 0);
        REQUIRE(objects.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 묶음 사이에 잠금을 푼다: 쓰레기 A의 소멸자가 쓰레기 B의 backend 보유를 놓으면 B(보유 때문에 지연
// 목록)는 같은 회수 호출의 다음 묶음에서 회수된다.
TEST(ObjectGcReclaim_DestructorReleasingHoldIsReclaimedInTheSameDrain) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterReclaimTypes(types));
        ObjectRegistry objects(allocator, types, Guid{0x5243, 3});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<ReclaimNode>(access, ReclaimNodeType);
        auto held = objects.NewObject<ReclaimNode>(access, ReclaimNodeType);
        REQUIRE(holder && held);
        auto lease = objects.AcquireBackendLease(access, held.Value().Handle());
        REQUIRE(lease);
        {
            auto borrowed = objects.Resolve<ReclaimNode>(access, holder.Value().Handle());
            REQUIRE(borrowed);
            ReclaimNode* node = borrowed.Value().Get().Value();
            node->held = Move(lease.Value());
        }

        holder.Value().Reset();
        held.Value().Reset();
        reclaimDestructions = 0;
        auto collected = objects.Collect(access, nullptr, GcReclaim::Deferred);
        REQUIRE(collected);
        CHECK(collected.Value().retired == 2);
        auto drained = objects.DrainRetired(access);
        REQUIRE(drained);
        CHECK(drained.Value() == 2);
        CHECK(reclaimDestructions == 2);
        CHECK(objects.Stats().liveObjects == 0);
        REQUIRE(objects.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
} // namespace gc_reclaim_tests_file
