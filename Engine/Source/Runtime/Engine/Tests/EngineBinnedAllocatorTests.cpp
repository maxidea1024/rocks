#include <TestSupport/Test.h>

#include <Core/Memory/BinnedAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Engine/Engine.h>
#include <Platform/Atomics.h>
#include <World/Actor.generated.h>

// 엔진을 BinnedAllocator 위에서 돌린다(GC 규모 설계 18.10절 "제품 연결" —
// Docs/Design/GarbageCollectionScale.ko.md). 실행 진입점(Launch)이 엔진 호스트에 넘기는 범용
// 할당자와 같은 구성이다: SystemAllocator를 상위로 둔 BinnedAllocator 하나. 엔진이 끝나면 남은
// 할당이 없고, 작은 요청이 큰 경로로 밀리지 않았으며, TrimMemory 뒤 commit된 slab이 없다.
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
    allocator.TrimMemory();
    CHECK(allocator.Usage().committedBytes == 0);
}

} // namespace

// GC scheduler(worker 스레드)가 있는 엔진에서 Actor를 만들고 파괴한 뒤 전체 수집을 요청해 돌린다.
// 엔진이 살아 있는 동안 받은 할당자에 할당이 있고(엔진이 그 할당자를 쓴다), 끝나면 모두 돌아온다.
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
            // 엔진이 받은 할당자를 쓴다.
            CHECK(allocator.Stats().liveAllocations != 0);
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
                // 엔진이 받은 할당자를 쓴다.
                CHECK(allocator.Stats().liveAllocations != 0);
                REQUIRE(engine.Shutdown());
            }

            // 엔진 객체까지 파괴된 뒤에는 남은 할당이 없다(다음 수명이 같은 slab을 다시 쓴다).
            CHECK(allocator.Stats().liveAllocations == 0);
        }

        CheckAllocatorIsEmpty(allocator);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

namespace {

// TrimMemory 호출을 세는 allocator다(나머지는 상위로 그대로 넘긴다 — 쉬는 표시도).
class TrimCounter final : public Allocator {
public:
    explicit TrimCounter(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return backing_.Deallocate(address);
    }

    void TrimMemory() noexcept override {
        (void)platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&trims_, uint32{1});
        backing_.TrimMemory();
    }

    void MarkThreadIdle() noexcept override {
        backing_.MarkThreadIdle();
    }

    void MarkThreadActive() noexcept override {
        backing_.MarkThreadActive();
    }

    uint32 Trims() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&trims_);
    }

private:
    Allocator& backing_;
    uint32 trims_ = 0;
};

// 프레임마다 회수하는 객체 수다(파괴한 Actor 256개의 회수가 여러 프레임에 걸치게 한다).
constexpr uint32 ReclaimObjectsPerFrame = 32;
// 미룬 회수가 끝나기를 기다리는 프레임 수의 상한이다(파괴한 객체 수 / 프레임당 회수 수보다 넉넉히).
constexpr uint32 ReclaimFrameLimit = 256;
// 사실상 오지 않는 주기 수집 간격이다(요청한 전체 수집만 돈다).
constexpr double NoPeriodicCollectionSeconds = 1.0e9;

} // namespace

// 엔진이 TrimMemory를 부르는 자리 둘이다(GC 규모 설계 18.11.6절): 전체 수집의 회수가 끝난 프레임에
// 한 번(미룬 회수가 남아 있는 동안은 부르지 않고, 회수할 것이 없으면 수집한 프레임에 바로), World를
// 파괴한 뒤 한 번.
TEST(Engine_TrimsMemoryAfterFullCollectionReclaimAndWorldDestroy) {
    SystemAllocator system;
    {
        BinnedAllocator binned(system);
        TrimCounter allocator(binned);
        {
            EngineConfig config;
            config.fixedFrameDeltaSeconds = FrameSeconds;
            config.gcWorkers = 2;
            Engine engine(allocator);
            REQUIRE(engine.Initialize(config));
            WorldCreateDesc desc;
            desc.name = "Trim";
            desc.gcSchedule.mode = WorldGcMode::FullCollection;
            desc.gcSchedule.fullCollectionIntervalSeconds = NoPeriodicCollectionSeconds;
            // 회수 예산은 객체 수로 둔다(시간 예산은 선점에 따라 프레임 수가 달라진다).
            desc.gcSchedule.reclaimBudget = GcBudget{ReclaimObjectsPerFrame, 0};
            auto created = engine.CreateWorld(desc);
            REQUIRE(created);
            World* world = created.Value();
            CHECK(SpawnAndDestroy(engine, *world, 2) == 0);
            CHECK(allocator.Trims() == 0);

            // 전체 수집을 한 프레임: 회수가 미뤄져 있으므로 아직 부르지 않는다.
            engine.RequestFullCollection();
            REQUIRE(engine.RunFrame());
            CHECK(allocator.Trims() == 0);
            REQUIRE(engine.RunFrame());
            CHECK(allocator.Trims() == 0);
            // 회수가 다 빠진 프레임에 한 번 부른다.
            uint32 frames = 0;
            while (allocator.Trims() == 0 && frames < ReclaimFrameLimit) {
                REQUIRE(engine.RunFrame());
                ++frames;
            }

            CHECK(allocator.Trims() == 1);
            // 그 뒤의 프레임은 부르지 않는다(전체 수집마다 한 번이다).
            constexpr uint32 QuietFrames = 4;
            for (uint32 frame = 0; frame < QuietFrames; ++frame) {
                REQUIRE(engine.RunFrame());
            }

            CHECK(allocator.Trims() == 1);

            // 회수할 것이 없는 전체 수집은 수집한 프레임에 바로 부른다.
            engine.RequestFullCollection();
            REQUIRE(engine.RunFrame());
            CHECK(allocator.Trims() == 2);

            // World를 파괴한 뒤에 한 번 부른다.
            REQUIRE(engine.DestroyWorld(world));
            CHECK(allocator.Trims() == 3);
            REQUIRE(engine.Shutdown());
            CHECK(allocator.Trims() == 3);
        }

        CheckAllocatorIsEmpty(binned);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 증분 cycle이 끝날 때는 TrimMemory를 부르지 않고, WorldGcSchedule::trimMemory를 끄면 두 자리 모두
// 부르지 않는다.
TEST(Engine_DoesNotTrimMemoryOnIncrementalCyclesOrWhenDisabled) {
    SystemAllocator system;
    {
        BinnedAllocator binned(system);
        TrimCounter allocator(binned);
        {
            EngineConfig config;
            config.fixedFrameDeltaSeconds = FrameSeconds;
            config.gcWorkers = 0;
            Engine engine(allocator);
            REQUIRE(engine.Initialize(config));
            // 기본 일정(증분): Actor 파괴가 증분 cycle을 돌려 회수한다.
            WorldCreateDesc incremental;
            incremental.name = "Incremental";
            auto first = engine.CreateWorld(incremental);
            REQUIRE(first);
            constexpr uint32 Frames = 32;
            CHECK(SpawnAndDestroy(engine, *first.Value(), Frames) == 0);
            CHECK(allocator.Trims() == 0);

            // 설정을 끈 World: 전체 수집의 회수가 끝나도, 파괴해도 부르지 않는다.
            WorldCreateDesc disabled;
            disabled.name = "Disabled";
            disabled.gcSchedule.trimMemory = false;
            auto second = engine.CreateWorld(disabled);
            REQUIRE(second);
            CHECK(SpawnAndDestroy(engine, *second.Value(), 2) == 0);
            second.Value()->RequestFullCollection();
            for (uint32 frame = 0; frame < Frames; ++frame) {
                REQUIRE(engine.RunFrame());
            }

            CHECK(allocator.Trims() == 0);
            REQUIRE(engine.DestroyWorld(second.Value()));
            CHECK(allocator.Trims() == 0);
            // 켜 둔 World(기본)를 파괴하면 부른다.
            REQUIRE(engine.DestroyWorld(first.Value()));
            CHECK(allocator.Trims() == 1);
            REQUIRE(engine.Shutdown());
        }

        CheckAllocatorIsEmpty(binned);
    }

    CHECK(system.Stats().liveAllocations == 0);
}
} // namespace engine_binned_allocator_tests_file
