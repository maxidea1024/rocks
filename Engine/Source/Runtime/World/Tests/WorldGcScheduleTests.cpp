#include <TestSupport/Test.h>

#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Object/ModuleRegistration.h>
#include <Object/TypeRegistry.h>
#include <Task/WorkerScheduler.h>
#include <World/World.h>
#include <World/World.module.generated.h>

// World GC 일정(Docs/Design/GarbageCollectionScale.ko.md 17.3절): 주기 전체 수집, 전체 수집 요청,
// 회수 예산.
namespace {

using namespace rocks;

constexpr double FrameSeconds = 1.0 / 60.0;

// World 타입이 아닌 평범한 객체다(World 파괴 없이 생기는 쓰레기).
struct PlainGarbage final : Object {
    void Trace(TraceVisitor&) const noexcept {
    }
};

constexpr Guid PlainGarbageType{0x5753434845ull, 0x100};

struct ScheduleFixture {
    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    UniquePtr<World> world;

    explicit ScheduleFixture(const WorldGcSchedule& schedule, uint64 context,
                             const GcBudget& gcBudget = {},
                             WorkerScheduler* scheduler = nullptr) noexcept
        : types(allocator), modules(types, allocator) {
        if (!RegisterWorldTypes(modules)) {
            return;
        }

        WorldDesc desc;
        desc.name = "GcSchedule";
        desc.context = {0x5753434845ull, context};
        desc.timeStep = {FrameSeconds, 0.25, 8};
        desc.gcSchedule = schedule;
        desc.gcBudget = gcBudget;
        desc.gcScheduler = scheduler;
        desc.objects.verifyClusterLedger = true;
        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created && created.Value()->Initialize() && created.Value()->BeginPlay()) {
            world = Move(created.Value());
        }
    }

    ~ScheduleFixture() {
        if (world) {
            (void)world->Shutdown();
            world.Reset();
        }

        (void)modules.Unregister();
        (void)types.Shutdown();
    }

    bool Exists(ObjectHandle handle) noexcept {
        return static_cast<bool>(world->Objects().InspectLifecycle(world->Access(), handle));
    }

    bool Tick() noexcept {
        return static_cast<bool>(world->Tick(FrameSeconds));
    }

    uint32 Retired() noexcept {
        return world->Objects().Stats().retiredObjects;
    }

    // count개의 Actor를 만들어 handles에 담는다.
    bool Spawn(ObjectHandle* handles, uint32 count) noexcept {
        for (uint32 index = 0; index < count; ++index) {
            auto spawned = world->SpawnActor<Actor>();
            if (!spawned) {
                return false;
            }

            handles[index] = spawned.Value();
        }

        return true;
    }
};

// 시험의 회수 예산은 객체 수로 둔다(시간 예산은 선점에 따라 프레임 수가 달라진다 — 결정적 진행).
WorldGcSchedule
ObjectBudgetSchedule(uint32 objectsPerFrame = GcBudget::DefaultMaxObjects) noexcept {
    WorldGcSchedule schedule;
    schedule.reclaimBudget = GcBudget{objectsPerFrame, 0};
    return schedule;
}

} // namespace

// FullCollection 모드는 파괴가 있어도 간격(World 게임 시간)이 지나기 전에는 수집하지 않는다. 간격이
// 지난 프레임에 STW 전체 수집을 하고(회수는 미룸), 다음 프레임의 예산 회수가 저장소를 반환한다.
TEST(WorldGc_FullCollectionModeWaitsForInterval) {
    WorldGcSchedule schedule = ObjectBudgetSchedule();
    schedule.mode = WorldGcMode::FullCollection;
    schedule.fullCollectionIntervalSeconds = 0.5;
    ScheduleFixture fixture(schedule, 1);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    auto actor = world.SpawnActor<Actor>();
    REQUIRE(actor);
    REQUIRE(world.DestroyActor(actor.Value()));
    // 0.5초는 30프레임이다. 그 전까지는 저장소가 남는다.
    for (uint32 frame = 0; frame < 28; ++frame) {
        REQUIRE(fixture.Tick());
    }

    CHECK(fixture.Exists(actor.Value()));
    // 간격이 지나는 프레임에 수집하고 그다음 프레임 안에 회수한다.
    for (uint32 frame = 0; frame < 4; ++frame) {
        REQUIRE(fixture.Tick());
    }

    CHECK(!fixture.Exists(actor.Value()));
    CHECK(world.Objects().Stats().retiredObjects == 0);
}

// 전체 수집 요청은 모드와 무관하게 다음 프레임 끝에 수집하고, 회수는 다음 프레임들에 예산(객체
// 2개)씩 나눈다. 수집한 프레임에는 분류만 되어 회수 대상이 남는다.
TEST(WorldGc_RequestedFullCollectionSpreadsReclaimOverFrames) {
    ScheduleFixture fixture(ObjectBudgetSchedule(2), 2);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    constexpr uint32 Actors = 5;
    ObjectHandle actors[Actors];
    for (ObjectHandle& handle : actors) {
        auto spawned = world.SpawnActor<Actor>();
        REQUIRE(spawned);
        handle = spawned.Value();
    }

    REQUIRE(fixture.Tick());
    // 증분 GC가 끼지 않게 파괴와 요청을 같은 프레임 밖에서 하고, 증분 예산 대신 요청 경로를 본다.
    for (ObjectHandle handle : actors) {
        REQUIRE(world.DestroyActor(handle));
    }

    world.RequestFullCollection();
    REQUIRE(fixture.Tick());
    const uint32 retired = world.Objects().Stats().retiredObjects;
    CHECK(retired == Actors);
    // 프레임마다 2개씩 회수한다(5 → 3 → 1 → 0).
    REQUIRE(fixture.Tick());
    CHECK(world.Objects().Stats().retiredObjects == 3);
    REQUIRE(fixture.Tick());
    CHECK(world.Objects().Stats().retiredObjects == 1);
    REQUIRE(fixture.Tick());
    CHECK(world.Objects().Stats().retiredObjects == 0);
    for (ObjectHandle handle : actors) {
        CHECK(!fixture.Exists(handle));
    }
}

// 증분 모드(기본)는 지금처럼 파괴 뒤 프레임마다 증분 slice로 회수한다(요청 없이).
TEST(WorldGc_IncrementalModeIsDefault) {
    WorldGcSchedule schedule;
    CHECK(schedule.mode == WorldGcMode::Incremental);
    ScheduleFixture fixture(schedule, 3);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    auto actor = world.SpawnActor<Actor>();
    REQUIRE(actor);
    REQUIRE(world.DestroyActor(actor.Value()));
    for (uint32 frame = 0; frame < 8 && fixture.Exists(actor.Value()); ++frame) {
        REQUIRE(fixture.Tick());
    }

    CHECK(!fixture.Exists(actor.Value()));
}

// 증분 cycle이 Marking일 때 요청하면 cycle을 취소하고 STW 전체 수집을 한다(취소 분기 — GC 설계
// 17.3절). 수집한 프레임에 분류되고 다음 프레임에 회수된다.
TEST(WorldGc_RequestDuringMarkingCancelsAndCollects) {
    ScheduleFixture fixture(ObjectBudgetSchedule(), 4, GcBudget{1, 0});
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    constexpr uint32 Actors = 8;
    ObjectHandle actors[Actors];
    REQUIRE(fixture.Spawn(actors, Actors));
    REQUIRE(fixture.Tick());
    REQUIRE(world.DestroyActor(actors[0]));
    // 파괴가 있은 프레임이 증분 cycle을 시작하고 객체 하나만 표식한다.
    REQUIRE(fixture.Tick());
    REQUIRE(world.Objects().CollectionPhase() == GcPhase::Marking);
    world.RequestFullCollection();
    REQUIRE(fixture.Tick());
    CHECK(world.Objects().CollectionPhase() == GcPhase::Idle);
    CHECK(fixture.Retired() != 0);
    REQUIRE(fixture.Tick());
    CHECK(!fixture.Exists(actors[0]));
    CHECK(fixture.Retired() == 0);
    for (uint32 index = 1; index < Actors; ++index) {
        CHECK(fixture.Exists(actors[index]));
    }
}

// 증분 cycle이 회수 중(Reclaiming)일 때 요청하면 남은 회수를 그 프레임에 하지 않고(회수 없이
// cycle을 끝낸다) 다음 프레임들에 예산(2개)씩 나눈다.
TEST(WorldGc_RequestDuringReclaimingKeepsReclaimBudgeted) {
    ScheduleFixture fixture(ObjectBudgetSchedule(2), 5, GcBudget{1, 0});
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    constexpr uint32 Actors = 12;
    ObjectHandle actors[Actors];
    REQUIRE(fixture.Spawn(actors, Actors));
    REQUIRE(fixture.Tick());
    for (ObjectHandle handle : actors) {
        REQUIRE(world.DestroyActor(handle));
    }

    for (uint32 frame = 0; frame < 4096 && world.Objects().CollectionPhase() != GcPhase::Reclaiming;
         ++frame) {
        REQUIRE(fixture.Tick());
    }

    REQUIRE(world.Objects().CollectionPhase() == GcPhase::Reclaiming);
    const uint32 left = fixture.Retired();
    REQUIRE(left > 4);
    world.RequestFullCollection();
    REQUIRE(fixture.Tick());
    CHECK(world.Objects().CollectionPhase() == GcPhase::Idle);
    CHECK(fixture.Retired() == left);
    REQUIRE(fixture.Tick());
    CHECK(fixture.Retired() == left - 2);
    for (uint32 frame = 0; frame < Actors && fixture.Retired() != 0; ++frame) {
        REQUIRE(fixture.Tick());
    }

    CHECK(fixture.Retired() == 0);
    for (ObjectHandle handle : actors) {
        CHECK(!fixture.Exists(handle));
    }
}

// 외부 Borrow가 있어 전체 수집이 Busy면 요청을 남겨 다음 GC 단계에서 다시 한다(Borrow는 Tick 전에
// 반환하는 것이 계약이지만, 어겨도 수집을 미룰 뿐 요청을 잃지 않는다).
TEST(WorldGc_BusyFullCollectionRetriesNextFrame) {
    WorldGcSchedule schedule = ObjectBudgetSchedule();
    schedule.mode = WorldGcMode::FullCollection;
    schedule.fullCollectionIntervalSeconds = 1.0e9;
    ScheduleFixture fixture(schedule, 6);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    ObjectHandle actors[2];
    REQUIRE(fixture.Spawn(actors, 2));
    REQUIRE(world.DestroyActor(actors[0]));
    world.RequestFullCollection();
    {
        auto held = world.Resolve<Actor>(actors[1]);
        REQUIRE(held);
        REQUIRE(fixture.Tick());
        REQUIRE(fixture.Tick());
    }

    CHECK(fixture.Exists(actors[0]));
    REQUIRE(fixture.Tick());
    REQUIRE(fixture.Tick());
    CHECK(!fixture.Exists(actors[0]));
    CHECK(fixture.Exists(actors[1]));
}

// 보유(BackendLease) 때문에 늦어진 객체는 보유가 풀린 뒤의 프레임에 회수된다(회수 대기를 반환값이
// 아니라 질의로 판정 — GC 설계 17.3절).
TEST(WorldGc_HeldObjectIsReclaimedAfterRelease) {
    ScheduleFixture fixture(ObjectBudgetSchedule(), 7);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    ObjectHandle actors[1];
    REQUIRE(fixture.Spawn(actors, 1));
    auto lease = world.Objects().AcquireBackendLease(world.Access(), actors[0]);
    REQUIRE(lease);
    REQUIRE(world.DestroyActor(actors[0]));
    world.RequestFullCollection();
    for (uint32 frame = 0; frame < 4; ++frame) {
        REQUIRE(fixture.Tick());
    }

    CHECK(fixture.Exists(actors[0]));
    lease.Value().Reset();
    REQUIRE(fixture.Tick());
    CHECK(!fixture.Exists(actors[0]));
}

// 여러 World가 엔진처럼 scheduler 하나를 빌려 쓴다(World Tick은 차례로 돌므로 표식이 겹치지
// 않는다).
TEST(WorldGc_WorldsShareOneScheduler) {
    SystemAllocator allocator;
    WorkerScheduler scheduler(allocator);
    REQUIRE(scheduler.Start({2, 256, 16}));
    {
        ScheduleFixture first(ObjectBudgetSchedule(), 8, {}, &scheduler);
        ScheduleFixture second(ObjectBudgetSchedule(), 9, {}, &scheduler);
        REQUIRE(first.world && second.world);
        CHECK(first.world->GcScheduler() == &scheduler);
        CHECK(second.world->GcScheduler() == &scheduler);
        ObjectHandle a[2];
        ObjectHandle b[2];
        REQUIRE(first.Spawn(a, 2) && second.Spawn(b, 2));
        REQUIRE(first.world->DestroyActor(a[0]) && second.world->DestroyActor(b[0]));
        first.world->RequestFullCollection();
        second.world->RequestFullCollection();
        for (uint32 frame = 0; frame < 2; ++frame) {
            REQUIRE(first.Tick() && second.Tick());
        }

        CHECK(!first.Exists(a[0]) && !second.Exists(b[0]));
        CHECK(first.Exists(a[1]) && second.Exists(b[1]));
    }

    REQUIRE(scheduler.Shutdown());
}

// 회수가 남은 채 전체 수집을 다시 요청해도(메모리 압박으로 요청이 연달아 올 때) 남은 회수가
// 이어진다. 둘째 수집이 새로 분류한 객체가 0이어도 회수 대기는 질의로 판정한다(push된 처음 구현은
// 표지를 내려 회수가 멈췄다 — 회귀 시험).
TEST(WorldGc_RepeatedRequestKeepsDrainingEarlierReclaim) {
    ScheduleFixture fixture(ObjectBudgetSchedule(2), 10);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    constexpr uint32 Actors = 5;
    ObjectHandle actors[Actors];
    REQUIRE(fixture.Spawn(actors, Actors));
    REQUIRE(fixture.Tick());
    for (ObjectHandle handle : actors) {
        REQUIRE(world.DestroyActor(handle));
    }

    world.RequestFullCollection();
    REQUIRE(fixture.Tick());
    CHECK(fixture.Retired() == 5);
    world.RequestFullCollection();
    REQUIRE(fixture.Tick());
    CHECK(fixture.Retired() == 3);
    REQUIRE(fixture.Tick());
    CHECK(fixture.Retired() == 1);
    REQUIRE(fixture.Tick());
    CHECK(fixture.Retired() == 0);
    for (ObjectHandle handle : actors) {
        CHECK(!fixture.Exists(handle));
    }
}

// 증분 cycle이 Sweeping일 때 외부 Borrow를 쥔 채 요청하면 cycle을 끝내지 못하고(Busy — safepoint
// 조건) 요청이 남는다. Borrow를 놓은 다음 프레임에 cycle을 끝내고 수집한다.
TEST(WorldGc_RequestDuringSweepingWithBorrowStaysBusy) {
    ScheduleFixture fixture(ObjectBudgetSchedule(), 11, GcBudget{1, 0});
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    constexpr uint32 Actors = 4;
    ObjectHandle actors[Actors];
    REQUIRE(fixture.Spawn(actors, Actors));
    REQUIRE(fixture.Tick());
    REQUIRE(world.DestroyActor(actors[0]));
    for (uint32 frame = 0; frame < 4096 && world.Objects().CollectionPhase() != GcPhase::Sweeping;
         ++frame) {
        REQUIRE(fixture.Tick());
    }

    REQUIRE(world.Objects().CollectionPhase() == GcPhase::Sweeping);
    world.RequestFullCollection();
    {
        auto held = world.Resolve<Actor>(actors[1]);
        REQUIRE(held);
        REQUIRE(fixture.Tick());
        CHECK(world.Objects().CollectionPhase() == GcPhase::Sweeping);
    }

    REQUIRE(fixture.Tick());
    CHECK(world.Objects().CollectionPhase() == GcPhase::Idle);
    REQUIRE(fixture.Tick());
    CHECK(!fixture.Exists(actors[0]));
    CHECK(fixture.Exists(actors[1]));
}

// FullCollection 모드는 간격만 본다: World 파괴가 없어도(루트 없이 만든 일반 객체) 간격이 지나면
// 회수한다(UE와 같다 — GC 설계 17.3절).
TEST(WorldGc_FullCollectionModeCollectsWithoutWorldDestroys) {
    WorldGcSchedule schedule = ObjectBudgetSchedule();
    schedule.mode = WorldGcMode::FullCollection;
    schedule.fullCollectionIntervalSeconds = 0.25;
    ScheduleFixture fixture(schedule, 12);
    REQUIRE(fixture.world);
    REQUIRE(
        fixture.types
            .Register(TypeSchema{PlainGarbageType, 1, {}, {}}, MakeRuntimeTypeOps<PlainGarbage>())
            .IsOk());
    World& world = *fixture.world;
    ObjectHandle garbage;
    {
        auto created = world.Objects().NewObject<PlainGarbage>(world.Access(), PlainGarbageType);
        REQUIRE(created);
        garbage = created.Value().Handle();
    }

    // 0.25초는 15프레임이다. 그 전에는 남는다.
    for (uint32 frame = 0; frame < 12; ++frame) {
        REQUIRE(fixture.Tick());
    }

    CHECK(fixture.Exists(garbage));
    for (uint32 frame = 0; frame < 6; ++frame) {
        REQUIRE(fixture.Tick());
    }

    CHECK(!fixture.Exists(garbage));
}

// 주기 간격은 실제 경과 시간으로 센다: 일시정지한 World도 간격이 지나면 수집한다(UE
// FApp::GetDeltaTime과 같다 — GC 설계 17.3절). 간격 0은 주기 수집을 하지 않는다.
TEST(WorldGc_FullCollectionIntervalCountsRealTimeWhilePaused) {
    WorldGcSchedule schedule = ObjectBudgetSchedule();
    schedule.mode = WorldGcMode::FullCollection;
    schedule.fullCollectionIntervalSeconds = 0.25;
    ScheduleFixture fixture(schedule, 13);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    ObjectHandle actors[1];
    REQUIRE(fixture.Spawn(actors, 1));
    REQUIRE(fixture.Tick());
    REQUIRE(world.DestroyActor(actors[0]));
    REQUIRE(fixture.Tick());
    world.SetPaused(true);
    for (uint32 frame = 0; frame < 20; ++frame) {
        REQUIRE(fixture.Tick());
    }

    CHECK(!fixture.Exists(actors[0]));

    WorldGcSchedule never = ObjectBudgetSchedule();
    never.mode = WorldGcMode::FullCollection;
    never.fullCollectionIntervalSeconds = 0.0;
    ScheduleFixture idle(never, 14);
    REQUIRE(idle.world);
    ObjectHandle kept[1];
    REQUIRE(idle.Spawn(kept, 1));
    REQUIRE(idle.Tick());
    REQUIRE(idle.world->DestroyActor(kept[0]));
    for (uint32 frame = 0; frame < 20; ++frame) {
        REQUIRE(idle.Tick());
    }

    CHECK(idle.Exists(kept[0]));
    idle.world->RequestFullCollection();
    REQUIRE(idle.Tick());
    REQUIRE(idle.Tick());
    CHECK(!idle.Exists(kept[0]));
}
