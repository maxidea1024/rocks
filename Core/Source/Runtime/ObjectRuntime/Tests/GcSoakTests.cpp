#include <TestSupport/Test.h>

#include <Core/Memory/SystemAllocator.h>
#include <GcSoak/GcSoak.h>
#include <Task/WorkerScheduler.h>

#include <stdio.h>

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
