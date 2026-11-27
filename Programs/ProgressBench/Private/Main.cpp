#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Launch/EntryPoint.h>
#include <Localization/Text.h>
#include <Platform/Clock.h>
#include <Progress/ProgressBinding.h>
#include <Progress/ProgressCounter.h>
#include <Progress/ProgressHub.h>
#include <Progress/ProgressScope.h>

#include <stdio.h>

// 진행 보고 비용 측정 도구다(Docs/Design/ProgressReporting 2장 요구 9, 15.5절). 묶음 전체 시간을
// 건수로 나눈 평균(ns/호출)을 낸다. 목표치는 보장이 아니라 측정해 보고하는 값이다.
//
// 재는 것:
// - unbound scope: 바인딩 없는 스레드에서 ProgressScope 생성 + EnterStep + 소멸
// - unbound step: 바인딩 없는 스레드의 열린 구간에서 문구 없는 EnterStep
// - bound step: Primary 바인딩 아래 문구 없는 EnterStep(seqlock 쓰기)
// - bound scope: Primary 바인딩 아래 구간 생성 + EnterStep + 소멸(trace 꺼짐)
// - counter add: 작업 수준 계수 Add(원자 더하기)
// - snapshot N: 살아 있는 작업 N개의 스냅샷(같은 ProgressSnapshot 재사용)
//
// 사용: ProgressBench. Release로 빌드해 같은 기기에서 비교한다.
namespace {

using namespace rocks;

constexpr uint32 Calls = 1'000'000;
constexpr uint32 SnapshotCalls = 10'000;
constexpr uint32 SnapshotTaskCounts[] = {1, 64, 1024};
constexpr double NanosecondsPerSecond = 1'000'000'000.0;

double TicksToNanoseconds(uint64 ticks) {
    return static_cast<double>(ticks) * NanosecondsPerSecond /
           static_cast<double>(platform::MonotonicTickFrequency());
}

// 최적화가 빈 반복을 지우지 않게 하는 값이다.
volatile uint32 sink = 0;

template <typename Call>
void Measure(const char* name, uint32 count, Call&& call) {
    const uint64 start = platform::MonotonicTicks();
    for (uint32 index = 0; index < count; ++index) {
        call(index);
    }

    const uint64 elapsed = platform::MonotonicTicks() - start;
    printf("%-16s mean %8.2f ns  (%u calls)\n", name, TicksToNanoseconds(elapsed) / count, count);
}

int RunMain(launch::ApplicationContext& context) noexcept {
    (void)context;
    SystemAllocator allocator;
    ProgressHub& hub = ProgressHub::Get();
    if (!hub.Initialize(allocator, {})) {
        printf("Progress hub did not start\n");
        return 1;
    }

    auto title = Text::Invariant(allocator, "Bench");
    if (!title) {
        printf("Text creation failed\n");
        return 1;
    }

    ProgressTaskDesc desc;
    desc.title = &title.Value();
    Measure("unbound scope", Calls, [](uint32) noexcept {
        ProgressScope scope(1.0);
        scope.EnterStep(1.0);
    });
    {
        ProgressScope scope(static_cast<double>(Calls));
        Measure("unbound step", Calls, [&](uint32) noexcept { scope.EnterStep(1.0); });
    }

    auto task = hub.Begin(desc);
    if (!task) {
        printf("Begin failed\n");
        return 1;
    }

    {
        auto binding = ProgressBinding::Bind(task.Value().Id(), ProgressBindingRole::Primary);
        if (!binding) {
            printf("Bind failed\n");
            return 1;
        }

        {
            ProgressScope scope(static_cast<double>(Calls));
            Measure("bound step", Calls, [&](uint32) noexcept { scope.EnterStep(1.0); });
        }

        ProgressScope outer(static_cast<double>(Calls));
        Measure("bound scope", Calls, [&](uint32) noexcept {
            outer.EnterStep(1.0);
            ProgressScope scope(2.0);
            scope.EnterStep(1.0);
        });
    }

    {
        auto counter = ProgressCounter::ForTask(task.Value(), Calls);
        if (!counter) {
            printf("Counter failed\n");
            return 1;
        }

        Measure("counter add", Calls, [&](uint32) noexcept { counter.Value().Add(); });
    }

    (void)task.Value().Finish(ProgressOutcome::Succeeded);
    for (const uint32 taskCount : SnapshotTaskCounts) {
        Array<ProgressTaskHandle> tasks(allocator);
        for (uint32 index = 0; index < taskCount; ++index) {
            auto live = hub.Begin(desc);
            if (!live || !tasks.TryPushBack(Move(live.Value()))) {
                printf("Begin failed\n");
                return 1;
            }
        }

        ProgressSnapshot snapshot(allocator);
        uint64 cursor = 0;
        char name[32];
        (void)snprintf(name, sizeof(name), "snapshot %u", taskCount);
        Measure(name, SnapshotCalls,
                [&](uint32) noexcept { sink = sink + (hub.Snapshot(snapshot, cursor) ? 1u : 0u); });
        for (ProgressTaskHandle& live : tasks) {
            (void)live.Finish(ProgressOutcome::Succeeded);
        }
    }

    hub.Shutdown();
    return 0;
}

const launch::ApplicationDescription& Describe() noexcept {
    static constexpr launch::ApplicationDescription Description{
        .name = "ProgressBench", .kind = launch::ApplicationKind::Tool, .run = &RunMain};
    return Description;
}

} // namespace

ROCKS_IMPLEMENT_APPLICATION(Describe)
