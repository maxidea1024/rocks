#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Localization/Catalog.h>
#include <Localization/Text.h>
#include <Progress/ProgressBinding.h>
#include <Progress/ProgressHub.h>
#include <Progress/ProgressParallelFor.h>
#include <Platform/PlatformMath.h>
#include <Progress/ProgressScope.h>
#include <TestSupport/OsThread.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace progress_scope_tests_file {

// 시험은 시간 운에 기대지 않는다: 시계는 주입하고 시험이 직접
// 전진한다(Docs/Design/ProgressReporting 14장). 다른 스레드 일은 RunOnOsThread(만들고 끝까지
// 기다림)로 순서가 정해진다.
struct FakeClock {
    uint64 now = 0;
};

uint64 ReadFakeClock(void* context) noexcept {
    return static_cast<FakeClock*>(context)->now;
}

constexpr uint64 PumpIntervalNanoseconds = 200'000'000;
constexpr double Tolerance = 1e-12;

bool Near(double left, double right) noexcept {
    const double difference = left - right;
    return difference < Tolerance && difference > -Tolerance;
}

struct Hub {
    SystemAllocator allocator;
    FakeClock clock;
    bool ready = false;

    Hub() noexcept {
        ProgressHubSettings settings;
        settings.clock = ReadFakeClock;
        settings.clockContext = &clock;
        ready = static_cast<bool>(ProgressHub::Get().Initialize(allocator, settings));
    }

    ~Hub() noexcept {
        ProgressHub::Get().Shutdown();
    }

    Text MakeText(StringView source) noexcept {
        auto text = Text::Invariant(allocator, source);
        return text ? Move(text.Value()) : Text();
    }

    ProgressTaskHandle Begin(StringView title, ProgressTaskId parent = NoProgressTask) noexcept {
        const Text text = MakeText(title);
        ProgressTaskDesc desc;
        desc.title = &text;
        desc.parent = parent;
        auto task = ProgressHub::Get().Begin(desc);
        return task ? Move(task.Value()) : ProgressTaskHandle();
    }

    // 작업 하나의 보기를 찾아 out에 담는다(없으면 false).
    bool View(ProgressSnapshot& snapshot, ProgressTaskId id,
              const ProgressTaskView*& out) noexcept {
        uint64 cursor = 0;
        if (!ProgressHub::Get().Snapshot(snapshot, cursor)) {
            return false;
        }

        for (const ProgressTaskView& task : snapshot.tasks) {
            if (task.id == id) {
                out = &task;
                return true;
            }
        }

        return false;
    }

    double Visible(ProgressTaskId id) noexcept {
        ProgressSnapshot snapshot(allocator);
        const ProgressTaskView* view = nullptr;
        return View(snapshot, id, view) ? view->fraction : -1.0;
    }

    ProgressShape Shape(ProgressTaskId id) noexcept {
        ProgressSnapshot snapshot(allocator);
        const ProgressTaskView* view = nullptr;
        return View(snapshot, id, view) ? view->shape : ProgressShape::Indeterminate;
    }

    bool SameSource(const Text& text, StringView expected) noexcept {
        auto resolved = ResolveText(allocator, text, CatalogSnapshot());
        return resolved && resolved.Value().value.View() == expected;
    }
};

// 시험 3: 3단 중첩, 가중치가 다른 구간, 남은 양 채움 — UE FSlowTaskStack 곱셈 합성과 같은 값이다.
//   바깥 10: [0,2) [2,7) [7,10)  →  가운데(4)가 [0.2, 0.7)을 1:3으로  →  안쪽(3)이 [0.325, 0.7)을
//   1:2로
TEST(Progress_NestedScopesComposeLikeUnrealSlowTasks) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressTaskHandle task = hub.Begin("Build");
    REQUIRE(task.IsValid());
    {
        auto binding = ProgressBinding::Bind(task.Id(), ProgressBindingRole::Primary);
        REQUIRE(binding);
        ProgressScope outer(10.0);
        CHECK(hub.Shape(task.Id()) == ProgressShape::Fraction);
        outer.EnterStep(2.0);
        CHECK(Near(hub.Visible(task.Id()), 0.0));
        outer.EnterStep(5.0);
        CHECK(Near(hub.Visible(task.Id()), 0.2));
        {
            ProgressScope middle(4.0);
            middle.EnterStep(1.0);
            CHECK(Near(hub.Visible(task.Id()), 0.2));
            middle.EnterStep(3.0);
            CHECK(Near(hub.Visible(task.Id()), 0.325));
            {
                ProgressScope inner(3.0);
                inner.EnterStep(1.0);
                CHECK(Near(hub.Visible(task.Id()), 0.325));
                inner.EnterStep(2.0);
                CHECK(Near(hub.Visible(task.Id()), 0.45));
            }

            // 안쪽 범위 소멸은 남은 양을 채운다(가운데 둘째 구간 끝 = 0.7).
            CHECK(Near(hub.Visible(task.Id()), 0.7));
        }

        CHECK(Near(hub.Visible(task.Id()), 0.7));
        outer.EnterStep(3.0);
        CHECK(Near(hub.Visible(task.Id()), 0.7));
    }

    // 가장 바깥 범위가 닫히면 범위 끝 값이다.
    CHECK(Near(hub.Visible(task.Id()), 1.0));
    CHECK(task.Finish(ProgressOutcome::Succeeded));
}

// 시험 3: 총량 0 범위는 범위 전체가 한 구간이라 안쪽 범위가 그 전체를 채운다.
TEST(Progress_ZeroTotalScopeIsOneWholeStep) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressTaskHandle task = hub.Begin("Zero");
    {
        auto binding = ProgressBinding::Bind(task.Id(), ProgressBindingRole::Primary);
        REQUIRE(binding);
        ProgressScope outer(2.0);
        outer.EnterStep(1.0);
        {
            ProgressScope zero(0.0);
            ProgressScope nested(2.0);
            nested.EnterStep(1.0);
            nested.EnterStep(1.0);
            CHECK(Near(hub.Visible(task.Id()), 0.25));
        }

        CHECK(Near(hub.Visible(task.Id()), 0.5));
    }

    CHECK(task.Finish(ProgressOutcome::Succeeded));
}

// 시험 4: 음수·NaN·무한 작업량은 0, 총량을 넘는 EnterStep은 잘림 — 값과 Warning 1회. 부동소수 합의
// 작은 넘침은 경고하지 않는다.
TEST(Progress_ScopeInputChecksClampAndWarnOnce) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressTaskHandle task = hub.Begin("Input");
    {
        auto binding = ProgressBinding::Bind(task.Id(), ProgressBindingRole::Primary);
        REQUIRE(binding);
        {
            ProgressScope sums(1.0);
            sums.EnterStep(0.1);
            sums.EnterStep(0.2);
            sums.EnterStep(0.7);
        }

        CHECK(ProgressHub::Get().Stats().stepOverflowWarnings == 0);
        ProgressTaskHandle second = hub.Begin("Input 2");
        auto inner = ProgressBinding::Bind(second.Id(), ProgressBindingRole::Primary);
        REQUIRE(inner);
        {
            ProgressScope scope(2.0);
            scope.EnterStep(-1.0);
            scope.EnterStep(platform::PlatformMath::QuietNaN<double>());
            scope.EnterStep(platform::PlatformMath::Infinity<double>());
            CHECK(ProgressHub::Get().Stats().invalidWorkWarnings == 1);
            scope.EnterStep(1.5);
            CHECK(Near(hub.Visible(second.Id()), 0.0));
            scope.EnterStep(5.0);
            CHECK(Near(hub.Visible(second.Id()), 0.75));
            scope.EnterStep(5.0);
            CHECK(ProgressHub::Get().Stats().stepOverflowWarnings == 1);
            CHECK(Near(hub.Visible(second.Id()), 1.0));
        }

        CHECK(second.Finish(ProgressOutcome::Succeeded));
    }

    CHECK(task.Finish(ProgressOutcome::Succeeded));
}

// 깊이 상한을 넘는 구간을 count개 더 연다(재귀로 스택 객체를 쌓는다).
void OpenNested(uint32 remaining) noexcept {
    if (remaining == 0) {
        return;
    }

    ProgressScope scope(1.0);
    scope.EnterStep(1.0);
    OpenNested(remaining - 1);
}

// 시험 4: 깊이 MaxProgressScopeDepth를 넘는 구간은 아무 일도 하지 않고 Warning 1회다.
TEST(Progress_ScopeDepthLimitWarnsOnce) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressTaskHandle task = hub.Begin("Depth");
    {
        auto binding = ProgressBinding::Bind(task.Id(), ProgressBindingRole::Primary);
        REQUIRE(binding);
        OpenNested(MaxProgressScopeDepth);
        CHECK(ProgressHub::Get().Stats().scopeDepthWarnings == 0);
        OpenNested(MaxProgressScopeDepth + 2);
        CHECK(ProgressHub::Get().Stats().scopeDepthWarnings == 1);
    }

    CHECK(Near(hub.Visible(task.Id()), 1.0));
    CHECK(task.Finish(ProgressOutcome::Succeeded));
}

// 시험 5: 바인딩 없는 스레드와 ProgressBindingSuspend 아래의 구간은 아무 일도 하지 않고 할당이
// 없다.
TEST(Progress_UnboundScopesDoNothingAndDoNotAllocate) {
    Hub hub;
    REQUIRE(hub.ready);
    const Text message = hub.MakeText("Message");
    ProgressTaskHandle task = hub.Begin("Unbound");
    const uint64 before = hub.allocator.Stats().totalAllocations;
    {
        ProgressScope scope(2.0, message);
        scope.EnterStep(1.0, message);
        scope.Tick();
        CHECK(!scope.IsCancellationRequested());
        auto counter = scope.MakeCounter(4);
        REQUIRE(counter);
        CHECK(!counter.Value().IsActive());
        counter.Value().Add();
    }

    CHECK(hub.allocator.Stats().totalAllocations == before);
    {
        auto binding = ProgressBinding::Bind(task.Id(), ProgressBindingRole::Primary);
        REQUIRE(binding);
        const uint64 bound = hub.allocator.Stats().totalAllocations;
        {
            ProgressBindingSuspend suspend;
            ProgressScope scope(2.0, message);
            scope.EnterStep(1.0, message);
            scope.EnterStep(1.0);
        }

        CHECK(hub.allocator.Stats().totalAllocations == bound);
    }

    {
        ProgressSnapshot snapshot(hub.allocator);
        const ProgressTaskView* view = nullptr;
        REQUIRE(hub.View(snapshot, task.Id(), view));
        CHECK(view->messageCount == 0);
    }

    CHECK(hub.Shape(task.Id()) == ProgressShape::Indeterminate);
    CHECK(task.Finish(ProgressOutcome::Succeeded));
}

// 시험 9(요구 9): 바인딩이 있어도 문구 없는 EnterStep·Tick·계수 Add는 할당이 없다.
TEST(Progress_BoundStepsWithoutMessagesDoNotAllocate) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressTaskHandle task = hub.Begin("Bound");
    {
        auto binding = ProgressBinding::Bind(task.Id(), ProgressBindingRole::Primary);
        REQUIRE(binding);
        ProgressScope scope(100.0);
        auto counter = scope.MakeCounter(10);
        REQUIRE(counter);
        const uint64 before = hub.allocator.Stats().totalAllocations;
        for (uint32 step = 0; step < 100; ++step) {
            scope.EnterStep(1.0);
            scope.Tick();
            counter.Value().Add();
        }

        CHECK(hub.allocator.Stats().totalAllocations == before);
    }

    CHECK(task.Finish(ProgressOutcome::Succeeded));
}

struct BindAttempt {
    ProgressTaskId id = NoProgressTask;
    ErrorCode error = ErrorCode::None;
};

void BindPrimaryOnThread(void* context) noexcept {
    auto& attempt = *static_cast<BindAttempt*>(context);
    auto binding = ProgressBinding::Bind(attempt.id, ProgressBindingRole::Primary);
    attempt.error = binding ? ErrorCode::None : binding.Error();
}

struct FinishAttempt {
    ProgressTaskHandle* task = nullptr;
    bool finished = false;
};

void FinishOnThread(void* context) noexcept {
    auto& attempt = *static_cast<FinishAttempt*>(context);
    attempt.finished = static_cast<bool>(attempt.task->Finish(ProgressOutcome::Succeeded));
}

// 시험 6: Primary는 동시에 하나(다른 스레드도 AlreadyExists), 풀린 뒤에는 다른 스레드가 Primary가
// 될 수 있다. 끝난 작업 StaleHandle, 발급된 적 없는 id NotFound.
TEST(Progress_PrimaryBindingIsExclusive) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressTaskHandle task = hub.Begin("Exclusive");
    BindAttempt attempt;
    attempt.id = task.Id();
    {
        auto binding = ProgressBinding::Bind(task.Id(), ProgressBindingRole::Primary);
        REQUIRE(binding);
        CHECK(ProgressBinding::Bind(task.Id(), ProgressBindingRole::Primary).Error() ==
              ErrorCode::AlreadyExists);
        REQUIRE(test::RunOnOsThread(BindPrimaryOnThread, &attempt));
        CHECK(attempt.error == ErrorCode::AlreadyExists);
        auto secondary = ProgressBinding::Bind(task.Id(), ProgressBindingRole::Secondary);
        CHECK(secondary);
    }

    REQUIRE(test::RunOnOsThread(BindPrimaryOnThread, &attempt));
    CHECK(attempt.error == ErrorCode::None);
    CHECK(ProgressBinding::Bind(task.Id() + 1000, ProgressBindingRole::Primary).Error() ==
          ErrorCode::NotFound);
    CHECK(ProgressBinding::Bind(NoProgressTask, ProgressBindingRole::Primary).Error() ==
          ErrorCode::NotFound);
    CHECK(task.Finish(ProgressOutcome::Succeeded));
    CHECK(ProgressBinding::Bind(task.Id(), ProgressBindingRole::Primary).Error() ==
          ErrorCode::StaleHandle);
}

// remaining개의 Secondary 바인딩을 쌓은 뒤 하나 더 묶어 본 결과다(재귀로 스택 객체를 쌓는다).
ErrorCode BindNested(ProgressTaskId id, uint32 remaining) noexcept {
    if (remaining == 0) {
        return ProgressBinding::Bind(id, ProgressBindingRole::Secondary).Error();
    }

    auto binding = ProgressBinding::Bind(id, ProgressBindingRole::Secondary);
    if (!binding) {
        return binding.Error();
    }

    return BindNested(id, remaining - 1);
}

// 시험 6: 스택 순서(맨 위가 현재 작업), Secondary 구간 무시, 스택 깊이 상한.
TEST(Progress_BindingStackOrderAndSecondary) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressTaskHandle first = hub.Begin("First");
    ProgressTaskHandle second = hub.Begin("Second");
    ProgressTaskHandle third = hub.Begin("Third");
    {
        auto outer = ProgressBinding::Bind(first.Id(), ProgressBindingRole::Primary);
        REQUIRE(outer);
        {
            auto inner = ProgressBinding::Bind(second.Id(), ProgressBindingRole::Primary);
            REQUIRE(inner);
            ProgressScope scope(2.0);
            scope.EnterStep(1.0);
            scope.EnterStep(1.0);
            CHECK(Near(hub.Visible(second.Id()), 0.5));
            CHECK(hub.Shape(first.Id()) == ProgressShape::Indeterminate);
        }

        {
            auto secondary = ProgressBinding::Bind(third.Id(), ProgressBindingRole::Secondary);
            REQUIRE(secondary);
            ProgressScope scope(2.0);
            scope.EnterStep(1.0);
            scope.EnterStep(1.0);
            auto counter = scope.MakeCounter(3);
            REQUIRE(counter);
            CHECK(!counter.Value().IsActive());
        }

        CHECK(hub.Shape(third.Id()) == ProgressShape::Indeterminate);
        ProgressScope scope(4.0);
        scope.EnterStep(1.0);
        scope.EnterStep(1.0);
        CHECK(Near(hub.Visible(first.Id()), 0.25));
    }

    // 깊이 상한: 같은 작업 Secondary를 상한까지 쌓고 하나 더.
    CHECK(BindNested(third.Id(), MaxProgressBindingDepth) == ErrorCode::CapacityExceeded);

    CHECK(first.Finish(ProgressOutcome::Succeeded));
    CHECK(second.Finish(ProgressOutcome::Succeeded));
    CHECK(third.Finish(ProgressOutcome::Succeeded));
}

// 시험 6: handle을 다른 스레드에서 Finish해도 이 스레드에 매달린 바인딩이 없다(자동 바인딩 없음).
TEST(Progress_FinishOnAnotherThreadLeavesNoBinding) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressTaskHandle task = hub.Begin("Moved");
    FinishAttempt attempt;
    attempt.task = &task;
    REQUIRE(test::RunOnOsThread(FinishOnThread, &attempt));
    CHECK(attempt.finished);
    ProgressTaskHandle next = hub.Begin("Next");
    {
        ProgressScope scope(2.0);
        scope.EnterStep(1.0);
        scope.EnterStep(1.0);
    }

    CHECK(hub.Shape(next.Id()) == ProgressShape::Indeterminate);
    CHECK(next.Finish(ProgressOutcome::Succeeded));
}

// 시험 8: 구간 계수는 그 구간 몫만 채우고, 구간이 끝난 뒤의 Add는 무시되며, 둘째 계수는
// AlreadyExists다.
TEST(Progress_ScopeCounterFillsOnlyItsStep) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressTaskHandle task = hub.Begin("Counter");
    {
        auto binding = ProgressBinding::Bind(task.Id(), ProgressBindingRole::Primary);
        REQUIRE(binding);
        ProgressScope scope(4.0);
        scope.EnterStep(1.0);
        scope.EnterStep(2.0);
        auto counter = scope.MakeCounter(4);
        REQUIRE(counter);
        CHECK(scope.MakeCounter(2).Error() == ErrorCode::AlreadyExists);
        counter.Value().Add(2);
        CHECK(Near(hub.Visible(task.Id()), 0.25 + 0.5 * 0.5));
        counter.Value().AddTotal(4);
        counter.Value().Add(10);
        CHECK(Near(hub.Visible(task.Id()), 0.75));
        scope.EnterStep(1.0);
        counter.Value().Add(100);
        CHECK(Near(hub.Visible(task.Id()), 0.75));
        counter = Result<ProgressCounter>::Success(ProgressCounter());
        auto next = scope.MakeCounter(2);
        REQUIRE(next);
        next.Value().Add();
        CHECK(Near(hub.Visible(task.Id()), 0.875));
    }

    CHECK(task.Finish(ProgressOutcome::Succeeded));
}

// 시험 6·6.2절: Primary 스택은 깊이별 문구를 발행하고 한 줄 문구는 가장 안쪽 문구다.
TEST(Progress_DepthMessagesFollowSteps) {
    Hub hub;
    REQUIRE(hub.ready);
    const Text build = hub.MakeText("Building mesh");
    const Text weld = hub.MakeText("Welding vertices");
    const Text status = hub.MakeText("Status");
    ProgressTaskHandle task = hub.Begin("Mesh");
    CHECK(task.SetMessage(status));
    {
        auto binding = ProgressBinding::Bind(task.Id(), ProgressBindingRole::Primary);
        REQUIRE(binding);
        ProgressScope scope(3.0, build);
        scope.EnterStep(1.0, weld);
        {
            ProgressSnapshot snapshot(hub.allocator);
            const ProgressTaskView* view = nullptr;
            REQUIRE(hub.View(snapshot, task.Id(), view));
            CHECK(view->messageCount == 1);
            CHECK(hub.SameSource(view->messages[0], "Welding vertices"));
            CHECK(hub.SameSource(view->DisplayMessage(), "Welding vertices"));
            CHECK(hub.SameSource(view->taskMessage, "Status"));
        }

        scope.EnterStep(2.0);
        {
            ProgressScope inner(1.0);
            ProgressSnapshot snapshot(hub.allocator);
            const ProgressTaskView* view = nullptr;
            REQUIRE(hub.View(snapshot, task.Id(), view));
            CHECK(view->messageCount == 2);
            CHECK(hub.SameSource(view->messages[0], "Building mesh"));
            CHECK(!view->messages[1].IsValid());
            CHECK(hub.SameSource(view->DisplayMessage(), "Building mesh"));
        }
    }

    ProgressSnapshot snapshot(hub.allocator);
    const ProgressTaskView* view = nullptr;
    REQUIRE(hub.View(snapshot, task.Id(), view));
    CHECK(view->messageCount == 0);
    CHECK(hub.SameSource(view->DisplayMessage(), "Status"));
    CHECK(task.Finish(ProgressOutcome::Succeeded));
}

struct PumpProbe {
    FakeClock* clock = nullptr;
    ProgressScope* scope = nullptr;
    Allocator* allocator = nullptr;
    uint32 calls = 0;
    uint32 snapshots = 0;
};

void CountPump(void* context) noexcept {
    auto& probe = *static_cast<PumpProbe*>(context);
    ++probe.calls;
    // 실제 펌프처럼 스냅샷을 당긴다(보고 경로가 hub 잠금을 놓은 뒤 부르므로 교착이 없다).
    if (probe.allocator != nullptr) {
        ProgressSnapshot snapshot(*probe.allocator);
        uint64 cursor = 0;
        if (ProgressHub::Get().Snapshot(snapshot, cursor)) {
            ++probe.snapshots;
        }
    }

    // 펌프 안에서 시계를 넘기고 다시 보고해도 재진입하지 않는다.
    if (probe.scope != nullptr) {
        probe.clock->now += PumpIntervalNanoseconds * 10;
        probe.scope->Tick();
    }
}

struct CounterOnThread {
    ProgressCounter* counter = nullptr;
};

void AddOnThread(void* context) noexcept {
    static_cast<CounterOnThread*>(context)->counter->Add();
}

void SetPumpOnThread(void* context) noexcept {
    auto& attempt = *static_cast<BindAttempt*>(context);
    ProgressUiPump pump;
    pump.pump = CountPump;
    attempt.error = ProgressHub::Get().SetUiPump(pump).Error();
}

// 시험 14: 가짜 시계로 0.2 s 경계(직전·정확히·직후), 등록 스레드만 펌프, 펌프 안 재진입 없음.
TEST(Progress_UiPumpRunsOnlyOnTheUiThreadAtTheInterval) {
    Hub hub;
    REQUIRE(hub.ready);
    PumpProbe probe;
    probe.clock = &hub.clock;
    probe.allocator = &hub.allocator;
    ProgressUiPump pump;
    pump.pump = CountPump;
    pump.context = &probe;
    CHECK(ProgressHub::Get().SetUiPump(ProgressUiPump{}).Error() == ErrorCode::InvalidArgument);
    REQUIRE(ProgressHub::Get().SetUiPump(pump));
    BindAttempt other;
    REQUIRE(test::RunOnOsThread(SetPumpOnThread, &other));
    CHECK(other.error == ErrorCode::AlreadyExists);
    ProgressTaskHandle task = hub.Begin("Pump");
    {
        auto binding = ProgressBinding::Bind(task.Id(), ProgressBindingRole::Primary);
        REQUIRE(binding);
        ProgressScope scope(100.0);
        hub.clock.now = PumpIntervalNanoseconds - 1;
        scope.EnterStep(1.0);
        CHECK(probe.calls == 0);
        hub.clock.now = PumpIntervalNanoseconds;
        scope.EnterStep(1.0);
        CHECK(probe.calls == 1);
        CHECK(probe.snapshots == 1);
        // 직전·정확히·직후(마지막 펌프 0.2 s 기준).
        hub.clock.now = PumpIntervalNanoseconds * 2 - 1;
        scope.Tick();
        CHECK(probe.calls == 1);
        hub.clock.now = PumpIntervalNanoseconds * 2;
        scope.Tick();
        CHECK(probe.calls == 2);
        hub.clock.now = PumpIntervalNanoseconds * 3 + 1;
        scope.Tick();
        CHECK(probe.calls == 3);

        // 펌프 안의 보고는 재진입하지 않는다.
        probe.scope = &scope;
        hub.clock.now += PumpIntervalNanoseconds;
        scope.Tick();
        CHECK(probe.calls == 4);
        probe.scope = nullptr;

        // 다른 스레드의 계수 Add는 펌프하지 않는다.
        auto counter = ProgressCounter::ForTask(task, 10);
        REQUIRE(counter);
        hub.clock.now += PumpIntervalNanoseconds * 100;
        CounterOnThread work;
        work.counter = &counter.Value();
        REQUIRE(test::RunOnOsThread(AddOnThread, &work));
        CHECK(probe.calls == 4);
        counter.Value().Add();
        CHECK(probe.calls == 5);

        ProgressHub::Get().ClearUiPump();
        hub.clock.now += PumpIntervalNanoseconds * 100;
        scope.Tick();
        CHECK(probe.calls == 5);
    }

    CHECK(task.Finish(ProgressOutcome::Succeeded));
}

// 6.5절: ProgressParallelFor는 호출 스레드 바인딩을 떼고 원소마다 계수를 올린다 — 조각 안의 구간이
// Primary 문구·진행을 흐리지 않는다.
TEST(Progress_ParallelForReportsThroughTheCounter) {
    Hub hub;
    REQUIRE(hub.ready);
    WorkerScheduler scheduler(hub.allocator);
    WorkerSchedulerConfig config;
    config.workerCount = 2;
    REQUIRE(scheduler.Start(config));
    const Text compile = hub.MakeText("Compiling");
    const Text piece = hub.MakeText("Piece");
    ProgressTaskHandle task = hub.Begin("Parallel");
    constexpr usize Count = 64;
    platform::Atomic<uint32> visited{0};
    {
        auto binding = ProgressBinding::Bind(task.Id(), ProgressBindingRole::Primary);
        REQUIRE(binding);
        ProgressScope scope(1.0);
        scope.EnterStep(1.0, compile);
        auto counter = scope.MakeCounter(Count);
        REQUIRE(counter);
        auto done = ProgressParallelFor(
            scheduler, hub.allocator, Count, counter.Value(), [&](usize index) noexcept {
                (void)index;
                ProgressScope inner(2.0, piece);
                inner.EnterStep(1.0, piece);
                (void)visited.FetchAdd<platform::MemoryOrder::Relaxed>(1u);
            });
        CHECK(done);
        CHECK(visited.Load<platform::MemoryOrder::Relaxed>() == Count);
        ProgressSnapshot snapshot(hub.allocator);
        const ProgressTaskView* view = nullptr;
        REQUIRE(hub.View(snapshot, task.Id(), view));
        CHECK(Near(view->fraction, 1.0));
        CHECK(view->messageCount == 1);
        CHECK(hub.SameSource(view->messages[0], "Compiling"));
    }

    CHECK(task.Finish(ProgressOutcome::Succeeded));
    REQUIRE(scheduler.Shutdown());
}

} // namespace progress_scope_tests_file
