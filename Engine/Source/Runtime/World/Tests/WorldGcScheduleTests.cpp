#include <TestSupport/Test.h>

#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Object/ModuleRegistration.h>
#include <Object/TypeRegistry.h>
#include <World/World.h>
#include <World/World.module.generated.h>

// World GC 일정(Docs/Design/GarbageCollectionScale.ko.md 17.3절): 주기 전체 수집, 전체 수집 요청,
// 회수 예산.
namespace {

using namespace rocks;

constexpr double FrameSeconds = 1.0 / 60.0;

struct ScheduleFixture {
    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    UniquePtr<World> world;

    explicit ScheduleFixture(const WorldGcSchedule& schedule, uint64 context) noexcept
        : types(allocator), modules(types, allocator) {
        if (!RegisterWorldTypes(modules)) {
            return;
        }

        WorldDesc desc;
        desc.name = "GcSchedule";
        desc.context = {0x5753434845ull, context};
        desc.timeStep = {FrameSeconds, 0.25, 8};
        desc.gcSchedule = schedule;
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
};

} // namespace

// FullCollection 모드는 파괴가 있어도 간격(World 게임 시간)이 지나기 전에는 수집하지 않는다. 간격이
// 지난 프레임에 STW 전체 수집을 하고(회수는 미룸), 다음 프레임의 예산 회수가 저장소를 반환한다.
TEST(WorldGc_FullCollectionModeWaitsForInterval) {
    WorldGcSchedule schedule;
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
    WorldGcSchedule schedule;
    schedule.reclaimBudget = GcBudget{2, 0};
    ScheduleFixture fixture(schedule, 2);
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
