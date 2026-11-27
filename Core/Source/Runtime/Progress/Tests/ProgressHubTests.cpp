#include "../Private/ProgressTestAccess.h"

#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Localization/Text.h>
#include <Progress/ProgressBinding.h>
#include <Progress/ProgressCounter.h>
#include <Progress/ProgressHub.h>
#include <Platform/PlatformMath.h>
#include <Progress/ProgressScope.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/OsThread.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace progress_hub_tests_file {

struct FakeClock {
    uint64 now = 0;
};

uint64 ReadFakeClock(void* context) noexcept {
    return static_cast<FakeClock*>(context)->now;
}

constexpr double Tolerance = 1e-12;

bool Near(double left, double right) noexcept {
    const double difference = left - right;
    return difference < Tolerance && difference > -Tolerance;
}

struct Hub {
    SystemAllocator allocator;
    FakeClock clock;
    bool ready = false;

    explicit Hub(uint32 eventCapacity = DefaultProgressEventLogCapacity) noexcept {
        ProgressHubSettings settings;
        settings.clock = ReadFakeClock;
        settings.clockContext = &clock;
        settings.eventLogCapacity = eventCapacity;
        ready = static_cast<bool>(ProgressHub::Get().Initialize(allocator, settings));
    }

    ~Hub() noexcept {
        ProgressHub::Get().Shutdown();
    }

    Text MakeText(StringView source) noexcept {
        auto text = Text::Invariant(allocator, source);
        return text ? Move(text.Value()) : Text();
    }

    Result<ProgressTaskHandle> Begin(ProgressTaskDesc desc, StringView title) noexcept {
        const Text text = MakeText(title);
        desc.title = &text;
        return ProgressHub::Get().Begin(desc);
    }

    ProgressTaskHandle BeginChild(StringView title, ProgressTaskId parent, double weight = 1.0,
                                  ProgressCancel cancel = ProgressCancel::NotCancellable) noexcept {
        ProgressTaskDesc desc;
        desc.parent = parent;
        desc.weightInParent = weight;
        desc.cancel = cancel;
        auto task = Begin(desc, title);
        return task ? Move(task.Value()) : ProgressTaskHandle();
    }

    const ProgressTaskView* Find(const ProgressSnapshot& snapshot, ProgressTaskId id) noexcept {
        for (const ProgressTaskView& task : snapshot.tasks) {
            if (task.id == id) {
                return &task;
            }
        }

        return nullptr;
    }

    double Visible(ProgressTaskId id) noexcept {
        ProgressSnapshot snapshot(allocator);
        uint64 cursor = 0;
        if (!ProgressHub::Get().Snapshot(snapshot, cursor)) {
            return -1.0;
        }

        const ProgressTaskView* view = Find(snapshot, id);
        return view != nullptr ? view->fraction : -1.0;
    }
};

struct CounterOnThread {
    ProgressCounter* counter = nullptr;
};

void AddOneOnThread(void* context) noexcept {
    static_cast<CounterOnThread*>(context)->counter->Add(1);
}

// 시험 7: 여러 스레드 Add(스레드 끝을 기다려 순서 고정), AddTotal로 총량이 늘어도 D는 단조, done >
// total 순간 값은 잘린다.
TEST(Progress_TaskCounterFromThreadsIsMonotonic) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressTaskHandle task = hub.BeginChild("Shaders", NoProgressTask);
    auto counter = ProgressCounter::ForTask(task, 4);
    REQUIRE(counter);
    CHECK(ProgressCounter::ForTask(task, 4).Error() == ErrorCode::AlreadyExists);
    CounterOnThread work;
    work.counter = &counter.Value();
    REQUIRE(test::RunOnOsThread(AddOneOnThread, &work));
    REQUIRE(test::RunOnOsThread(AddOneOnThread, &work));
    {
        ProgressSnapshot snapshot(hub.allocator);
        uint64 cursor = 0;
        REQUIRE(ProgressHub::Get().Snapshot(snapshot, cursor));
        const ProgressTaskView* view = hub.Find(snapshot, task.Id());
        REQUIRE(view != nullptr);
        CHECK(view->shape == ProgressShape::Count);
        CHECK(view->done == 2);
        CHECK(view->total == 4);
        CHECK(Near(view->fraction, 0.5));
    }

    counter.Value().AddTotal(4);
    CHECK(Near(hub.Visible(task.Id()), 0.5));
    counter.Value().Add(4);
    CHECK(Near(hub.Visible(task.Id()), 0.75));
    counter.Value().Add(10);
    {
        ProgressSnapshot snapshot(hub.allocator);
        uint64 cursor = 0;
        REQUIRE(ProgressHub::Get().Snapshot(snapshot, cursor));
        const ProgressTaskView* view = hub.Find(snapshot, task.Id());
        REQUIRE(view != nullptr);
        CHECK(view->done == 8);
        CHECK(view->total == 8);
        CHECK(Near(view->fraction, 1.0));
    }

    CHECK(task.Finish(ProgressOutcome::Succeeded));
}

// 시험 9: selfWeight·weightInParent, 끝난 하위 작업은 1, 늦게 붙는 하위 작업에도 D 단조, 자기 보고
// 없는 부모의 w₀ = 0.
TEST(Progress_SubtasksComposeByWeight) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressTaskHandle parent = hub.BeginChild("Parent", NoProgressTask);
    ProgressTaskHandle child = hub.BeginChild("Child", parent.Id(), 2.0);
    auto childCounter = ProgressCounter::ForTask(child, 4);
    REQUIRE(childCounter);
    childCounter.Value().Add(1);
    {
        auto binding = ProgressBinding::Bind(parent.Id(), ProgressBindingRole::Primary);
        REQUIRE(binding);
        ProgressScope scope(2.0);
        scope.EnterStep(1.0);
        scope.EnterStep(1.0);
        // (1·0.5 + 2·0.25) / (1 + 2)
        CHECK(Near(hub.Visible(parent.Id()), 1.0 / 3.0));
        CHECK(child.Finish(ProgressOutcome::Failed));
        // 끝난 하위 작업(결과 무관)은 1: (0.5 + 2) / 3
        CHECK(Near(hub.Visible(parent.Id()), 2.5 / 3.0));
        ProgressTaskHandle late = hub.BeginChild("Late", parent.Id(), 3.0);
        // (0.5 + 2 + 0) / 6은 작지만 보이는 값은 줄지 않는다.
        CHECK(Near(hub.Visible(parent.Id()), 2.5 / 3.0));
        CHECK(late.Finish(ProgressOutcome::Succeeded));
    }

    ProgressTaskHandle silent = hub.BeginChild("Silent", NoProgressTask);
    ProgressTaskHandle half = hub.BeginChild("Half", silent.Id(), 1.0);
    auto halfCounter = ProgressCounter::ForTask(half, 2);
    REQUIRE(halfCounter);
    halfCounter.Value().Add(1);
    CHECK(Near(hub.Visible(silent.Id()), 0.5));
    CHECK(half.Finish(ProgressOutcome::Succeeded));
    CHECK(Near(hub.Visible(silent.Id()), 1.0));
    CHECK(silent.Finish(ProgressOutcome::Succeeded));
    CHECK(parent.Finish(ProgressOutcome::Succeeded));
}

// 시험 10: Finish 없이 소멸 → Abandoned + Warning, 끝난 뒤 보고 no-op, 부모 끝 → 하위 Abandoned가
// 먼저 기록됨.
TEST(Progress_LifetimeAndEndOrder) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressSnapshot snapshot(hub.allocator);
    uint64 cursor = 0;
    REQUIRE(ProgressHub::Get().Snapshot(snapshot, cursor));
    ProgressTaskHandle parent = hub.BeginChild("Parent", NoProgressTask);
    ProgressTaskHandle child = hub.BeginChild("Child", parent.Id());
    ProgressTaskHandle grandchild = hub.BeginChild("Grandchild", child.Id());
    const Text reason = hub.MakeText("Disk full");
    CHECK(parent.Finish(ProgressOutcome::Failed, reason));
    REQUIRE(ProgressHub::Get().Snapshot(snapshot, cursor));
    REQUIRE(snapshot.events.Size() == 6);
    CHECK(snapshot.events[0].kind == ProgressEventKind::Begin);
    CHECK(snapshot.events[0].id == parent.Id());
    CHECK(snapshot.events[2].parent == child.Id());
    CHECK(snapshot.events[3].kind == ProgressEventKind::End);
    CHECK(snapshot.events[3].id == grandchild.Id());
    CHECK(snapshot.events[3].outcome == ProgressOutcome::Abandoned);
    CHECK(snapshot.events[4].id == child.Id());
    CHECK(snapshot.events[4].outcome == ProgressOutcome::Abandoned);
    CHECK(snapshot.events[5].id == parent.Id());
    CHECK(snapshot.events[5].outcome == ProgressOutcome::Failed);
    CHECK(snapshot.events[5].detail.IsValid());
    CHECK(snapshot.tasks.IsEmpty());

    // 끝난 작업: 보고는 no-op, 조작은 ScopeClosed.
    CHECK(child.Finish(ProgressOutcome::Succeeded).Error() == ErrorCode::ScopeClosed);
    CHECK(child.SetMessage(reason).Error() == ErrorCode::ScopeClosed);
    CHECK(ProgressCounter::ForTask(child, 1).Error() == ErrorCode::ScopeClosed);

    {
        ProgressTaskHandle dropped = hub.BeginChild("Dropped", NoProgressTask);
        auto counter = ProgressCounter::ForTask(dropped, 2);
        REQUIRE(counter);
        dropped = ProgressTaskHandle();
        counter.Value().Add();
        CHECK(ProgressHub::Get().Stats().abandonedTasks == 1);
    }

    REQUIRE(ProgressHub::Get().Snapshot(snapshot, cursor));
    REQUIRE(snapshot.events.Size() == 2);
    CHECK(snapshot.events[1].outcome == ProgressOutcome::Abandoned);
    CHECK(Near(snapshot.events[1].fraction, 0.0));
}

// 시험 10: Shutdown 뒤 handle·계수는 ShuttingDown·no-op이고, 다시 켠 hub에서도 옛 handle은
// ShuttingDown이다. 마지막 참조가 놓이면 기록이 모두 반환된다.
TEST(Progress_ShutdownAbandonsLiveTasks) {
    SystemAllocator allocator;
    Hub hub;
    REQUIRE(hub.ready);
    {
        ProgressTaskHandle task = hub.BeginChild("Live", NoProgressTask);
        ProgressTaskHandle child = hub.BeginChild("Child", task.Id());
        auto counter = ProgressCounter::ForTask(task, 3);
        REQUIRE(counter);
        const Text message = hub.MakeText("After");
        ProgressHub::Get().Shutdown();
        CHECK(ProgressHub::Get().Stats().shutdownAbandoned == 1);
        CHECK(task.SetMessage(message).Error() == ErrorCode::ShuttingDown);
        CHECK(task.Finish(ProgressOutcome::Succeeded).Error() == ErrorCode::ShuttingDown);
        CHECK(ProgressCounter::ForTask(child, 1).Error() == ErrorCode::ShuttingDown);
        counter.Value().Add();
        ProgressSnapshot snapshot(hub.allocator);
        uint64 cursor = 0;
        CHECK(ProgressHub::Get().Snapshot(snapshot, cursor).Error() == ErrorCode::ShuttingDown);
        CHECK(ProgressBinding::Bind(task.Id(), ProgressBindingRole::Primary).Error() ==
              ErrorCode::ShuttingDown);
        CHECK(ProgressHub::Get().Initialize(allocator, {}));
        CHECK(task.Finish(ProgressOutcome::Succeeded).Error() == ErrorCode::ShuttingDown);
        CHECK(ProgressHub::Get().Stats().abandonedTasks == 0);
        ProgressHub::Get().Shutdown();
    }

    // 마지막 handle·계수가 놓이면 첫 켜짐의 기록과 문구가 모두 반환된다(live·ring은 Shutdown이
    // 놓음).
    CHECK(hub.allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 시험 10: 바인딩을 쥔 채 작업이 끝나면 그 뒤 구간 보고는 아무 일도 하지 않는다(입력 검사 Warning도
// 없다). 끝 사건의 비율은 마지막 스냅샷 뒤의 보고까지 담는다.
TEST(Progress_ReportsAfterFinishAreNoOps) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressSnapshot snapshot(hub.allocator);
    uint64 cursor = 0;
    REQUIRE(ProgressHub::Get().Snapshot(snapshot, cursor));
    ProgressTaskHandle task = hub.BeginChild("Finished early", NoProgressTask);
    {
        auto binding = ProgressBinding::Bind(task.Id(), ProgressBindingRole::Primary);
        REQUIRE(binding);
        ProgressScope scope(4.0);
        scope.EnterStep(3.0);
        scope.EnterStep(1.0);
        CHECK(task.Finish(ProgressOutcome::Failed));
        scope.EnterStep(platform::PlatformMath::QuietNaN<double>());
        ProgressScope late(2.0);
        late.EnterStep(-1.0);
        auto counter = late.MakeCounter(3);
        REQUIRE(counter);
        CHECK(!counter.Value().IsActive());
        CHECK(!late.IsReporting());
    }

    CHECK(ProgressHub::Get().Stats().invalidWorkWarnings == 0);
    REQUIRE(ProgressHub::Get().Snapshot(snapshot, cursor));
    REQUIRE(snapshot.events.Size() == 2);
    CHECK(snapshot.events[1].outcome == ProgressOutcome::Failed);
    CHECK(Near(snapshot.events[1].fraction, 0.75));
}

// 시험 11: 오류 코드 표(5.2절).
TEST(Progress_ErrorCodeTable) {
    {
        // 켜기 전.
        ProgressTestAccess::ReturnToUninitialized();
        SystemAllocator allocator;
        auto title = Text::Invariant(allocator, "Early");
        REQUIRE(title);
        ProgressTaskDesc early;
        early.title = &title.Value();
        CHECK(ProgressHub::Get().Begin(early).Error() == ErrorCode::NotReady);
        ProgressSnapshot snapshot(allocator);
        uint64 cursor = 0;
        CHECK(ProgressHub::Get().Snapshot(snapshot, cursor).Error() == ErrorCode::NotReady);
        CHECK(ProgressBinding::Bind(1, ProgressBindingRole::Primary).Error() ==
              ErrorCode::NotReady);
        CHECK(ProgressHub::Get().RequestCancel(1).Error() == ErrorCode::NotReady);
    }

    Hub hub;
    REQUIRE(hub.ready);
    SystemAllocator other;
    CHECK(ProgressHub::Get().Initialize(other, {}).Error() == ErrorCode::AlreadyExists);
    ProgressHubSettings zero;
    zero.eventLogCapacity = 0;
    CHECK(ProgressHub::Get().Initialize(other, zero).Error() == ErrorCode::InvalidArgument);

    const auto beginError = [&](ProgressTaskDesc desc, StringView title) noexcept {
        auto task = hub.Begin(desc, title);
        return task ? ErrorCode::None : task.Error();
    };
    ProgressTaskDesc desc;
    CHECK(ProgressHub::Get().Begin(desc).Error() == ErrorCode::InvalidArgument);
    const Text invalid;
    desc.title = &invalid;
    CHECK(ProgressHub::Get().Begin(desc).Error() == ErrorCode::InvalidArgument);
    CHECK(beginError({}, "") == ErrorCode::InvalidArgument);
    const double nan = platform::PlatformMath::QuietNaN<double>();
    const double infinity = platform::PlatformMath::Infinity<double>();
    for (const double weight : {0.0, -1.0, nan, infinity}) {
        ProgressTaskDesc bad;
        bad.weightInParent = weight;
        CHECK(beginError(bad, "Task") == ErrorCode::InvalidArgument);
    }

    for (const double weight : {-1.0, nan, infinity}) {
        ProgressTaskDesc bad;
        bad.selfWeight = weight;
        CHECK(beginError(bad, "Task") == ErrorCode::InvalidArgument);
        bad = {};
        bad.modalDelaySeconds = weight;
        CHECK(beginError(bad, "Task") == ErrorCode::InvalidArgument);
    }

    ProgressTaskDesc zeroSelf;
    zeroSelf.selfWeight = 0.0;
    auto zeroSelfTask = hub.Begin(zeroSelf, "Zero self weight");
    REQUIRE(zeroSelfTask);

    ProgressTaskHandle parent = hub.BeginChild("Parent", NoProgressTask);
    {
        auto external = CancelSource::Create(hub.allocator);
        REQUIRE(external);
        ProgressTaskDesc both;
        both.parent = parent.Id();
        both.externalCancel = external.Value().Token();
        CHECK(beginError(both, "Child") == ErrorCode::InvalidArgument);
    }

    ProgressTaskDesc missing;
    missing.parent = parent.Id() + 1000;
    CHECK(beginError(missing, "Child") == ErrorCode::NotFound);
    ProgressTaskHandle ended = hub.BeginChild("Ended", NoProgressTask);
    CHECK(ended.Finish(ProgressOutcome::Succeeded));
    ProgressTaskDesc stale;
    stale.parent = ended.Id();
    CHECK(beginError(stale, "Child") == ErrorCode::StaleHandle);
    CHECK(ProgressHub::Get().RequestCancel(ended.Id()).Error() == ErrorCode::StaleHandle);
    CHECK(ProgressHub::Get().RequestCancel(parent.Id() + 1000).Error() == ErrorCode::NotFound);
    CHECK(ProgressHub::Get().RequestCancel(parent.Id()).Error() == ErrorCode::Unsupported);
    CHECK(ProgressTaskHandle().Finish(ProgressOutcome::Succeeded).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(ProgressTaskHandle().SetMessage(invalid).Error() == ErrorCode::InvalidArgument);
    CHECK(ProgressCounter::ForTask(ProgressTaskHandle(), 1).Error() == ErrorCode::InvalidArgument);

    // 살아 있는 작업 수 초과: 이미 둘(zeroSelf·parent)이 살아 있다.
    {
        Array<ProgressTaskHandle> tasks(hub.allocator);
        REQUIRE(tasks.TryReserve(MaxLiveProgressTasks));
        const Text title = hub.MakeText("Many");
        ProgressTaskDesc many;
        many.title = &title;
        for (uint32 index = 2; index < MaxLiveProgressTasks; ++index) {
            auto task = ProgressHub::Get().Begin(many);
            REQUIRE(task);
            REQUIRE(tasks.TryPushBack(Move(task.Value())));
        }

        CHECK(ProgressHub::Get().Begin(many).Error() == ErrorCode::CapacityExceeded);
        for (ProgressTaskHandle& task : tasks) {
            CHECK(task.Finish(ProgressOutcome::Succeeded));
        }
    }

    CHECK(zeroSelfTask.Value().Finish(ProgressOutcome::Succeeded));
    CHECK(parent.Finish(ProgressOutcome::Succeeded));
    ProgressHub::Get().Shutdown();
    CHECK(beginError({}, "Late") == ErrorCode::ShuttingDown);
}

// 시험 11: 할당 실패는 그 오류이고 새는 것이 없다(Begin의 모든 할당 지점).
TEST(Progress_BeginAllocationFailuresLeakNothing) {
    SystemAllocator system;
    {
        test::NFailAllocator allocator(system);
        REQUIRE(ProgressHub::Get().Initialize(allocator, {}));
        auto title = Text::Invariant(system, "A title long enough to need heap storage for text");
        REQUIRE(title);
        ProgressTaskDesc desc;
        desc.title = &title.Value();
        desc.cancel = ProgressCancel::UserCancellable;
        // Begin의 할당 지점(원문 해석·취소 source·기록)보다 넉넉한 시도 수다.
        constexpr uint64 MaxAttempts = 16;
        bool succeeded = false;
        for (uint64 attempt = 1; attempt < MaxAttempts && !succeeded; ++attempt) {
            allocator.FailOnAttempt(attempt);
            auto task = ProgressHub::Get().Begin(desc);
            if (task) {
                succeeded = true;
                CHECK(task.Value().Finish(ProgressOutcome::Succeeded));
            } else {
                CHECK(task.Error() == ErrorCode::OutOfMemory);
            }
        }

        CHECK(succeeded);
        allocator.DisableFailure();
        ProgressHub::Get().Shutdown();
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 시험 12: 두 소비자가 각자 cursor로 같은 사건을 받고, 첫 호출(cursor 0)은 지금부터, 고리 넘침은
// lostEvents다.
TEST(Progress_EventLogCursorsPerConsumer) {
    constexpr uint32 Capacity = 4;
    Hub hub(Capacity);
    REQUIRE(hub.ready);
    ProgressTaskHandle early = hub.BeginChild("Early", NoProgressTask);
    ProgressSnapshot first(hub.allocator);
    ProgressSnapshot second(hub.allocator);
    uint64 firstCursor = 0;
    uint64 secondCursor = 0;
    REQUIRE(ProgressHub::Get().Snapshot(first, firstCursor));
    REQUIRE(ProgressHub::Get().Snapshot(second, secondCursor));
    CHECK(first.events.IsEmpty());
    CHECK(first.tasks.Size() == 1);

    CHECK(early.Finish(ProgressOutcome::Succeeded));
    ProgressTaskHandle task = hub.BeginChild("Task", NoProgressTask);
    REQUIRE(ProgressHub::Get().Snapshot(first, firstCursor));
    REQUIRE(ProgressHub::Get().Snapshot(second, secondCursor));
    REQUIRE(first.events.Size() == 2);
    REQUIRE(second.events.Size() == 2);
    CHECK(first.events[0].kind == ProgressEventKind::End);
    CHECK(Near(first.events[0].fraction, 1.0));
    CHECK(second.events[1].kind == ProgressEventKind::Begin);
    CHECK(second.events[1].id == task.Id());
    CHECK(first.events[0].serial + 1 == first.events[1].serial);

    // 넘침: 사건 6개(시작·끝 셋) 가운데 고리 4칸만 남는다.
    for (uint32 index = 0; index < 3; ++index) {
        ProgressTaskHandle burst = hub.BeginChild("Burst", NoProgressTask);
        CHECK(burst.Finish(ProgressOutcome::Succeeded));
    }

    REQUIRE(ProgressHub::Get().Snapshot(first, firstCursor));
    CHECK(first.lostEvents == 2);
    CHECK(first.events.Size() == Capacity);
    REQUIRE(ProgressHub::Get().Snapshot(first, firstCursor));
    CHECK(first.lostEvents == 0);
    CHECK(first.events.IsEmpty());
    CHECK(task.Finish(ProgressOutcome::Succeeded));
}

// 시험 13: RequestCancel → 작업·하위 작업 token, RequestCancelAllTopLevel, 외부 token.
TEST(Progress_CancellationReachesTasksAndSubtasks) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressTaskDesc cancellable;
    cancellable.cancel = ProgressCancel::UserCancellable;
    auto parent = hub.Begin(cancellable, "Parent");
    REQUIRE(parent);
    ProgressTaskHandle plainChild = hub.BeginChild("Plain child", parent.Value().Id());
    ProgressTaskHandle ownChild =
        hub.BeginChild("Own child", parent.Value().Id(), 1.0, ProgressCancel::UserCancellable);
    auto other = hub.Begin(cancellable, "Other");
    REQUIRE(other);
    ProgressTaskHandle fixed = hub.BeginChild("Fixed", NoProgressTask);
    CHECK(!fixed.Token().CanBeCancelled());

    // 하위 작업 쪽 취소는 부모에 닿지 않는다.
    CHECK(ProgressHub::Get().RequestCancel(ownChild.Id()));
    CHECK(ownChild.Token().IsCancellationRequested());
    CHECK(!parent.Value().Token().IsCancellationRequested());

    CHECK(ProgressHub::Get().RequestCancel(parent.Value().Id()));
    CHECK(parent.Value().Token().IsCancellationRequested());
    CHECK(plainChild.Token().CheckCancelled().Error() == ErrorCode::Cancelled);
    CHECK(!other.Value().Token().IsCancellationRequested());
    {
        auto binding = ProgressBinding::Bind(plainChild.Id(), ProgressBindingRole::Secondary);
        REQUIRE(binding);
        ProgressScope scope(1.0);
        CHECK(scope.IsCancellationRequested());
        CHECK(scope.CheckCancelled().Error() == ErrorCode::Cancelled);
    }

    {
        ProgressSnapshot snapshot(hub.allocator);
        uint64 cursor = 0;
        REQUIRE(ProgressHub::Get().Snapshot(snapshot, cursor));
        const ProgressTaskView* view = hub.Find(snapshot, plainChild.Id());
        REQUIRE(view != nullptr);
        CHECK(!view->cancellable);
        CHECK(view->cancelRequested);
    }

    ProgressHub::Get().RequestCancelAllTopLevel();
    CHECK(other.Value().Token().IsCancellationRequested());
    CHECK(!fixed.Token().IsCancellationRequested());

    // 외부 token: 취소 불가 최상위 작업의 token은 외부 token 그대로다.
    auto external = CancelSource::Create(hub.allocator);
    REQUIRE(external);
    ProgressTaskDesc linked;
    linked.externalCancel = external.Value().Token();
    auto follower = hub.Begin(linked, "Follower");
    REQUIRE(follower);
    linked.cancel = ProgressCancel::UserCancellable;
    auto owner = hub.Begin(linked, "Owner");
    REQUIRE(owner);
    CHECK(!follower.Value().Token().IsCancellationRequested());
    external.Value().Cancel();
    CHECK(follower.Value().Token().IsCancellationRequested());
    CHECK(owner.Value().Token().IsCancellationRequested());

    CHECK(parent.Value().Finish(ProgressOutcome::Cancelled));
    CHECK(other.Value().Finish(ProgressOutcome::Cancelled));
    CHECK(fixed.Finish(ProgressOutcome::Succeeded));
    CHECK(follower.Value().Finish(ProgressOutcome::Cancelled));
    CHECK(owner.Value().Finish(ProgressOutcome::Cancelled));
}

} // namespace progress_hub_tests_file
