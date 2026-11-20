#include <TestSupport/Test.h>

#include <Core/Memory/BinnedAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <GcSoak/GcSoak.h>
#include <Task/WorkerScheduler.h>

#include <stdio.h>

namespace gc_soak_tests_file {

// 장기 GC smoke 시뮬레이터(GcSoak)의 짧은 실행이다(Docs/Design/GarbageCollectionScale.ko.md 9절).
// 긴 실행은 GcBench --soak으로 한다. 실패하면 첫 위반의 설명·tick·모델 객체를 stderr에 적는다.
namespace {

using namespace rocks;

void PrintFailure(const GcSoakReport& report) noexcept {
    if (report.failure != nullptr) {
        fprintf(stderr, "GcSoak failure at tick %llu (object %u, error %u): %s\n",
                static_cast<unsigned long long>(report.failureTick), report.failureObject,
                static_cast<unsigned>(report.failureError), report.failure);
    }
}

} // namespace

// 직렬 수집과 객체 수 예산(Step마다 1–64개): seed마다 결정적인 실행이다.
TEST(GcSoak_SerialCollectionKeepsModelInvariants) {
    for (uint64 seed = 0; seed < 3; ++seed) {
        SystemAllocator allocator;
        GcSoakOptions options;
        options.seed = rocks::test::CurrentSeed() * 1000 + seed;
        options.ticks = 3000;
        options.maxObjects = 600;
        GcSoakReport report;
        const bool passed = RunGcSoak(allocator, options, report);
        PrintFailure(report);
        CHECK(passed);
        CHECK(report.ticks == options.ticks);
        // 실행이 실제로 수집·회수·slice 안 cross-thread 반환을 거쳤는지 본다.
        CHECK(report.cyclesCompleted >= 5);
        CHECK(report.reclaimed > 100);
        CHECK(report.crossThreadInSlice > 0);
        CHECK(report.crossThreadOutside > 0);
        CHECK(report.cancels + report.collects + report.drains > 0);
        CHECK(allocator.Stats().liveAllocations == 0);
    }
}

// scheduler 경로(parallelTraceSafe 타입은 worker가 trace)다. 표식 순서가 실행마다 달라도 불변식은
// 순서와 무관해야 한다.
TEST(GcSoak_SchedulerCollectionKeepsModelInvariants) {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({3, 256, 16}));
        GcSoakOptions options;
        options.seed = rocks::test::CurrentSeed() * 1000 + 77;
        options.ticks = 2000;
        options.maxObjects = 600;
        options.scheduler = &scheduler;
        GcSoakReport report;
        const bool passed = RunGcSoak(allocator, options, report);
        PrintFailure(report);
        CHECK(passed);
        CHECK(report.cyclesCompleted >= 3);
        CHECK(report.reclaimed > 50);
        REQUIRE(scheduler.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

void PrintClusterFailure(const GcClusterSoakReport& report) noexcept {
    if (report.failure != nullptr) {
        fprintf(stderr, "GcClusterSoak failure at tick %llu (object %u, error %u): %s\n",
                static_cast<unsigned long long>(report.failureTick), report.failureObject,
                static_cast<unsigned>(report.failureError), report.failure);
    }
}

} // namespace

// 클러스터 smoke(GC-S7): World형과 패키지형, 직렬과 scheduler 경로다.
TEST(GcSoak_ClusterWorldKeepsOracle) {
    for (uint64 seed = 0; seed < 2; ++seed) {
        SystemAllocator allocator;
        GcClusterSoakOptions options;
        options.seed = rocks::test::CurrentSeed() * 1000 + seed;
        options.ticks = 60;
        options.maxObjects = 700;
        GcClusterSoakReport report;
        const bool passed = RunGcClusterSoak(allocator, options, report);
        PrintClusterFailure(report);
        CHECK(passed);
        CHECK(report.cyclesCompleted > 30);
        CHECK(report.reclaimed > 20);
        CHECK(report.destroys > 20);
        CHECK(allocator.Stats().liveAllocations == 0);
    }
}

TEST(GcSoak_ClusterPackagesKeepOracle) {
    for (uint64 seed = 0; seed < 2; ++seed) {
        SystemAllocator allocator;
        GcClusterSoakOptions options;
        options.seed = rocks::test::CurrentSeed() * 1000 + 40 + seed;
        options.ticks = 60;
        options.maxObjects = 700;
        options.packages = true;
        GcClusterSoakReport report;
        const bool passed = RunGcClusterSoak(allocator, options, report);
        PrintClusterFailure(report);
        CHECK(passed);
        CHECK(report.cyclesCompleted > 30);
        CHECK(report.reclaimed > 20);
        CHECK(report.discards > 10);
        CHECK(allocator.Stats().liveAllocations == 0);
    }
}

TEST(GcSoak_ClusterSchedulerKeepsOracle) {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({3, 256, 16}));
        for (uint32 packages = 0; packages < 2; ++packages) {
            GcClusterSoakOptions options;
            options.seed = rocks::test::CurrentSeed() * 1000 + 90 + packages;
            options.ticks = 40;
            options.maxObjects = 600;
            options.packages = packages != 0;
            options.scheduler = &scheduler;
            GcClusterSoakReport report;
            const bool passed = RunGcClusterSoak(allocator, options, report);
            PrintClusterFailure(report);
            CHECK(passed);
        }

        REQUIRE(scheduler.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 모델 상한(maxObjects)에 닿은 뒤에도 smoke가 모델을 키우지 않는다(Level 생성도 상한을 지킨다 —
// 상한 없이 커지면 oracle 비용이 tick마다 늘어 긴 실행이 끝나지 않는다). 작은 상한과 긴 실행으로
// 상한에 일찍 닿게 한다.
TEST(GcSoak_ClusterWorldStaysWithinModelLimit) {
    SystemAllocator allocator;
    GcClusterSoakOptions options;
    options.seed = 31;
    options.ticks = 600;
    options.maxObjects = 120;
    GcClusterSoakReport report;
    const bool passed = RunGcClusterSoak(allocator, options, report);
    PrintClusterFailure(report);
    CHECK(passed);
    CHECK(report.ticks == options.ticks);
    CHECK(report.created <= options.maxObjects);
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 같은 장기 smoke를 BinnedAllocator 위에서 돈다(Docs/Design/GarbageCollectionScale.ko.md 18.9절).
// 객체 저장소와 Registry의 작은 할당은 스레드 캐시·slab을, GC 페이지·Slot chunk는 큰 경로를 지난다.
// 끝나면 남은 할당이 없고, TrimMemory 뒤 commit된 slab이 없으며, 작은 요청이 큰 경로로 밀려나지
// 않았다.
TEST(GcSoak_SerialCollectionOnBinnedAllocator) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        GcSoakOptions options;
        options.seed = rocks::test::CurrentSeed() * 1000 + 31;
        options.ticks = 3000;
        options.maxObjects = 600;
        GcSoakReport report;
        const bool passed = RunGcSoak(allocator, options, report);
        PrintFailure(report);
        CHECK(passed);
        CHECK(report.ticks == options.ticks);
        CHECK(report.cyclesCompleted >= 5);
        CHECK(report.reclaimed > 100);
        CHECK(report.crossThreadInSlice > 0);
        CHECK(report.crossThreadOutside > 0);
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().liveBytes == 0);
        CHECK(allocator.Stats().totalAllocations > report.reclaimed);
        CHECK(allocator.Usage().fallbackAllocations == 0);
        allocator.TrimMemory();
        CHECK(allocator.Usage().committedBytes == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// scheduler 경로: worker 스레드가 표식 큐 블록을 같은 allocator에서 받고 돌려준다(스레드마다 캐시가
// 생기고, scheduler가 멈추면 스레드 종료 정리로 돌아간다).
TEST(GcSoak_SchedulerCollectionOnBinnedAllocator) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        {
            WorkerScheduler scheduler(allocator);
            REQUIRE(scheduler.Start({3, 256, 16}));
            GcSoakOptions options;
            options.seed = rocks::test::CurrentSeed() * 1000 + 78;
            options.ticks = 2000;
            options.maxObjects = 600;
            options.scheduler = &scheduler;
            GcSoakReport report;
            const bool passed = RunGcSoak(allocator, options, report);
            PrintFailure(report);
            CHECK(passed);
            CHECK(report.cyclesCompleted >= 3);
            CHECK(report.reclaimed > 50);
            REQUIRE(scheduler.Shutdown());
        }

        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Usage().fallbackAllocations == 0);
        // worker 스레드는 끝났으므로 그 캐시는 이미 돌아와 있다. 이 스레드의 캐시와 이전 칸을
        // 비운다.
        allocator.TrimMemory();
        CHECK(allocator.Usage().committedBytes == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 클러스터 smoke(World Level 모양)도 같다.
TEST(GcSoak_ClusterWorldOnBinnedAllocator) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        GcClusterSoakOptions options;
        options.seed = rocks::test::CurrentSeed() * 1000 + 5;
        options.ticks = 60;
        options.maxObjects = 700;
        GcClusterSoakReport report;
        const bool passed = RunGcClusterSoak(allocator, options, report);
        PrintClusterFailure(report);
        CHECK(passed);
        CHECK(report.cyclesCompleted > 30);
        CHECK(report.reclaimed > 20);
        CHECK(report.destroys > 20);
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Usage().fallbackAllocations == 0);
        allocator.TrimMemory();
        CHECK(allocator.Usage().committedBytes == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}
} // namespace gc_soak_tests_file
