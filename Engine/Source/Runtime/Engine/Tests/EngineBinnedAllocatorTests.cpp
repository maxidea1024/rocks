#include <TestSupport/Test.h>

#include <Core/Memory/BinnedAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Engine/Engine.h>
#include <World/Actor.generated.h>

// 엔진을 BinnedAllocator 위에서 돌린다(GC 규모 설계 18.10절 "제품 연결" —
// Docs/Design/GarbageCollectionScale.ko.md). 실행 진입점(Launch)이 엔진 호스트에 넘기는 범용
// 할당자와 같은 구성이다: SystemAllocator를 상위로 둔 BinnedAllocator 하나. 엔진이 끝나면 남은
// 할당이 없고, 작은 요청이 큰 경로로 밀리지 않았으며, Trim 뒤 commit된 slab이 없다.
using namespace rocks;
namespace engine_binned_allocator_tests_file {

namespace {

constexpr double FrameSeconds = 1.0 / 60.0;
// 한 World에 만드는 Actor 수와, 그 가운데 프레임 사이에 파괴하는 수다.
constexpr uint32 ActorCount = 512;
constexpr uint32 DestroyedCount = 256;

// Actor를 만들고 앞의 절반을 파괴한 뒤 프레임을 돌려 수집되게 한다. 실패 수를 돌려준다.
uint32 SpawnAndDestroy(Engine& engine, World& world, uint32 frames) noexcept {
    uint32 failures = 0;
    ObjectHandle actors[ActorCount];
    for (ObjectHandle& handle : actors) {
        auto actor = world.SpawnActor<Actor>();
        if (!actor) {
            ++failures;
            continue;
        }

        handle = actor.Value();
    }

    for (uint32 index = 0; index < DestroyedCount; ++index) {
        if (!world.DestroyActor(actors[index])) {
            ++failures;
        }
    }

    for (uint32 frame = 0; frame < frames; ++frame) {
        if (!engine.RunFrame()) {
            ++failures;
        }
    }

    return failures;
}

// 엔진이 모두 끝난 뒤의 할당자 상태다.
void CheckAllocatorIsEmpty(BinnedAllocator& allocator) {
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Usage().fallbackAllocations == 0);
    // worker 스레드는 끝났으므로 그 캐시는 이미 돌아와 있다. 이 스레드의 캐시와 이전 칸을 비운다.
    allocator.Trim();
    CHECK(allocator.Usage().committedBytes == 0);
}

} // namespace

// GC scheduler(worker 스레드)가 있는 엔진: worker가 표식 큐 블록을 같은 할당자에서 주고받고, 전체
// 수집 요청으로 파괴한 Actor가 회수된다.
TEST(Engine_RunsOnBinnedAllocatorWithGcWorkers) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        {
            EngineConfig config;
            config.fixedFrameDeltaSeconds = FrameSeconds;
            config.gcWorkers = 2;
            Engine engine(allocator);
            REQUIRE(engine.Initialize(config));
            REQUIRE(engine.GcScheduler() != nullptr);
            WorldCreateDesc desc;
            desc.name = "Binned";
            desc.gcSchedule.mode = WorldGcMode::FullCollection;
            desc.gcSchedule.fullCollectionIntervalSeconds = 1.0e9;
            // 회수 예산은 객체 수로 둔다(시간 예산은 선점에 따라 프레임 수가 달라진다).
            desc.gcSchedule.reclaimBudget = GcBudget{GcBudget::DefaultMaxObjects, 0};
            auto created = engine.CreateWorld(desc);
            REQUIRE(created);
            World& world = *created.Value();
            CHECK(SpawnAndDestroy(engine, world, 2) == 0);
            engine.RequestFullCollection();
            constexpr uint32 CollectionFrames = 16;
            for (uint32 frame = 0; frame < CollectionFrames; ++frame) {
                REQUIRE(engine.RunFrame());
            }

            // 엔진이 살아 있는 동안에는 할당이 남아 있다(작은 블록은 이 할당자에서 나갔다).
            CHECK(allocator.Stats().liveAllocations != 0);
            CHECK(allocator.Usage().committedBytes != 0);
            REQUIRE(engine.Shutdown());
        }

        CheckAllocatorIsEmpty(allocator);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// scheduler 없는 엔진(직렬 표식)과 기본 수집 일정(증분).
TEST(Engine_RunsOnBinnedAllocatorWithSerialMarking) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        {
            EngineConfig config;
            config.fixedFrameDeltaSeconds = FrameSeconds;
            config.gcWorkers = 0;
            Engine engine(allocator);
            REQUIRE(engine.Initialize(config));
            CHECK(engine.GcScheduler() == nullptr);
            WorldCreateDesc desc;
            desc.name = "Serial";
            auto created = engine.CreateWorld(desc);
            REQUIRE(created);
            constexpr uint32 Frames = 32;
            CHECK(SpawnAndDestroy(engine, *created.Value(), Frames) == 0);
            REQUIRE(engine.Shutdown());
        }

        CheckAllocatorIsEmpty(allocator);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 같은 할당자 위에서 엔진을 두 번 살린다(같은 프로세스에서 엔진을 다시 시작하는 호스트 — slab과
// 스레드 캐시가 다시 쓰인다).
TEST(Engine_RestartsOnTheSameBinnedAllocator) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        constexpr uint32 Lifetimes = 2;
        for (uint32 lifetime = 0; lifetime < Lifetimes; ++lifetime) {
            {
                EngineConfig config;
                config.fixedFrameDeltaSeconds = FrameSeconds;
                config.gcWorkers = 2;
                Engine engine(allocator);
                REQUIRE(engine.Initialize(config));
                WorldCreateDesc desc;
                desc.name = "Restart";
                auto created = engine.CreateWorld(desc);
                REQUIRE(created);
                constexpr uint32 Frames = 8;
                CHECK(SpawnAndDestroy(engine, *created.Value(), Frames) == 0);
                REQUIRE(engine.Shutdown());
            }

            // 엔진 객체까지 파괴된 뒤에는 남은 할당이 없다(다음 수명이 같은 slab을 다시 쓴다).
            CHECK(allocator.Stats().liveAllocations == 0);
        }

        CheckAllocatorIsEmpty(allocator);
    }

    CHECK(system.Stats().liveAllocations == 0);
}
} // namespace engine_binned_allocator_tests_file
