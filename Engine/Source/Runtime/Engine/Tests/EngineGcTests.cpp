#include <TestSupport/Test.h>

#include <Core/Memory/SystemAllocator.h>
#include <Engine/Engine.h>
#include <World/World.module.generated.h>

// 엔진이 소유하는 GC scheduler와 전체 수집
// 요청이다(Docs/Design/GarbageCollectionScale.ko.md 17.4절).
using namespace rocks;

// 엔진이 GC scheduler를 만들어 World에 빌려주고, 종료 때 World를 모두 파괴한 뒤 끝낸다. 엔진의
// 전체 수집 요청은 World의 요청이 된다(주기 간격이 아주 긴 FullCollection World도 요청으로는
// 수집한다).
TEST(Engine_OwnsGcSchedulerAndLendsItToWorlds) {
    SystemAllocator allocator;
    {
        EngineConfig config;
        config.fixedFrameDeltaSeconds = 1.0 / 60.0;
        config.gcWorkers = 2;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        REQUIRE(engine.GcScheduler() != nullptr);
        WorldCreateDesc desc;
        desc.name = "Gc";
        desc.gcSchedule.mode = WorldGcMode::FullCollection;
        desc.gcSchedule.fullCollectionIntervalSeconds = 1.0e9;
        auto created = engine.CreateWorld(desc);
        REQUIRE(created);
        World& world = *created.Value();
        CHECK(world.GcScheduler() == engine.GcScheduler());
        auto actor = world.SpawnActor<Actor>();
        REQUIRE(actor);
        REQUIRE(world.DestroyActor(actor.Value()));
        REQUIRE(engine.RunFrame());
        REQUIRE(engine.RunFrame());
        // 간격이 지나지 않았으므로 아직 남아 있다.
        CHECK(world.Objects().InspectLifecycle(world.Access(), actor.Value()));
        engine.RequestFullCollection();
        REQUIRE(engine.RunFrame());
        REQUIRE(engine.RunFrame());
        CHECK(!world.Objects().InspectLifecycle(world.Access(), actor.Value()));
        REQUIRE(engine.Shutdown());
        CHECK(engine.GcScheduler() == nullptr);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// gcWorkers 0이면 scheduler를 만들지 않고 World는 직렬 표식이다.
TEST(Engine_GcWorkersZeroMeansSerialMarking) {
    SystemAllocator allocator;
    {
        EngineConfig config;
        config.fixedFrameDeltaSeconds = 1.0 / 60.0;
        config.gcWorkers = 0;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        CHECK(engine.GcScheduler() == nullptr);
        WorldCreateDesc desc;
        desc.name = "Serial";
        auto created = engine.CreateWorld(desc);
        REQUIRE(created);
        CHECK(created.Value()->GcScheduler() == nullptr);
        REQUIRE(engine.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
